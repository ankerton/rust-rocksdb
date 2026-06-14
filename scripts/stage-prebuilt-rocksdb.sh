#!/usr/bin/env bash
# Stage a prebuilt librocksdb.a / libsnappy.a so downstream Ankerton stacks LINK the
# static lib instead of recompiling the ~1.5G RocksDB C++ tree on every fresh/clean build.
#
# Pairs with each stack's .cargo/config.toml, which sets:
#   ROCKSDB_LIB_DIR / SNAPPY_LIB_DIR -> <workspace-root>/.prebuilt/rocksdb-10.7.5
#   ROCKSDB_STATIC=1 / SNAPPY_STATIC=1
# The surrealdb-librocksdb-sys build.rs then skips build_rocksdb() and links the .a.
#
# Run this once per machine (and after the rocksdb submodule version bumps). To force a
# from-source build anywhere instead, set ROCKSDB_COMPILE=1 in the environment.
#
# Usage:
#   scripts/stage-prebuilt-rocksdb.sh [TARGET_DIR]
#   FROM_SOURCE=1 scripts/stage-prebuilt-rocksdb.sh [TARGET_DIR]   # build locally, no download
#
# Default TARGET_DIR = <workspace-root>/.prebuilt/rocksdb-10.7.5, derived from the canonical
# layout <root>/forge/rust-rocksdb. Pass a path to override (e.g. standalone clones).
set -euo pipefail

TAG="prebuilt-rocksdb-10.7.5"
REPO="ankerton/rust-rocksdb"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DEFAULT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
TARGET_DIR="${1:-$ROOT_DEFAULT/.prebuilt/rocksdb-10.7.5}"

mkdir -p "$TARGET_DIR"

if [ "${FROM_SOURCE:-0}" = "1" ]; then
  echo "Building librocksdb from source via surrealdb-librocksdb-sys ..."
  pushd "$SCRIPT_DIR/.." >/dev/null
  ROCKSDB_COMPILE=1 cargo build --release -p surrealdb-librocksdb-sys
  out=$(find target/release/build -path '*surrealdb-librocksdb-sys*/out' -type d | head -1)
  cp "$out/librocksdb.a" "$out/libsnappy.a" "$TARGET_DIR/"
  ( cd "$TARGET_DIR" && shasum -a 256 librocksdb.a libsnappy.a > SHA256SUMS )
  popd >/dev/null
else
  echo "Downloading prebuilt rocksdb ($TAG) from $REPO -> $TARGET_DIR ..."
  gh release download "$TAG" -R "$REPO" \
    --pattern 'librocksdb.a' --pattern 'libsnappy.a' --pattern 'SHA256SUMS' \
    --dir "$TARGET_DIR" --clobber
  ( cd "$TARGET_DIR" && shasum -a 256 -c SHA256SUMS )
fi

echo "Staged into $TARGET_DIR:"
ls -la "$TARGET_DIR"/*.a
