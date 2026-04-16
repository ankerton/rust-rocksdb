# AGENTS.md — surrealdb-rocksdb encryption patch

## What you are building

A fork of `surrealdb/rust-rocksdb` that exposes RocksDB's `EncryptedEnv` to the Rust layer. The goal is transparent encryption at rest — SurrealDB sees a normal RocksDB instance and knows nothing about encryption. The encryption key is supplied by the caller (derived from the device secure enclave at application startup).

This is a targeted patch to an existing codebase. You are not rewriting anything. You are adding the minimum FFI bindings, Rust wrappers, and feature flag needed to wire `EncryptedEnv` into the existing `DB::open` path.

**Upstream repo:** `https://github.com/surrealdb/rust-rocksdb`  
Fork it, apply the patch, publish as a path or git dependency in the Renovium workspace.

---

## Background — what EncryptedEnv is

RocksDB's `EncryptedEnv` is a C++ wrapper around RocksDB's `Env` interface. It intercepts all file I/O and applies AES-CTR encryption transparently. Every SST file, WAL file, and manifest written to disk is encrypted. Reads are decrypted on the fly. RocksDB's own code above `Env` never touches plaintext on disk.

The `EncryptedEnv` is in the mainline RocksDB C++ source (`utilities/env_encryption.h`, `utilities/env_encryption.cc`). It is compiled into the RocksDB static library that `surrealdb/rust-rocksdb` links against via `librocksdb-sys`. It is not currently exposed in the Rust bindings — no FFI declaration, no Rust wrapper, no feature flag.

This patch adds all three.

---

## Constraints

- Minimal diff — touch only what is necessary; do not refactor unrelated code
- No `unwrap()` in new Rust code — use `Result` throughout
- The `EncryptedEnv` feature is opt-in behind a Cargo feature flag `encrypted-env` — callers without the feature see no change
- Key length is exactly 32 bytes (AES-256-CTR)
- The `EncryptedEnv` takes ownership of the key material at construction — zero the key buffer in Rust after passing it
- Do not add any new dependencies to `Cargo.toml` beyond what is already present — `EncryptedEnv` is in the existing RocksDB C++ build

---

## Repo layout — files to touch

```
surrealdb/rust-rocksdb/
├── Cargo.toml                         ← add encrypted-env feature flag
├── librocksdb-sys/
│   ├── rocksdb/
│   │   └── utilities/
│   │       ├── env_encryption.h       ← already exists in RocksDB source (read only)
│   │       └── env_encryption.cc      ← already exists in RocksDB source (read only)
│   └── build.rs                       ← add env_encryption.cc to compilation list
├── src/
│   ├── lib.rs                         ← re-export EncryptedEnv when feature enabled
│   ├── env.rs                         ← NEW: Rust wrapper for EncryptedEnv
│   └── db.rs                          ← add encrypted_env field to Options, wire into open
```

---

## Step 1 — Verify EncryptedEnv is in the C++ source

Before writing any Rust, confirm the C++ files exist in the submodule:

```
librocksdb-sys/rocksdb/utilities/env_encryption.h
librocksdb-sys/rocksdb/utilities/env_encryption.cc
```

Check the header for the `EncryptedEnv` class and `NewEncryptedEnv` factory function signature. The expected signature is:

```cpp
// env_encryption.h
class EncryptedEnv : public EnvWrapper { ... };

Status NewEncryptedEnv(Env* base_env,
                       EncryptionProvider* provider,
                       Env** result);
```

Also check for `CTREncryptionProvider` — this is the AES-CTR provider used in practice:

```cpp
class CTREncryptionProvider : public EncryptionProvider { ... };
```

If either class is absent from the submodule, stop and report — the RocksDB version in the submodule may predate these classes. Do not proceed.

---

## Step 2 — Add env_encryption.cc to the build

**File: `librocksdb-sys/build.rs`**

Find where `.cc` source files are added to the build (look for `cpp_files.push` or similar). Add:

