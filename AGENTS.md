# AGENTS.md — forge/rust-rocksdb

## What this is

The Ankerton fork of `rust-rocksdb`. Fixes a duplicate symbol linker error in `librocksdb-sys` when the `encrypted-env` feature is active — which is always the case when used via the Ankerton `surrealdb` fork.

## Fork difference from upstream

`librocksdb-sys/build.rs` filters `db/c.cc` from `lib_sources` when `encrypted-env` is enabled. Without this fix, `crocksdb/crocksdb.cc` and `db/c.cc` both export the same RocksDB C API symbols, causing a linker error in debug builds.

## How it is used

Pulled transitively by the Ankerton `surrealdb` fork — you never reference this crate directly in application code. It is listed here for traceability.

## Key constraints

- Do not update RocksDB submodule version without verifying the encrypted-env fix still applies
- Build requires the `rust-builder` Podman container (has the C++ toolchain)
- Submodules must be populated: `git submodule update --init --recursive`
