#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-1.77.2}"
ARCHIVE_NAME="filament-v${VERSION}-linux.tgz"
URL="https://github.com/google/filament/releases/download/v${VERSION}/${ARCHIVE_NAME}"
DEPS_DIR="$ROOT/build/deps"
DEST="$DEPS_DIR/filament-v${VERSION}-linux"
ARCHIVE="$DEPS_DIR/$ARCHIVE_NAME"

if [[ "$VERSION" != "1.77.2" ]]; then
    echo "No pinned checksum is configured for Filament v${VERSION}." >&2
    echo "Update this script's release checksum before using another version." >&2
    exit 2
fi

EXPECTED_SHA256="b01d7aeb3d6877fbd6c9736ce1fa1eb2aeb67fa3a3603017c9aa04a15f8e455a"

mkdir -p "$DEPS_DIR"
if [[ ! -f "$ARCHIVE" ]]; then
    curl --fail --location --retry 3 "$URL" --output "$ARCHIVE"
fi

printf '%s  %s\n' "$EXPECTED_SHA256" "$ARCHIVE" | sha256sum --check --status || {
    echo "Filament archive checksum mismatch: $ARCHIVE" >&2
    exit 1
}

if [[ ! -f "$DEST/filament/lib/x86_64/libfilament.a" ]]; then
    mkdir -p "$DEST"
    tar -xzf "$ARCHIVE" -C "$DEST"
fi

if [[ ! -f "$DEST/filament/lib/x86_64/libfilament.a" ]]; then
    echo "Filament SDK extraction is incomplete under $DEST." >&2
    exit 1
fi

for material in unlit lit lit_aorm; do
    "$DEST/filament/bin/matc" -a vulkan -p desktop \
        -o "$ROOT/assets/shader/desktop/vulkan/$material.filamat" \
        "$ROOT/src/mat/$material.mat"
done

echo "Filament v${VERSION} SDK ready: $DEST/filament"
