/*
 * Y2JB Autoloader X Updater - native PS5 ELF
 *
 * Replaces a YouTube app's download0.dat with the latest Y2JB Autoloader X
 * release. Like the WebKit installer, it starts a temporary HTTP server on
 * 127.0.0.1, opens the PS5 browser on its page, and lets the user pick which
 * YouTube region (PPSA01650 / PPSA01651 / PPSA01652) to update. The page
 * shows what each region holds now (Y2JB X version, upstream Y2JB, nothing).
 *
 * The new image is downloaded next to the old one (download0.dat.x-new),
 * checked against the release's size and SHA-256, then renamed over
 * download0.dat in one step, so a failed download never leaves a broken file.
 */

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/param.h>
#include <sys/stat.h>
#ifdef HOST_TEST
#include <sys/vfs.h>
#else
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#endif
#include <time.h>
#include <unistd.h>

#include "sha256.h"
#include "assets_index_html.h"
#include "assets_jb_png.h"
#include "assets_cacert_pem.h"

#ifndef UPDATER_VERSION
#define UPDATER_VERSION "0.0.0-dev"
#endif

#define UPDATER_NAME     "Y2JB Autoloader X Updater"
#define THREAD_NAME      "y2jbx_updater"
#define PORT             18383
#define RELEASE_API      "https://api.github.com/repos/bsk193/ps5-y2jb-autoloader-x/releases/latest"
#define IMAGE_NAME       "download0.dat"
#define MAX_IMAGE_SIZE   (768ull * 1024 * 1024)
#define IDLE_EXIT_SEC    (15 * 60)
#define STR2(x) #x
#define STR(x) STR2(x)
#define HOST_A "127.0.0.1:" STR(PORT)
#define HOST_B "localhost:" STR(PORT)

/* Host test builds (tests/host_test.sh) put /user under a temporary folder. */
#ifndef ROOT_PREFIX
#define ROOT_PREFIX ""
#endif

/* Read once in main() so --gc-sections keeps it (`used` only stops the compiler). */
__attribute__((used)) volatile const char y2jbx_updater_sig[] =
    "Y2JBX_UPDATER_VER:" UPDATER_VERSION;

/* ── PS5 services ───────────────────────────────────────────────────── */

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

#ifdef HOST_TEST
static int sceKernelSendNotificationRequest(int d, notify_request_t *r, size_t s, int u) { (void)d; (void)r; (void)s; (void)u; return 0; }
static int sceSystemServiceLaunchWebBrowser(const char *uri) { printf("[Y2JBXU] (host) browser: %s\n", uri); return 0; }
static int sceNetCtlInit(void) { return 0; }
static int sceUserServiceInitialize(void *p) { (void)p; return 0; }
#else
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int unused);
int sceSystemServiceLaunchWebBrowser(const char *uri);
int sceNetCtlInit(void);
int sceUserServiceInitialize(void *);
#endif

static void notify(const char *fmt, ...) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    printf("[Y2JBXU] notify: %s\n", req.message);
}

/* ── State shared between the HTTP threads and the workers ────────── */

typedef struct {
    const char *tid;
    const char *region;
    int scanned;          /* 0 = not yet, 1 = done */
    int app_installed;    /* /user/app/<tid> exists */
    int has_image;        /* /user/download/<tid>/download0.dat exists */
    char kind[12];        /* none | x | upstream | unknown */
    char version[48];     /* highest autoloader_version found in the image */
} Region;

static Region regions[] = {
    { "PPSA01650", "US" },
    { "PPSA01651", "EU" },
    { "PPSA01652", "JP" },
};
#define REGION_COUNT (int)(sizeof(regions) / sizeof(regions[0]))

enum { JOB_IDLE, JOB_RELEASE, JOB_DOWNLOAD, JOB_VERIFY, JOB_INSTALL, JOB_DONE, JOB_ERROR };
static const char *job_names[] = { "idle", "release", "download", "verify", "install", "done", "error" };
#define JOB_BUSY(s) ((s) >= JOB_RELEASE && (s) <= JOB_INSTALL)

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static int job_state = JOB_IDLE;
static char job_tid[16];
static char job_message[256];
static unsigned long long job_bytes, job_total;
static char latest_tag[64];
static char latest_url[512];
static unsigned long long latest_size;
static char latest_digest[65];
static char latest_error[200];
static int latest_state; /* 0 = loading, 1 = ok, 2 = error */