```rust
cpp_files.push("rocksdb/utilities/env_encryption.cc");
```

Confirm the build still compiles after this change before proceeding.

---

## Step 3 — Add the Cargo feature flag

**File: `Cargo.toml`**

```toml
[features]
# existing features unchanged
encrypted-env = []
```

No new crate dependencies — `EncryptedEnv` is in the existing RocksDB static library.

---

## Step 4 — Write the FFI declarations

**File: `src/env.rs`** (new file)

```rust
#[cfg(feature = "encrypted-env")]
mod ffi_encrypted {
    use libc::c_char;

    extern "C" {
        /// Create a CTREncryptionProvider with the given 32-byte key.
        /// Returns a raw pointer to the provider. Caller owns the pointer.
        /// Key must be exactly 32 bytes (AES-256).
        pub fn rocksdb_ctr_encryption_provider_create(
            key: *const c_char,
            key_len: libc::size_t,
        ) -> *mut libc::c_void;

        /// Destroy a CTREncryptionProvider created by rocksdb_ctr_encryption_provider_create.
        pub fn rocksdb_ctr_encryption_provider_destroy(
            provider: *mut libc::c_void,
        );

        /// Create an EncryptedEnv wrapping the default Env with the given provider.
        /// Returns a raw pointer to the EncryptedEnv. Caller owns the pointer.
        pub fn rocksdb_encrypted_env_create(
            provider: *mut libc::c_void,
        ) -> *mut libc::c_void;

        /// Destroy an EncryptedEnv created by rocksdb_encrypted_env_create.
        pub fn rocksdb_encrypted_env_destroy(
            env: *mut libc::c_void,
        );
    }
}
```

These C functions do not exist yet — you will add their implementations in the C++ shim in Step 5.

---

## Step 5 — Write the C++ shim

The existing `EncryptedEnv` C++ API takes C++ objects. To call it from Rust FFI, add a thin C shim with C linkage.

**File: `librocksdb-sys/crocksdb/crocksdb.h`** — add declarations (find the existing shim header and append):

```c
#ifdef ROCKSDB_ENCRYPTED_ENV
extern "C" {
void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len);
void  crocksdb_ctr_encryption_provider_destroy(void* provider);
void* crocksdb_encrypted_env_create(void* provider);
void  crocksdb_encrypted_env_destroy(void* env);
}
#endif
```

**File: `librocksdb-sys/crocksdb/crocksdb.cc`** — add implementations (append to existing file):

```cpp
#ifdef ROCKSDB_ENCRYPTED_ENV
#include "rocksdb/utilities/env_encryption.h"

extern "C" {

void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len) {
    std::string key_str(key, key_len);
    auto* provider = new rocksdb::CTREncryptionProvider(key_str);
    return static_cast<void*>(provider);
}

void crocksdb_ctr_encryption_provider_destroy(void* provider) {
    delete static_cast<rocksdb::CTREncryptionProvider*>(provider);
}

void* crocksdb_encrypted_env_create(void* provider) {
    auto* enc_provider = static_cast<rocksdb::EncryptionProvider*>(provider);
    rocksdb::Env* encrypted_env = nullptr;
    rocksdb::NewEncryptedEnv(rocksdb::Env::Default(), enc_provider, &encrypted_env);
    return static_cast<void*>(encrypted_env);
}

void crocksdb_encrypted_env_destroy(void* env) {
    delete static_cast<rocksdb::Env*>(env);
}

} // extern "C"
#endif
```

**Note on CTREncryptionProvider constructor signature:** the exact constructor may differ across RocksDB versions. Check `env_encryption.h` for the actual constructor. If it takes a `std::string` key directly, use the above. If it takes a separate `BlockCipher` object, adapt accordingly and document the change.

---

## Step 6 — Write the Rust wrapper

**File: `src/env.rs`** — complete the file:

