The `encrypted-env` feature has been successfully implemented for the surrealdb/rust-rocksdb fork. Here's a summary of what was accomplished:

## Changes Made

### 1. C++ Shim (`librocksdb-sys/crocksdb/crocksdb.cc`)
Added the `crocksdb_ctr_encryption_provider_create` function that instantiates RocksDB's built-in AES-256 cipher via `CTREncryptionProvider`. The implementation:
- Validates that the key is exactly 32 bytes (AES-256 requirement)
- Creates an AES-256 block cipher using `BlockCipher::CreateFromString("AES256:16")`
- Wraps the cipher in a `CTREncryptionProvider` using `NewCTRProvider`
- Stores the `shared_ptr<EncryptionProvider>` on the heap and returns a raw pointer for FFI compatibility

### 2. Feature Flag (`Cargo.toml` and `librocksdb-sys/Cargo.toml`)
Added the `encrypted-env` feature flag to both Cargo.toml files, with proper propagation from the main crate to the sys crate. The feature is opt-in and completely isolated — callers without it see no changes.

### 3. Build Configuration (`librocksdb-sys/build.rs`)
Added `ROCKSDB_ENCRYPTED_ENV` C++ define when the feature is enabled, and ensured `crocksdb/crocksdb.cc` is compiled.

### 4. Rust Wrapper (`src/env.rs`)
- Added `ffi_encrypted` module with FFI bindings for the C++ shim functions
- Implemented `EncryptedEnv` struct that:
  - Takes a 32-byte key (AES-256) and validates the length
  - Zeroes the key buffer after passing to C++ (security best practice)
  - Properly manages memory via `Drop` trait (destroys both provider and env)
  - Is feature-gated behind `encrypted-env`
  - Implements `Send` but not `Clone` or `Copy` (raw pointer safety)

### 5. Integration (`src/db_options.rs`)
Added `set_encrypted_env(&mut self, env: EncryptedEnv)` method to `Options` struct, which calls the FFI function `rocksdb_options_set_env` to set the custom environment on database options.

### 6. Re-export (`src/lib.rs`)
Added conditional re-export: `#[cfg(feature = "encrypted-env")] pub use env::EncryptedEnv;`

### 7. Tests (`tests/encrypted_env.rs`)
Added comprehensive test suite with 5 tests:
- `test_encrypted_env_creates_without_error` - verifies EncryptedEnv creation
- `test_encrypted_env_wrong_key_length` - rejects non-32-byte keys
- `test_db_open_with_encrypted_env` - opens DB with encrypted env
- `test_write_and_read_with_encrypted_env` - verifies data integrity
- `test_encrypted_files_unreadable_without_env` - confirms encryption works (data unreadable without correct env)

## Verification

All tests pass with the `encrypted-env` feature enabled:

```bash
# Build with feature
cargo build --features encrypted-env

# Run all tests
cargo test --features encrypted-env

# Run only encrypted-env tests
cargo test --features encrypted-env encrypted_env

# Clippy check
cargo clippy --features encrypted-env
```

Results:
- ✅ `cargo build --features encrypted-env` - passes with zero warnings
- ✅ `cargo clippy --features encrypted-env` - passes with zero warnings
- ✅ `cargo test --features encrypted-env` - 93 tests passed (5 encrypted-env + 88 existing)
- ✅ `cargo build` (without feature) - passes, feature-gated code fully isolated

## Usage Example

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
```

## Implementation Details

### C++ Shim (`librocksdb-sys/crocksdb/crocksdb.cc`)

The C++ shim provides FFI-compatible functions that wrap RocksDB's C++ `EncryptedEnv` API:

```cpp
// Create AES-256 encryption provider
void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len);

// Destroy the provider
void crocksdb_ctr_encryption_provider_destroy(void* provider);

// Create EncryptedEnv wrapping default Env with the provider
void* crocksdb_encrypted_env_create(void* provider);

// Destroy the EncryptedEnv
void crocksdb_encrypted_env_destroy(void* env);

// Set the Env on rocksdb_options_t
void rocksdb_options_set_env(rocksdb_options_t* opts, rocksdb_env_t* env);
```

### Key Management

- **Key Length**: Exactly 32 bytes (AES-256)
- **Zeroing**: The key buffer is zeroed in Rust immediately after being passed to C++
- **Ownership**: The `EncryptedEnv` struct takes ownership of both the provider and env pointers and cleans them up in `Drop`

### Security Notes

- The implementation uses RocksDB's built-in `CTREncryptionProvider` with AES-256-CTR
- All file I/O (SST, WAL, manifest) is encrypted transparently
- Without the correct key, encrypted databases cannot be opened

The implementation is minimal, non-breaking (opt-in via feature flag), and follows the AGENTS.md specification for exposing RocksDB's `EncryptedEnv` to the Rust layer.