static atomic_int keep_running = 1;
/* Per-launch secret: in the URL the browser is opened on (#t=...), and required
 * on every state-changing request, so another page can't drive the updater even
 * if it gets same-origin through DNS rebinding. */
static char session_token[33];
static atomic_long last_request;

#define LOG_LINES 40
static char log_ring[LOG_LINES][200];
static int log_head, log_count;

static void ulog(const char *fmt, ...) {
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("[Y2JBXU] %s\n", line);
    pthread_mutex_lock(&lock);
    snprintf(log_ring[(log_head + log_count) % LOG_LINES], sizeof(log_ring[0]), "%s", line);
    if (log_count < LOG_LINES) log_count++;
    else log_head = (log_head + 1) % LOG_LINES;
    pthread_mutex_unlock(&lock);
}

static void set_job(int state, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&lock);
    job_state = state;
    snprintf(job_message, sizeof(job_message), "%s", msg);
    pthread_mutex_unlock(&lock);
    ulog("%s", msg);
}

static Region *find_region(const char *tid) {
    for (int i = 0; i < REGION_COUNT; i++)
        if (tid && !strcmp(regions[i].tid, tid)) return &regions[i];
    return NULL;
}

/* ── Image inspection ───────────────────────────────────────────────
 * download0.dat is a UFS2 image; Y2JB's main.js sits in it uncompressed.
 * Y2JB X's main.js has `const upstream_version = "` (upstream's does not),
 * and both have `const autoloader_version = "vX"`. An in-place zip update
 * can leave older copies in freed blocks, so keep the highest version seen. */

static int compare_version(const char *a, const char *b) {
    if (*a == 'v') a++;
    if (*b == 'v') b++;
    for (int i = 0; i < 3; i++) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y) return x < y ? -1 : 1;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
    }
    return 0;
}

static void scan_image(Region *r) {
    char path[300], app[256];
    struct stat st;
    snprintf(app, sizeof(app), ROOT_PREFIX "/user/app/%s", r->tid);
    snprintf(path, sizeof(path), ROOT_PREFIX "/user/download/%s/" IMAGE_NAME, r->tid);
    int app_installed = stat(app, &st) == 0;
    int has_image = stat(path, &st) == 0 && S_ISREG(st.st_mode);
    char kind[12] = "none", version[48] = "";

    if (has_image) {
        static const char ver_key[] = "const autoloader_version = \"";
        static const char x_key[] = "const upstream_version = \"";
        const size_t klen = sizeof(ver_key) - 1, xlen = sizeof(x_key) - 1;
        const size_t chunk = 4 << 20, overlap = 128;
        unsigned char *buf = malloc(chunk + overlap);
        FILE *fp = fopen(path, "rb");
        int is_x = 0, found = 0;
        if (buf && fp) {
            size_t carry = 0, n;
            while ((n = fread(buf + carry, 1, chunk, fp)) > 0) {
                size_t len = carry + n;
                for (size_t i = 0; i + klen + 1 < len; i++) {
                    if (buf[i] != 'c') continue;
                    if (i + xlen <= len && !memcmp(buf + i, x_key, xlen)) is_x = 1;
                    if (memcmp(buf + i, ver_key, klen)) continue;
                    const char *v = (const char *)buf + i + klen;
                    size_t room = len - i - klen, vl = 0;
                    while (vl < room && vl < sizeof(version) - 1 && v[vl] != '"' && v[vl] >= ' ' && v[vl] < 127) vl++;
                    if (vl && vl < room && v[vl] == '"' && v[0] != '@') {
                        char cand[48];
                        memcpy(cand, v, vl);
                        cand[vl] = 0;
                        if (!found || compare_version(cand, version) > 0) snprintf(version, sizeof(version), "%s", cand);
                        found = 1;
                    }
                }
                carry = len < overlap ? len : overlap;
                memmove(buf, buf + len - carry, carry);
            }
        }
        if (fp) fclose(fp);
        free(buf);
        snprintf(kind, sizeof(kind), "%s", is_x ? "x" : found ? "upstream" : "unknown");
    }

    pthread_mutex_lock(&lock);
    r->app_installed = app_installed;
    r->has_image = has_image;
    snprintf(r->kind, sizeof(r->kind), "%s", kind);
    snprintf(r->version, sizeof(r->version), "%s", version);
    r->scanned = 1;
    pthread_mutex_unlock(&lock);
    printf("[Y2JBXU] %s (%s): app=%d image=%d kind=%s version=%s\n",
           r->tid, r->region, app_installed, has_image, kind, version);
}

