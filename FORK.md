# Fork notes

This repo is a fork of [itsPLK/ps5-y2jb-autoloader](https://github.com/itsPLK/ps5-y2jb-autoloader)
that loads [Payload Manager X](https://github.com/bsk193/ps5-payload-manager-x) instead of the
official Payload Manager. It is the Y2JB sibling of
[ps5-webkit-autoloader-x](https://github.com/bsk193/ps5-webkit-autoloader-x); both show as
**Jailbreak** with the broken-chain icon (play badge here, globe badge for WebKit).

## Dependency chain

```
bsk193/ps5-y2jb-autoloader-x            (this repo — Y2JB exploit chain, runs in the YouTube app)
  └─ ps5-unified-autoloader.elf
     = bsk193/ps5-unified-autoloader-x  (fork: browser/app handling, autoload.txt)
        └─ always-launched manager
           = bsk193/ps5-payload-manager-x  (pldmgrx, HTTP port 8084)
```

Same layering as upstream, which bundles `itsPLK/ps5-unified-autoloader` (→ official Payload
Manager). No exploit logic is changed.

## What actually changed here

| File | Change |
|---|---|
| `.gitmodules`, `third_party/ps5-unified-autoloader` | URL → `bsk193/ps5-unified-autoloader-x` (path kept as upstream's to keep merges small) |
| `scripts/download_deps.sh` | Release repo → `bsk193/ps5-unified-autoloader-x` |
| `src/update.js` | Update file is `y2jb_x_update.zip` (`UPDATE_ZIP_NAME`), so it coexists with upstream |
| `src/main.js`, `src/splash.html`, `src/jb.png` | Jailbreak shared UI, identical to the WebKit fork's (dark, flat, small radii, plain icon `jb.png`, version footer); log "Y2JB Autoloader X vN, based on Y2JB Autoloader vX by PLK" |
| `src/icon0.png` | Homescreen tile icon: broken chain + play badge, 512x512 (the updater writes it to the YouTube tile on each successful jailbreak; shows after a reboot) |
| `Makefile` | Fork version hook (`X_VERSION`); `VERSION` stays upstream's |
| `tools/fork_version.sh`, `tools/release.ps1` | Fork versioning (see below) |
| `updater/` | Fork-only: `y2jb-x-updater.elf`, installs the latest `download0.dat` into a YouTube region picked in the browser (see below) |
| `.github/workflows/` | `build.yml` (every push/PR) and tag-driven `release.yml` replace `dev.yml`/`stable.yml` |
| `README.md`, `.gitignore` | Fork notes, update zip name |

## Coexisting with upstream's Y2JB

Y2JB has no app of its own: `download0.dat` is the YouTube app's download data, mounted from
`/user/download/<title id>/`. Each YouTube region (`PPSA01650`, `PPSA01651`, `PPSA01652`) has its
own folder, so **install a second region's YouTube** and give each one a different `download0.dat`.
The updater also writes the build's `icon0.png` to that YouTube's homescreen tile
(`/user/appmeta/<title id>/icon0.png`), so the two tiles look different.

The one shared thing was the update file: upstream's updater looks for `y2jb_update.zip` on USB /
`/data`, verifies only file sizes and never deletes it, so one zip would be installed by **both**
loaders on every launch. This fork looks only for **`y2jb_x_update.zip`**; upstream never reads it.

`ps5_autoloader/autoload.txt` is read by both (that is intended: same payload config).

## Building `y2jb_x_update.zip` and `download0.dat`

Both are built in CI (`build.yml`); `download0.dat` needs FreeBSD's `newfs`:

1. `./build.sh --download-deps` downloads the pinned elfldr / kexp / unified-autoloader-x ELFs
   into `src/`, substitutes the versions into `main.js`, and runs
   `third_party/y2jb-updater/create_update_package.py` → `y2jb_update.zip` (files + `update-info.txt`
   with their sizes, which the updater verifies).
2. The zip is unpacked into `download0/cache/splash_screen/aHR0cHM6Ly93d3cueW91dHViZS5jb20vdHY=/`
   (base64 of `https://www.youtube.com/tv`), minus `update-info.txt`.
3. A FreeBSD VM creates a 336789504-byte UFS2 image (`newfs -O 2 -b 32768 -f 4096`), copies the
   folder in, `chmod 444` on `splash.html` → `download0.dat`.
4. The zip is renamed to `y2jb_x_update.zip` and both are uploaded as
   `y2jb-autoloader-x_v<ver>[-<sha>]_ps5`.

## The updater (`updater/`)

A native PS5 ELF, same pattern as the WebKit installer: it serves a page on `127.0.0.1:18383`,
opens the PS5 browser on it, and waits for a choice.

- **Shows** each YouTube region (`PPSA01650` US, `PPSA01651` EU, `PPSA01652` JP): whether the app is
  installed (`/user/app/<tid>`) and what `/user/download/<tid>/download0.dat` holds. It scans the
  image for `const upstream_version = "` (only Y2JB X's `main.js` has it) and the highest
  `const autoloader_version = "vX"` (an in-place zip update can leave older copies in freed blocks).
- **Downloads** `download0.dat` from this repo's latest release (GitHub API `releases/latest`, so
  pre-releases are skipped) to `download0.dat.x-new` next to the old file, after a free-space check.
- **Verifies** the size and the SHA-256 that GitHub lists for the asset, then `rename()`s it over
  `download0.dat`, so a failed or partial download never replaces a working image. If that YouTube
  app is running, the new image is used the next time it starts.
- **Guards:** listens on loopback only; changing routes are `POST` + `X-Y2JB-Updater: 1`, which a page
  from another origin can't send without a CORS preflight (never allowed). Exits from the page, or
  after 15 idle minutes.

Built with the same SDK image as Payload Manager X (`updater/Dockerfile.sdk`, `build_deps.sh`:
libcurl + mbedTLS + libmicrohttpd). `tests/host_test.sh` builds the same code for Linux with the PS5
calls stubbed and runs region detection, the guards and a real download + SHA-256 check; CI runs it
in `build.yml` (job `updater`). Release asset: `y2jb-x-updater_v<ver>_ps5.elf`.

## Versioning

Own semver from git tags with the prefix **`x-v`**, the same as every bsk193 `-x` fork:
`x-v1.2.3` is a release (Latest), `x-v1.2.3-alpha.N` / `-beta.N` / `-rc.N` a pre-release. `VERSION`
in the `Makefile` is upstream's and is never edited; it is shown as "based on vX". Releases come
only from commits on `main`. Dev builds use `git describe` (`0.0.0-dev` with no tags).

## Release procedure

Bottom-up: **payload-manager-x → unified-autoloader-x → this repo**. `download_deps.sh` downloads
the unified-autoloader-x release whose tag (`x-v*`) is on the pinned submodule commit, so pin it
to a released tag:

```bash
git -C third_party/ps5-unified-autoloader fetch --tags origin
git -C third_party/ps5-unified-autoloader checkout <unified-autoloader-x release tag>
git commit -am "build(deps): bump ps5-unified-autoloader-x"
git push fork <branch>      # build.yml must pass, then merge to main
```

Then `tools/release.ps1 -Bump patch|minor|major [-Pre beta] [-DryRun] [-Yes]` on `main`.

## Syncing with upstream

```bash
git fetch upstream
git merge upstream/main
```

Remotes: `upstream` = itsPLK (read-only), `fork` = bsk193.