```rust
use std::ptr::NonNull;

/// A handle to a RocksDB EncryptedEnv.
/// When passed to Options, all I/O through this DB instance is AES-256-CTR encrypted.
/// The key is consumed at construction — it is zeroed in memory after being passed to C++.
#[cfg(feature = "encrypted-env")]
pub struct EncryptedEnv {
    provider_ptr: NonNull<libc::c_void>,
    env_ptr: NonNull<libc::c_void>,
}

#[cfg(feature = "encrypted-env")]
impl EncryptedEnv {
    /// Create a new EncryptedEnv with a 32-byte AES-256 key.
    /// The key buffer is zeroed after use.
    /// Returns an error if key length is not exactly 32 bytes.
    pub fn new(mut key: Vec<u8>) -> Result<Self, String> {
        if key.len() != 32 {
            return Err(format!(
                "EncryptedEnv key must be exactly 32 bytes, got {}",
                key.len()
            ));
        }

        let provider_ptr = unsafe {
            ffi_encrypted::rocksdb_ctr_encryption_provider_create(
                key.as_ptr() as *const libc::c_char,
                key.len(),
            )
        };

        // Zero the key immediately after passing to C++
        for b in key.iter_mut() { *b = 0; }
        drop(key);

        let provider_ptr = NonNull::new(provider_ptr)
            .ok_or_else(|| "Failed to create CTREncryptionProvider".to_string())?;

        let env_ptr = unsafe {
            ffi_encrypted::rocksdb_encrypted_env_create(provider_ptr.as_ptr())
        };

        let env_ptr = NonNull::new(env_ptr)
            .ok_or_else(|| "Failed to create EncryptedEnv".to_string())?;

        Ok(Self { provider_ptr, env_ptr })
    }

    /// Returns the raw env pointer for passing to Options.
    /// Do not free this pointer manually — Drop handles cleanup.
    pub(crate) fn as_ptr(&self) -> *mut libc::c_void {
        self.env_ptr.as_ptr()
    }
}

#[cfg(feature = "encrypted-env")]
impl Drop for EncryptedEnv {
    fn drop(&mut self) {
        unsafe {
            ffi_encrypted::rocksdb_encrypted_env_destroy(self.env_ptr.as_ptr());
            ffi_encrypted::rocksdb_ctr_encryption_provider_destroy(self.provider_ptr.as_ptr());
        }
    }
}

// EncryptedEnv is not Clone or Copy — the raw pointer must not be aliased.
#[cfg(feature = "encrypted-env")]
unsafe impl Send for EncryptedEnv {}
```

---

## Step 7 — Wire EncryptedEnv into Options and DB::open

**File: `src/db.rs`** (or wherever `Options` and DB open logic live — check the actual file)

Find the `Options` struct (or its FFI equivalent). Add the env field:

```rust
// In the DB open function, before calling rocksdb_open:
#[cfg(feature = "encrypted-env")]
if let Some(env) = &options.encrypted_env {
    // Set the env on the rocksdb Options via FFI
    // The function name may be rocksdb_options_set_env — check existing bindings
    unsafe {
        ffi::rocksdb_options_set_env(opts_ptr, env.as_ptr() as *mut _);
    }
}
```

The exact integration point depends on how the existing `DB::open` is structured. Find where `rocksdb_open` or `rocksdb_open_for_read_only` is called and insert the env assignment immediately before it.

**Public API addition** — add to whatever struct or builder is used to configure DB open:

```rust
#[cfg(feature = "encrypted-env")]
pub fn set_encrypted_env(&mut self, env: EncryptedEnv) -> &mut Self {
    self.encrypted_env = Some(env);
    self
}
```

---

## Step 8 — Update lib.rs re-exports

**File: `src/lib.rs`**

```rust
#[cfg(feature = "encrypted-env")]
pub use env::EncryptedEnv;
```

---

## How the caller uses this (reference only — do not implement in this repo)

In `app-data` (the Renovium crate that initialises SurrealDB):

```rust
#[cfg(feature = "encrypted-env")]
{
    let key: Vec<u8> = enclave::derive_key()?;   // 32-byte key from secure enclave
    let encrypted_env = EncryptedEnv::new(key)?;
    // Pass encrypted_env into RocksDB Options before opening SurrealDB
}
```