/* ── HTTPS (libcurl + mbedTLS, CA bundle embedded) ───────────────── */

typedef struct { char *data; size_t len, cap; } MemBuf;

static size_t mem_write(void *ptr, size_t size, size_t nmemb, void *ctx) {
    MemBuf *m = ctx;
    size_t n = size * nmemb;
    if (m->len + n + 1 > m->cap) return 0;
    memcpy(m->data + m->len, ptr, n);
    m->len += n;
    m->data[m->len] = 0;
    return n;
}

static void curl_common(CURL *c, const char *url, struct curl_blob *ca) {
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "y2jb-x-updater/" UPDATER_VERSION);
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, ca);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#endif
}

/* Finds `"key"`, skips whitespace and ':', returns the value start (bounded by end). */
static const char *json_value(const char *from, const char *end, const char *key) {
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pl = strlen(pat);
    const char *p = from;
    while (p && p < end && (p = strstr(p, pat)) && p + pl <= end) {
        const char *v = p + pl;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
        if (v < end && *v == ':') {
            v++;
            while (v < end && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) v++;
            return v < end ? v : NULL;
        }
        p += pl;
    }
    return NULL;
}

static int json_string(const char *v, const char *end, char *out, size_t out_len) {
    if (!v || v >= end || *v != '"') return -1;
    v++;
    size_t n = 0;
    while (v < end && *v != '"' && n + 1 < out_len) {
        if (*v == '\\') return -1; /* not expected in tags/URLs/digests */
        out[n++] = *v++;
    }
    out[n] = 0;
    return (v < end && *v == '"') ? 0 : -1;
}

static void fetch_latest_release(void) {
    pthread_mutex_lock(&lock);
    latest_state = 0;
    latest_error[0] = 0;
    pthread_mutex_unlock(&lock);

    MemBuf m = { malloc(1 << 20), 0, 1 << 20 };
    char err[200] = "";
    char tag[64] = "", url[512] = "", digest[65] = "";
    unsigned long long size = 0;
    struct curl_blob ca = { (void *)assets_cacert_pem, assets_cacert_pem_len, CURL_BLOB_COPY };
    CURL *c = curl_easy_init();
    if (!m.data || !c) {
        snprintf(err, sizeof(err), "out of memory");
    } else {
        struct curl_slist *h = curl_slist_append(NULL, "Accept: application/vnd.github+json");
        curl_common(c, RELEASE_API, &ca);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, mem_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &m);
        CURLcode res = curl_easy_perform(c);
        long status = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_slist_free_all(h);
        if (res != CURLE_OK) {
            snprintf(err, sizeof(err), "GitHub unreachable: %s", curl_easy_strerror(res));
        } else if (status != 200) {
            snprintf(err, sizeof(err), "GitHub answered HTTP %ld", status);
        } else {
            const char *end = m.data + m.len;
            json_string(json_value(m.data, end, "tag_name"), end, tag, sizeof(tag));
            /* The download0.dat asset: its fields run from "name" to
             * "browser_download_url" (the asset object's last field). */
            const char *a = m.data;
            while ((a = json_value(a, end, "name"))) {
                char name[64];
                if (!json_string(a, end, name, sizeof(name)) && !strcmp(name, IMAGE_NAME)) break;
                a++;
            }
            const char *u = a ? json_value(a, end, "browser_download_url") : NULL;
            if (!tag[0] || !u || json_string(u, end, url, sizeof(url)) || strncmp(url, "https://", 8)) {
                snprintf(err, sizeof(err), "latest release has no " IMAGE_NAME);
            } else {
                const char *s = json_value(a, u, "size");
                if (s) size = strtoull(s, NULL, 10);
                char d[80] = "";
                if (!json_string(json_value(a, u, "digest"), u, d, sizeof(d)) &&
                    !strncmp(d, "sha256:", 7) && strlen(d) == 71)
                    snprintf(digest, sizeof(digest), "%s", d + 7);
                if (!size || size > MAX_IMAGE_SIZE)
                    snprintf(err, sizeof(err), "unexpected " IMAGE_NAME " size %llu", size);
            }
        }
    }
    if (c) curl_easy_cleanup(c);
    free(m.data);

    pthread_mutex_lock(&lock);
    if (err[0]) {
        latest_state = 2;
        snprintf(latest_error, sizeof(latest_error), "%s", err);
    } else {
        latest_state = 1;
        snprintf(latest_tag, sizeof(latest_tag), "%s", tag);
        snprintf(latest_url, sizeof(latest_url), "%s", url);
        snprintf(latest_digest, sizeof(latest_digest), "%s", digest);
        latest_size = size;
    }
    pthread_mutex_unlock(&lock);
    if (err[0]) ulog("Release check failed: %s", err);
    else ulog("Latest release: %s (%llu MB%s)", tag, size >> 20, digest[0] ? ", SHA-256 known" : "");
}

