# Y2JB Autoloader X Updater - native PS5 ELF
# Build inside the SDK image (Dockerfile.sdk here, same as Payload Manager X):
#   docker build -t ps5-payload-sdk -f updater/Dockerfile.sdk updater
#   docker run --rm -v "$PWD":/src -w /src/updater ps5-payload-sdk make X_VERSION=1.2.3

PYTHON := python3
SDK    := /opt/ps5-payload-sdk
TARGET := $(SDK)/target
CC     := $(SDK)/bin/prospero-clang
STRIP  := $(SDK)/bin/prospero-strip

# Same version as the Y2JB Autoloader X release it ships with.
ifeq ($(strip $(X_VERSION)),)
X_VERSION := $(shell cd .. && bash tools/fork_version.sh dev 2>/dev/null)
endif

ELF := y2jb-x-updater.elf

CFLAGS  := -Os -Wall -ffunction-sections -fdata-sections -Iinclude -Ibuild -I$(TARGET)/include \
           -DUPDATER_VERSION=\"$(X_VERSION)\"
LDFLAGS := -Wl,--gc-sections
LIBS    := $(TARGET)/lib/libcurl.a $(TARGET)/lib/libmbedtls.a $(TARGET)/lib/libmbedx509.a \
           $(TARGET)/lib/libmbedcrypto.a $(TARGET)/lib/libmicrohttpd.a \
           -L$(TARGET)/lib -lpthread -lSceNetCtl -lSceUserService -lSceSystemService

ASSETS := build/assets_index_html.h build/assets_jb_png.h build/assets_cacert_pem.h
SRCS   := src/main.c src/sha256.c

all: $(ELF)

build:
	mkdir -p build

build/assets_index_html.h: frontend/index.html | build
	$(PYTHON) tools/gen_assets.py $< $@ index_html
build/assets_jb_png.h: ../src/jb.png | build
	$(PYTHON) tools/gen_assets.py $< $@ jb_png
# CA bundle from the image's signed Ubuntu ca-certificates package.
build/assets_cacert_pem.h: /etc/ssl/certs/ca-certificates.crt | build
	$(PYTHON) tools/gen_assets.py $< $@ cacert_pem

# The version is baked in, so always relink.
$(ELF): $(SRCS) $(ASSETS) FORCE
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)
	$(STRIP) $@
	@echo "Built $@ (Y2JB Autoloader X Updater $(X_VERSION))"

clean:
	rm -rf build $(ELF)

FORCE:
.PHONY: all clean FORCE
