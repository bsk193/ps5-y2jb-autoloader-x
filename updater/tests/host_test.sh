#!/usr/bin/env bash
# Host test for the updater: builds main.c for Linux with the PS5 calls stubbed
# (HOST_TEST) and /user under a temp folder, then drives the HTTP API.
#   docker run --rm -v "$PWD":/src -w /src/updater debian:bookworm-slim bash tests/host_test.sh [x-image]
# x-image: optional path (inside the container) to a real Y2JB X download0.dat
# used as the "already on X" region. The test downloads the latest release
# from GitHub into another region, so it needs network access.
set -euo pipefail

apt-get update -qq >/dev/null
apt-get install -y -qq gcc libc6-dev libcurl4-openssl-dev libmicrohttpd-dev python3 ca-certificates curl >/dev/null

ROOT=$(mktemp -d)
B=$(mktemp -d)
python3 tools/gen_assets.py frontend/index.html "$B/assets_index_html.h" index_html >/dev/null
python3 tools/gen_assets.py ../src/jb.png "$B/assets_jb_png.h" jb_png >/dev/null
python3 tools/gen_assets.py /etc/ssl/certs/ca-certificates.crt "$B/assets_cacert_pem.h" cacert_pem >/dev/null
gcc -O1 -Wall -Wextra -Wno-unused-parameter -DHOST_TEST -DROOT_PREFIX="\"$ROOT\"" -DUPDATER_VERSION='"test"' \
    -Iinclude -I"$B" -o "$B/updater" src/main.c src/sha256.c -lcurl -lmicrohttpd -lpthread

# PPSA01650: Y2JB X (real image if given, else a fake one); PPSA01651: upstream; PPSA01652: nothing.
mkdir -p "$ROOT/user/app/PPSA01650" "$ROOT/user/app/PPSA01651" "$ROOT/user/download/PPSA01650" "$ROOT/user/download/PPSA01651"
if [ -n "${1:-}" ]; then
  cp "$1" "$ROOT/user/download/PPSA01650/download0.dat"
else
  { head -c 3000000 /dev/zero; printf 'const autoloader_version = "v1.0.0";\n'; head -c 5000000 /dev/zero
    printf 'const autoloader_version = "v1.1.1";\nconst upstream_version = "v1.1.0";\n'; } > "$ROOT/user/download/PPSA01650/download0.dat"
fi
{ head -c 4194300 /dev/zero; printf 'const autoloader_version = "v1.1.0-abc1234";\n'; } > "$ROOT/user/download/PPSA01651/download0.dat"

"$B/updater" > "$B/log.txt" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true; cat "$B/log.txt"' EXIT
U=http://127.0.0.1:18383
for _ in $(seq 50); do curl -sf $U/status >/dev/null && break; sleep 0.2; done
TOK=$(sed -n 's/.*browser: .*#t=\([0-9a-f]*\).*/\1/p' "$B/log.txt" | head -1)
[ ${#TOK} = 32 ] || { echo "FAIL: no launch token in log"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }
st() { curl -sf "$U/status"; }
# field: evaluates a fixed expression written in this script against /status JSON (test-only).
field() { python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }

# Wait for the startup scan + release check.
for _ in $(seq 300); do
  [ "$(st | field "all(r['scanned'] for r in d['regions']) and d['latest']['state']!='loading'")" = True ] && break; sleep 1
done
st | python3 -m json.tool | head -40
[ "$(st | field "d['latest']['state']")" = ok ] || fail "release check"
[ "$(st | field "d['regions'][0]['kind']")" = x ] || fail "PPSA01650 should be x"
[ "$(st | field "d['regions'][1]['kind']+' '+d['regions'][1]['version']")" = "upstream v1.1.0-abc1234" ] || fail "PPSA01651 should be upstream v1.1.0-abc1234"
[ "$(st | field "d['regions'][2]['kind']+' '+str(d['regions'][2]['app'])")" = "none False" ] || fail "PPSA01652 should be none"
echo "scan OK: $(st | field "[(r['tid'],r['kind'],r['version']) for r in d['regions']]")"

# Guards: GET or POST without the header must not start anything.
[ "$(curl -s -o /dev/null -w '%{http_code}' $U/update?tid=PPSA01651)" = 404 ] || fail "GET /update"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST $U/update?tid=PPSA01651)" = 403 ] || fail "POST without header"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Y2JB-Updater: $TOK" $U/update?tid=PPSA9)" = 400 ] || fail "bad tid"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H 'Origin: http://evil' -X OPTIONS $U/update)" = 403 ] || fail "preflight"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'X-Y2JB-Updater: 1' $U/update?tid=PPSA01651)" = 403 ] || fail "wrong token"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'X-Y2JB-Updater: ' $U/update?tid=PPSA01651)" = 403 ] || fail "empty token"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H 'Host: rebind.example:18383' $U/status)" = 421 ] || fail "rebound Host on GET"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Host: rebind.example:18383' -H "X-Y2JB-Updater: $TOK" $U/update?tid=PPSA01651)" = 421 ] || fail "rebound Host on POST"
[ "$(curl -s -o /dev/null -w '%{http_code}' -H 'Host: localhost:18383' $U/status)" = 200 ] || fail "localhost Host"
echo "guards OK"

# Real update of the upstream region from the latest GitHub release.
curl -sf -X POST -H "X-Y2JB-Updater: $TOK" "$U/update?tid=PPSA01651" >/dev/null || fail "start update"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Y2JB-Updater: $TOK" $U/update?tid=PPSA01650)" = 409 ] || fail "second update not refused"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST -H "X-Y2JB-Updater: $TOK" $U/exit)" = 409 ] || fail "exit during update not refused"
for _ in $(seq 900); do
  S=$(st | field "d['job']['state']")
  [ "$S" = done ] || [ "$S" = error ] && break
  sleep 1
done
st | field "d['job']"
[ "$S" = done ] || fail "update ended in $S"
TAG=$(st | field "d['latest']['tag']")
[ "$(st | field "d['regions'][1]['kind']+' '+d['regions'][1]['version']")" = "x v${TAG#x-v}" ] || fail "PPSA01651 not on $TAG after update"
[ ! -e "$ROOT/user/download/PPSA01651/download0.dat.x-new" ] || fail "temp file left behind"
WANT=$(curl -sf https://api.github.com/repos/bsk193/ps5-y2jb-autoloader-x/releases/latest | python3 -c "import json,sys; print([a['digest'] for a in json.load(sys.stdin)['assets'] if a['name']=='download0.dat'][0])")
GOT="sha256:$(sha256sum "$ROOT/user/download/PPSA01651/download0.dat" | cut -d' ' -f1)"
[ "$WANT" = "$GOT" ] || fail "installed image digest $GOT != release $WANT"
echo "update OK: PPSA01651 -> $TAG ($GOT)"

curl -sf -X POST -H "X-Y2JB-Updater: $TOK" $U/exit >/dev/null
for _ in $(seq 30); do kill -0 $PID 2>/dev/null || break; sleep 0.2; done
kill -0 $PID 2>/dev/null && fail "did not exit"
echo "ALL PASSED"