/* ── Download + install worker ──────────────────────────────────────── */

typedef struct { FILE *fp; SHA256_CTX sha; unsigned long long bytes, limit; } DlCtx;

static size_t file_write(void *ptr, size_t size, size_t nmemb, void *ctx) {
    DlCtx *d = ctx;
    size_t n = size * nmemb;
    if (d->bytes + n > d->limit) return 0;
    if (fwrite(ptr, 1, n, d->fp) != n) return 0;
    sha256_update(&d->sha, ptr, n);
    d->bytes += n;
    return n;
}

static int progress(void *ctx, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ut, curl_off_t un) {
    (void)ctx; (void)ut; (void)un;
    pthread_mutex_lock(&lock);
    job_bytes = (unsigned long long)dlnow;
    if (dltotal > 0) job_total = (unsigned long long)dltotal;
    pthread_mutex_unlock(&lock);
    return atomic_load(&keep_running) ? 0 : 1;
}

static void *install_worker(void *arg) {
    Region *r = arg;
    char dir[256], tmp[300], dst[300], url[512], tag[64], digest[65], lerr[200];
    unsigned long long size;

    set_job(JOB_RELEASE, "Checking the latest release...");
    fetch_latest_release();
    pthread_mutex_lock(&lock);
    int ok = latest_state == 1;
    snprintf(url, sizeof(url), "%s", latest_url);
    snprintf(tag, sizeof(tag), "%s", latest_tag);
    snprintf(digest, sizeof(digest), "%s", latest_digest);
    snprintf(lerr, sizeof(lerr), "%s", latest_error);
    size = latest_size;
    pthread_mutex_unlock(&lock);
    if (!ok) {
        set_job(JOB_ERROR, "Could not get the latest release: %s", lerr);
        return NULL;
    }

    snprintf(dir, sizeof(dir), ROOT_PREFIX "/user/download/%s", r->tid);
    snprintf(tmp, sizeof(tmp), "%s/" IMAGE_NAME ".x-new", dir);
    snprintf(dst, sizeof(dst), "%s/" IMAGE_NAME, dir);
    if (mkdir(dir, 0777) && errno != EEXIST) {
        set_job(JOB_ERROR, "Cannot create %s (%s)", dir, strerror(errno));
        return NULL;
    }
    unlink(tmp);

    struct statfs fs;
    if (statfs(dir, &fs) == 0) {
        unsigned long long avail = (unsigned long long)fs.f_bavail * fs.f_bsize;
        if (avail < size + (32ull << 20)) {
            set_job(JOB_ERROR, "Not enough free space: need %llu MB, %llu MB free", (size >> 20) + 32, avail >> 20);
            return NULL;
        }
    }

    pthread_mutex_lock(&lock);
    job_bytes = 0;
    job_total = size;
    pthread_mutex_unlock(&lock);
    set_job(JOB_DOWNLOAD, "Downloading " IMAGE_NAME " %s for %s (%s)...", tag, r->tid, r->region);

    DlCtx d;
    memset(&d, 0, sizeof(d));
    d.fp = fopen(tmp, "wb");
    d.limit = size;
    if (!d.fp) {
        set_job(JOB_ERROR, "Cannot write %s (%s)", tmp, strerror(errno));
        return NULL;
    }
    sha256_init(&d.sha);
    struct curl_blob ca = { (void *)assets_cacert_pem, assets_cacert_pem_len, CURL_BLOB_COPY };
    CURL *c = curl_easy_init();
    CURLcode res = CURLE_FAILED_INIT;
    long status = 0;
    if (c) {
        curl_common(c, url, &ca);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, file_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &d);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 2048L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)size);
        res = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
    }
    int werr = fflush(d.fp) != 0;
    werr |= fsync(fileno(d.fp)) != 0;
    werr |= fclose(d.fp) != 0;
    if (res != CURLE_OK || status != 200 || werr) {
        unlink(tmp);
        if (res != CURLE_OK) set_job(JOB_ERROR, "Download failed: %s", curl_easy_strerror(res));
        else if (status != 200) set_job(JOB_ERROR, "Download failed: HTTP %ld", status);
        else set_job(JOB_ERROR, "Writing %s failed", tmp);
        return NULL;
    }

    set_job(JOB_VERIFY, "Verifying the download...");
    unsigned char hash[32];
    char hex[65];
    sha256_final(&d.sha, hash);
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    if (d.bytes != size) {
        unlink(tmp);
        set_job(JOB_ERROR, "Download incomplete: %llu of %llu bytes", d.bytes, size);
        return NULL;
    }
    if (digest[0] && strcasecmp(hex, digest)) {
        unlink(tmp);
        set_job(JOB_ERROR, "SHA-256 mismatch, download discarded");
        return NULL;
    }
    ulog("SHA-256 %s%s", hex, digest[0] ? " (matches the release)" : " (release lists no digest)");

    set_job(JOB_INSTALL, "Installing into %s...", dir);
    chmod(tmp, 0666);
    if (rename(tmp, dst)) {
        unlink(tmp);
        set_job(JOB_ERROR, "Replacing %s failed (%s)", dst, strerror(errno));
        return NULL;
    }
    sync();
    pthread_mutex_lock(&scan_lock);
    scan_image(r);
    pthread_mutex_unlock(&scan_lock);
    set_job(JOB_DONE, "Done: %s (%s) now has Y2JB Autoloader X %s. If that YouTube app is open, close it and start it again.",
            r->tid, r->region, tag);
    notify(UPDATER_NAME "\n%s (%s) updated to %s.\nRestart that YouTube app.", r->tid, r->region, tag);
    return NULL;
}

