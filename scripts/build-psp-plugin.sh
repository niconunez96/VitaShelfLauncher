#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${PSPDEV_IMAGE:-pspdev/pspdev:latest}
SOURCE="$ROOT/vendor/pocket-shelf-psp-boot"
BUILD_DIR="$ROOT/.build/psp-plugin"
OUTPUT="$ROOT/assets/psp/vitashelflauncher_pspboot.prx"

mkdir -p "$BUILD_DIR"

docker run --rm \
  -v "$SOURCE:/src:ro" \
  -v "$BUILD_DIR:/out" \
  -w /work \
  "$IMAGE" \
  sh -ec '
    set -eu
    mkdir -p /work
    cp -R /src/psp-boot /work/
    export PATH="$PSPDEV/bin:$PATH"
    cd /work/psp-boot
    make clean >/dev/null 2>&1 || true
    make
    cp vitashelflauncher_pspboot.prx /out/
  '

cp "$BUILD_DIR/vitashelflauncher_pspboot.prx" "$OUTPUT"
sha256sum "$OUTPUT"