SurrealDB opens RocksDB via its own `kv-rocksdb` feature. The encrypted env must be injected at the RocksDB `Options` level before `Surreal::new::<RocksDb>(path)` is called. Exactly how this is wired through SurrealDB's own open path is a separate concern — this repo's job is only to expose `EncryptedEnv` from the binding layer.

---

## Build verification sequence

Run these in order. Each must pass before proceeding to the next.

1. `cargo build` — base build, no features
2. `cargo build --features encrypted-env` — with feature enabled; C++ shim compiles
3. `cargo test` — existing tests pass
4. `cargo test --features encrypted-env` — encrypted-env tests pass (see below)
5. `cargo clippy --features encrypted-env` — zero warnings

---

## Tests required

**File: `tests/encrypted_env.rs`** (new file, feature-gated):

```rust
#![cfg(feature = "encrypted-env")]

use rocksdb::{DB, EncryptedEnv, Options};
use tempfile::TempDir;

/// Create a 32-byte test key. Do not use in production.
fn test_key() -> Vec<u8> {
    vec![0x42u8; 32]
}

#[test]
fn test_encrypted_env_creates_without_error() {
    let env = EncryptedEnv::new(test_key());
    assert!(env.is_ok(), "EncryptedEnv creation failed: {:?}", env.err());
}

#[test]
fn test_encrypted_env_wrong_key_length() {
    let result = EncryptedEnv::new(vec![0u8; 16]);
    assert!(result.is_err());
    assert!(result.unwrap_err().contains("32 bytes"));
}

#[test]
fn test_db_open_with_encrypted_env() {
    let dir = TempDir::new().unwrap();
    let env = EncryptedEnv::new(test_key()).unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_encrypted_env(env);

    let db = DB::open(&opts, dir.path());
    assert!(db.is_ok(), "DB::open with EncryptedEnv failed: {:?}", db.err());
}

#[test]
fn test_write_and_read_with_encrypted_env() {
    let dir = TempDir::new().unwrap();
    let env = EncryptedEnv::new(test_key()).unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_encrypted_env(env);

    let db = DB::open(&opts, dir.path()).unwrap();
    db.put(b"key1", b"value1").unwrap();
    let result = db.get(b"key1").unwrap();
    assert_eq!(result.as_deref(), Some(b"value1".as_ref()));
    drop(db);
}

#[test]
fn test_encrypted_files_unreadable_without_env() {
    let dir = TempDir::new().unwrap();

    // Write with encrypted env
    {
        let env = EncryptedEnv::new(test_key()).unwrap();
        let mut opts = Options::default();
        opts.create_if_missing(true);
        opts.set_encrypted_env(env);
        let db = DB::open(&opts, dir.path()).unwrap();
        db.put(b"secret", b"data").unwrap();
    }

    // Attempt to open without encrypted env — should fail or return garbage
    {
        let mut opts = Options::default();
        opts.create_if_missing(false);
        let result = DB::open(&opts, dir.path());
        // Either open fails, or the key is not found, or the value is corrupt
        if let Ok(db) = result {
            let val = db.get(b"secret").unwrap();
            // Must not return the plaintext value
            assert_ne!(val.as_deref(), Some(b"data".as_ref()),
                "Encrypted data was readable without EncryptedEnv — encryption is not working");
        }
        // Open failing entirely is also an acceptable outcome
    }
}
```

The final test — `test_encrypted_files_unreadable_without_env` — is the meaningful security validation. If this test passes, encryption is functioning correctly.

---

## Definition of done

- `cargo build --features encrypted-env` passes with zero warnings
- `cargo test --features encrypted-env` passes with all five tests green
- `cargo build` (no features) passes — feature-gated code fully isolated
- `test_encrypted_files_unreadable_without_env` passes — this is the critical one
- The patch is committed to the fork with a clear commit message: `feat: expose EncryptedEnv via encrypted-env feature flag`