static void *scan_worker(void *arg) {
    (void)arg;
    if (pthread_mutex_trylock(&scan_lock)) return NULL; /* a scan is already running */
    fetch_latest_release();
    for (int i = 0; i < REGION_COUNT; i++) scan_image(&regions[i]);
    pthread_mutex_unlock(&scan_lock);
    return NULL;
}

/* ── HTTP ──────────────────────────────────────────────────────────── */

static void json_escape(char *out, size_t out_len, const char *in) {
    size_t n = 0;
    for (; *in && n + 7 < out_len; in++) {
        unsigned char ch = (unsigned char)*in;
        if (ch == '"' || ch == '\\') { out[n++] = '\\'; out[n++] = (char)ch; }
        else if (ch < 0x20) n += snprintf(out + n, out_len - n, "\\u%04x", ch);
        else out[n++] = (char)ch;
    }
    out[n] = 0;
}

static char *build_status(void) {
    size_t cap = 24 * 1024, n = 0;
    char *o = malloc(cap), esc[512];
    if (!o) return NULL;
    pthread_mutex_lock(&lock);
    n += snprintf(o + n, cap - n, "{\"version\":\"%s\",", UPDATER_VERSION);
    json_escape(esc, sizeof(esc), latest_error);
    n += snprintf(o + n, cap - n,
                  "\"latest\":{\"state\":\"%s\",\"tag\":\"%s\",\"size\":%llu,\"error\":\"%s\"},",
                  latest_state == 0 ? "loading" : latest_state == 1 ? "ok" : "error",
                  latest_tag, latest_size, esc);
    n += snprintf(o + n, cap - n, "\"regions\":[");
    for (int i = 0; i < REGION_COUNT; i++) {
        Region *r = &regions[i];
        json_escape(esc, sizeof(esc), r->version);
        n += snprintf(o + n, cap - n,
                      "%s{\"tid\":\"%s\",\"region\":\"%s\",\"scanned\":%s,\"app\":%s,\"image\":%s,\"kind\":\"%s\",\"version\":\"%s\"}",
                      i ? "," : "", r->tid, r->region, r->scanned ? "true" : "false",
                      r->app_installed ? "true" : "false", r->has_image ? "true" : "false", r->kind, esc);
    }
    json_escape(esc, sizeof(esc), job_message);
    n += snprintf(o + n, cap - n,
                  "],\"job\":{\"state\":\"%s\",\"tid\":\"%s\",\"bytes\":%llu,\"total\":%llu,\"message\":\"%s\"},\"log\":[",
                  job_names[job_state], job_tid, job_bytes, job_total, esc);
    for (int i = 0; i < log_count && n + 600 < cap; i++) {
        json_escape(esc, sizeof(esc), log_ring[(log_head + i) % LOG_LINES]);
        n += snprintf(o + n, cap - n, "%s\"%s\"", i ? "," : "", esc);
    }
    snprintf(o + n, cap - n, "]}");
    pthread_mutex_unlock(&lock);
    return o;
}

