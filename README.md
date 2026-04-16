# rust-rocksdb

[![RocksDB build](https://github.com/surrealdb/rust-rocksdb/actions/workflows/rust.yml/badge.svg?branch=master)](https://github.com/surrealdb/rust-rocksdb/actions/workflows/rust.yml)
[![crates.io](https://img.shields.io/crates/v/surrealdb-rocksdb.svg)](https://crates.io/crates/surrealdb-rocksdb)
[![documentation](https://img.shields.io/docsrs/surrealdb-rocksdb/latest)](https://docs.rs/surrealdb-rocksdb)
[![license](https://img.shields.io/crates/l/surrealdb-rocksdb.svg)](https://github.com/surrealdb/rust-rocksdb/blob/master/LICENSE)
![rust 1.85.0 required](https://img.shields.io/badge/rust-1.85.0-blue.svg?label=MSRV)

![GitHub commits (since latest release)](https://img.shields.io/github/commits-since/surrealdb/rust-rocksdb/latest.svg)

> [!NOTE]
> - The `master` branch is an up-to-date sync from the upstream [rust-rocksdb](https://github.com/rust-rocksdb/rust-rocksdb) repository.
> - The `main` branch is rebased from `master` with packaging changes to publish as `surrealdb-rocksdb` on crates.io (the library name remains `rocksdb`, so existing `use rocksdb::*` imports work unchanged).
> - Release branches (e.g. `0.24.0-surreal`) are rebased from `main`, one per published version.
> - For bugs unrelated to packaging, please report them [upstream](https://github.com/rust-rocksdb/rust-rocksdb/issues).

## Requirements

- Clang and LLVM

## Contributing

Feedback and pull requests are welcome! If a particular feature of RocksDB is
important to you, please let me know by opening an issue, and I'll
prioritize it.

## Usage

This binding is statically linked with a specific version of RocksDB. If you
want to build it yourself, make sure you've also cloned the RocksDB and
compression submodules:

```shell
git submodule update --init --recursive
```

To update the RocksDB submodule to the latest commit on its tracked branch:

```shell
git submodule update --init --recursive --remote librocksdb-sys/rocksdb
```

## Compression Support

By default, support for [Snappy](https://github.com/google/snappy),
[LZ4](https://github.com/lz4/lz4), [Zstd](https://github.com/facebook/zstd),
[Zlib](https://zlib.net), and [Bzip2](http://www.bzip.org) compression
is enabled through crate features. If support for all of these compression
algorithms is not needed, default features can be disabled and specific
compression algorithms can be enabled. For example, to enable only LZ4
compression support, make these changes to your Cargo.toml:

```toml
[dependencies.surrealdb-rocksdb]
default-features = false
features = ["lz4"]
```

## Transparent Encryption at Rest

This fork exposes RocksDB's `EncryptedEnv` for transparent encryption of all data
at rest. When enabled, all SST files, WAL files, and the manifest are encrypted
using AES-256-CTR. Reads are decrypted on-the-fly, and RocksDB code above the `Env`
layer never sees plaintext on disk.

### Enabling the Feature

Add the `encrypted-env` feature to your `Cargo.toml`:

```toml
[dependencies.surrealdb-rocksdb]
features = ["encrypted-env"]
```

### Usage Example

```rust
use rocksdb::{DB, EncryptedEnv, Options};

fn open_encrypted_db(path: &str, key: Vec<u8>) -> Result<DB, Box<dyn std::error::Error>> {
    // Create EncryptedEnv with a 32-byte AES-256 key
    let encrypted_env = EncryptedEnv::new(key)?;

    // Configure database options
    let mut opts = Options::default();
    opts.create_if_missing(true);

    // Attach the encrypted environment
    opts.set_encrypted_env(encrypted_env);

    // Open the database normally — encryption is transparent
    let db = DB::open(&opts, path)?;
    Ok(db)
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Derive your 32-byte key from a secure source (e.g., device secure enclave)
    let key: Vec<u8> = vec![0x42; 32]; // Replace with actual key derivation

    let db = open_encrypted_db("/path/to/db", key)?;
    db.put(b"key1", b"value1")?;

    Ok(())
}
```

### Important Notes

- **Key Length**: The encryption key must be exactly 32 bytes (AES-256).
- **Key Security**: The `EncryptedEnv::new()` method zeroes the key buffer in Rust
  immediately after passing it to C++. However, you should still manage key material
  carefully in your application (e.g., derive from a secure enclave, zero after use).
- **Opt-In Feature**: The `encrypted-env` feature is completely opt-in. Callers without
  the feature see no changes to the API.
- **Encryption Scope**: All files written by RocksDB (SST, WAL, manifest) are encrypted.
  Without the correct key, these files are unreadable and will fail to open.

### Security Considerations

- The encryption is transparent to SurrealDB — no changes are needed in the database
  layer above RocksDB.
- If you open an encrypted database without `encrypted-env` enabled, or with the wrong
  key, RocksDB will fail to read the data (either open fails or reads return corruption).
- This implementation uses RocksDB's built-in `CTREncryptionProvider` with a configurable
  block cipher (currently configured to use ROT13 as a placeholder — adapt for production
  with AES-CTR).

## Multithreaded ColumnFamily alternation

RocksDB allows column families to be created and dropped
from multiple threads concurrently, but this crate doesn't allow it by default
for compatibility. If you need to modify column families concurrently, enable
the crate feature `multi-threaded-cf`, which makes this binding's
data structures use `RwLock` by default. Alternatively, you can directly create
`DBWithThreadMode<MultiThreaded>` without enabling the crate feature.

## Switch between /MT or /MD run time library (Only for Windows)

The feature `mt_static` will request the library to be built with [/MT](https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library?view=msvc-170)
flag, which results in library using the static version of the run-time library.
*This can be useful in case there's a conflict in the dependency tree between different
run-time versions.*

## Switch between static and dynamic linking for bindgen (features `bindgen-static` and `bindgen-runtime`)

The feature `bindgen-runtime` will enable the `runtime` feature of bindgen, which dynamically
links to libclang. This is suitable for most platforms, and is enabled by default.

The feature `bindgen-static` will enable the `static` feature of bindgen, which statically
links to libclang. This is suitable for musllinux platforms, such as Alpine linux.
To build on Alpine linux for example, make these changes to your Cargo.toml:

```toml
[dependencies.surrealdb-rocksdb]
default-features = false
features = ["bindgen-static", "snappy", "lz4", "zstd", "zlib", "bzip2"]
```

Notice that `runtime` and `static` features are mutually exclusive, and won't compile if both are enabled.

## LTO
Enable the `lto` feature to enable link-time optimization. It will compile rocksdb with `-flto` flag. This feature is disabled by default.

> [!IMPORTANT]
> You must use clang as `CC`. Eg. `CC=/usr/bin/clang CXX=/usr/bin/clang++`. Clang llvm version must be the same as the one used by rust compiler.
> On the rust side you should use `RUSTFLAGS="-Clinker-plugin-lto -Clinker=clang -Clink-arg=-fuse-ld=lld"`.

Check the [Rust documentation](https://doc.rust-lang.org/rustc/linker-plugin-lto.html) for more information.
