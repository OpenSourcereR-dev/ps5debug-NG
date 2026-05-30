#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ARTIFACT="$SCRIPT_DIR/ps5debug-NG.elf"

clean_build() {
    echo "==> cleaning"
    (cd debugger  && make clean) || true
    (cd installer && make clean) || true
    rm -f "$ARTIFACT"
}

build_debugger() {
    echo "==> building debugger"
    make -C debugger
}

build_installer() {
    echo "==> building installer (embeds debugger)"
    make -C installer
}

publish_artifact() {
    cp installer/build/ps5debug-NG.elf "$ARTIFACT"
    echo "==> ps5debug-NG.elf ready ($(stat -c %s "$ARTIFACT") bytes)"
}

if [ "${1:-}" = "clean" ]; then
    clean_build
    exit 0
fi

build_debugger
build_installer
publish_artifact