static enum MHD_Result reply(struct MHD_Connection *conn, unsigned int status, const char *type,
                             void *data, size_t len, enum MHD_ResponseMemoryMode mode) {
    struct MHD_Response *resp = MHD_create_response_from_buffer(len, data, mode);
    if (!resp) return MHD_NO;
    MHD_add_response_header(resp, "Content-Type", type);
    MHD_add_response_header(resp, "Cache-Control", "no-store");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

static enum MHD_Result text(struct MHD_Connection *conn, unsigned int status, const char *msg) {
    return reply(conn, status, "text/plain; charset=utf-8", (void *)msg, strlen(msg), MHD_RESPMEM_MUST_COPY);
}

static enum MHD_Result on_request(void *cls, struct MHD_Connection *conn, const char *url,
                                  const char *method, const char *version, const char *upload,
                                  size_t *upload_size, void **con_cls) {
    (void)cls; (void)version; (void)upload;
    if (!*con_cls) { *con_cls = (void *)1; return MHD_YES; }
    if (*upload_size) { *upload_size = 0; return MHD_YES; }

    /* Only our own loopback names: a DNS-rebound hostname is refused outright. */
    const char *host = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "Host");
    if (!host || (strcmp(host, HOST_A) && strcmp(host, HOST_B)))
        return text(conn, 421, "wrong host");
    atomic_store(&last_request, (long)time(NULL));

    if (!strcmp(method, "GET")) {
        if (!strcmp(url, "/") || !strcmp(url, "/index.html"))
            return reply(conn, 200, "text/html; charset=utf-8", (void *)assets_index_html, assets_index_html_len, MHD_RESPMEM_PERSISTENT);
        if (!strcmp(url, "/jb.png"))
            return reply(conn, 200, "image/png", (void *)assets_jb_png, assets_jb_png_len, MHD_RESPMEM_PERSISTENT);
        if (!strcmp(url, "/status")) {
            char *s = build_status();
            if (!s) return text(conn, 500, "out of memory");
            return reply(conn, 200, "application/json", s, strlen(s), MHD_RESPMEM_MUST_FREE);
        }
        return text(conn, 404, "not found");
    }

    /* State-changing routes: POST + the per-launch token in a custom header. A page
     * from another origin can't send that header without a CORS preflight (never
     * allowed), and doesn't know the token. */
    const char *hdr = MHD_lookup_connection_value(conn, MHD_HEADER_KIND, "X-Y2JB-Updater");
    if (strcmp(method, "POST") || !hdr || !session_token[0] || strcmp(hdr, session_token))
        return text(conn, 403, "forbidden");

    if (!strcmp(url, "/update")) {
        Region *r = find_region(MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "tid"));
        if (!r) return text(conn, 400, "unknown title id");
        pthread_mutex_lock(&lock);
        int busy = JOB_BUSY(job_state);
        if (!busy) {
            job_state = JOB_RELEASE;
            snprintf(job_tid, sizeof(job_tid), "%s", r->tid);
            job_bytes = job_total = 0;
        }
        pthread_mutex_unlock(&lock);
        if (busy) return text(conn, 409, "an update is already running");
        pthread_t t;
        if (pthread_create(&t, NULL, install_worker, r)) {
            set_job(JOB_ERROR, "Could not start the update thread");
            return text(conn, 500, "thread failed");
        }
        pthread_detach(t);
        return text(conn, 200, "OK");
    }
    if (!strcmp(url, "/rescan")) {
        pthread_t t;
        if (!pthread_create(&t, NULL, scan_worker, NULL)) pthread_detach(t);
        return text(conn, 200, "OK");
    }
    if (!strcmp(url, "/exit")) {
        pthread_mutex_lock(&lock);
        int busy = JOB_BUSY(job_state);
        pthread_mutex_unlock(&lock);
        if (busy) return text(conn, 409, "an update is running");
        atomic_store(&keep_running, 0);
        return text(conn, 200, "OK");
    }
    return text(conn, 404, "not found");
}

