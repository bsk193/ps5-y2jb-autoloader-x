# Y2JB Autoloader X Updater

`y2jb-x-updater.elf` installs the latest Y2JB Autoloader X `download0.dat` into a YouTube app you
pick, without FTP. Send it to the PS5 with Payload Manager X or any ELF loader; the PS5 browser
opens on its page (`http://127.0.0.1:18383`).

How it works, guards and tests: [FORK.md, "The updater"](../FORK.md#the-updater-updater).

Build (from the repo root):

```
docker build -t ps5-payload-sdk -f updater/Dockerfile.sdk updater
docker run --rm -v "$PWD":/src -w /src/updater ps5-payload-sdk make X_VERSION=1.2.3
docker run --rm -v "$PWD":/src:ro -w /src/updater debian:bookworm-slim bash tests/host_test.sh
```

`src/sha256.c`, `Dockerfile.sdk`, `build_deps.sh` and `tools/gen_assets.py` come from
[Payload Manager X](https://github.com/bsk193/ps5-payload-manager-x) (GPLv3).
