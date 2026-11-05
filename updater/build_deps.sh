#!/usr/bin/env bash
# Unified dependency build script for PS5 Payload SDK inside Docker
# Tarballs are checked against pinned SHA-256s; bump version and hash together.
set -e

export PATH="/opt/ps5-payload-sdk/bin:$PATH"

TEMPDIR=$(mktemp -d)
trap 'rm -rf -- "$TEMPDIR"' EXIT

cd $TEMPDIR

# Common compiler tools mapped to SDK wrappers
export CC=prospero-clang
export CXX=prospero-clang++
export AR=prospero-ar
export NM=prospero-nm
export RANLIB=prospero-ranlib

echo "=== 1. Building libmicrohttpd 1.0.1 ==="
wget -O libmicrohttpd.tar.gz https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz
echo "a89e09fc9b4de34dde19f4fcb4faaa1ce10299b9908db1132bbfa1de47882b94  libmicrohttpd.tar.gz" | sha256sum -c -
tar xf libmicrohttpd.tar.gz
cd libmicrohttpd-1.0.1
./configure --host=x86_64-pc-freebsd12 \
            --disable-shared --enable-static \
            --disable-curl --disable-examples \
            --prefix=/opt/ps5-payload-sdk/target
make -j$(nproc)
make install

cd $TEMPDIR

echo "=== 2. Building mbedTLS 3.6.0 ==="
wget -O mbedtls.tar.gz https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/v3.6.0.tar.gz
echo "32c500e73ee878e193e7d66bf5e4c34fb42bb968a6c9f9488aa466b16f6f3bff  mbedtls.tar.gz" | sha256sum -c -
tar xf mbedtls.tar.gz
cd mbedtls-*
make CC=prospero-clang AR=prospero-ar RANLIB=prospero-ranlib CFLAGS="-Os" lib -j$(nproc)
mkdir -p /opt/ps5-payload-sdk/target/include
cp -r include/mbedtls include/psa /opt/ps5-payload-sdk/target/include/
mkdir -p /opt/ps5-payload-sdk/target/lib
cp library/libmbedtls.a library/libmbedx509.a library/libmbedcrypto.a /opt/ps5-payload-sdk/target/lib/

cd $TEMPDIR

echo "=== 3. Building libcurl 8.18.0 ==="
wget -O curl.tar.xz https://curl.haxx.se/download/curl-8.18.0.tar.xz
echo "40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946  curl.tar.xz" | sha256sum -c -
tar xf curl.tar.xz
cd curl-8.18.0
sed -i 's|define USE_XATTR| |g' src/tool_xattr.h
wget -O ca-bundle.crt https://curl.se/ca/cacert.pem
./configure --prefix=/opt/ps5-payload-sdk/target \
            --host=x86_64-pc-freebsd \
            --enable-static --disable-shared \
            --with-mbedtls=/opt/ps5-payload-sdk/target \
            --without-openssl \
            --without-libpsl \
            --disable-docs \
            --disable-ftp --disable-ldap --disable-ldaps --disable-rtsp --disable-proxy --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher --disable-mqtt
make -j$(nproc)
make install

echo "=== 4. Deploying CA bundle ==="
mkdir -p /opt/ps5-payload-sdk/target/etc
cp ca-bundle.crt /opt/ps5-payload-sdk/target/etc/ca-bundle.crt

echo "All SDK dependencies (libmicrohttpd, mbedTLS, libcurl) successfully built and installed!"