/* ── Process ───────────────────────────────────────────────────────── */

#ifndef HOST_TEST
static pid_t find_pid(const char *name) {
    int mib[4] = {1, 14, 8, 0};
    pid_t mypid = getpid(), pid = -1;
    size_t size;
    if (sysctl(mib, 4, 0, &size, 0, 0)) return -1;
    uint8_t *buf = malloc(size);
    if (!buf) return -1;
    if (sysctl(mib, 4, buf, &size, 0, 0)) { free(buf); return -1; }
    /* FreeBSD kinfo_proc as exposed by the PS5 kernel: ki_pid at 72, ki_tdname at 447. */
    for (uint8_t *p = buf; p < buf + size;) {
        int len = *(int *)p;
        if (len <= 0) break;
        if (!strcmp(name, (char *)&p[447]) && *(pid_t *)&p[72] != mypid) pid = *(pid_t *)&p[72];
        p += len;
    }
    free(buf);
    return pid;
}
#endif

int main(void) {
    (void)y2jbx_updater_sig[0];
    setvbuf(stdout, NULL, _IOLBF, 0); /* one log line at a time, even into a file */
#ifndef HOST_TEST
    pid_t pid;
    syscall(SYS_thr_set_name, -1, THREAD_NAME);
    /* One updater at a time: replace an older instance. */
    while ((pid = find_pid(THREAD_NAME)) > 0) {
        if (kill(pid, SIGKILL)) break;
        sleep(1);
    }
#endif

    printf("[Y2JBXU] " UPDATER_NAME " v" UPDATER_VERSION " starting on 127.0.0.1:%d\n", PORT);
    int prio = 256;
    sceNetCtlInit();
    sceUserServiceInitialize(&prio);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Token first: no request may be served while it is still empty. */
    unsigned char rnd[16];
    arc4random_buf(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) snprintf(session_token + i * 2, 3, "%02x", rnd[i]);

    /* Loopback only: the page runs in the PS5's own browser. */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, PORT, NULL, NULL, &on_request, NULL,
        MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&addr,
        MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)4,
        MHD_OPTION_END);
    if (!daemon) {
        notify(UPDATER_NAME "\nError: HTTP server failed to start");
        return 1;
    }

    atomic_store(&last_request, (long)time(NULL));
    pthread_t t;
    if (!pthread_create(&t, NULL, scan_worker, NULL)) pthread_detach(t);

    char url[128];
    snprintf(url, sizeof(url), "http://" HOST_A "/?v=%s#t=%s", UPDATER_VERSION, session_token);
    if (sceSystemServiceLaunchWebBrowser(url) != 0)
        notify(UPDATER_NAME "\nCould not open the browser.\nOpen %s", url);
    else
        notify(UPDATER_NAME " v" UPDATER_VERSION "\nPick the YouTube app to update in the browser.");

    while (atomic_load(&keep_running)) {
        usleep(200000);
        pthread_mutex_lock(&lock);
        int busy = JOB_BUSY(job_state);
        pthread_mutex_unlock(&lock);
        if (!busy && time(NULL) - atomic_load(&last_request) > IDLE_EXIT_SEC) {
            printf("[Y2JBXU] Idle for %d minutes, exiting.\n", IDLE_EXIT_SEC / 60);
            break;
        }
    }

    usleep(300000);
    MHD_stop_daemon(daemon);
    curl_global_cleanup();
    printf("[Y2JBXU] Stopped.\n");
    return 0;
}
