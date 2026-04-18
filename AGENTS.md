# AGENTS.md — surrealdb-rocksdb-ankerton

This file covers all active development plans for the `surrealdb-rocksdb-ankerton` fork.

---

## Plan A — EncryptedEnv (encryption at rest) ✅ COMPLETE

Exposes RocksDB's `EncryptedEnv` to the Rust layer so all SST, WAL, and manifest files
are transparently AES-256-CTR encrypted. The encryption key is supplied by the caller
at DB open time — derived from the device Keychain by `app-crypto` at application startup.

### What was built

- `src/env.rs` — `EncryptedEnv` Rust wrapper and `ffi_encrypted` FFI module
- `src/db_options.rs` — `Options::set_encrypted_env(env: EncryptedEnv)` method
- `src/lib.rs` — `pub use env::EncryptedEnv` re-export
- `Cargo.toml` — `encrypted-env` feature flag
- `librocksdb-sys` — C shim functions (`crocksdb_ctr_encryption_provider_create`, etc.)

### How the caller uses it

```rust
use rocksdb::{EncryptedEnv, Options, OptimisticTransactionDB};

let key: Vec<u8> = app_crypto.raw_key().to_vec(); // 32-byte AES-256 key
let enc_env = EncryptedEnv::new(key)?;

let mut opts = Options::default();
opts.create_if_missing(true);
opts.set_encrypted_env(enc_env); // all I/O through this DB is encrypted

let db = OptimisticTransactionDB::open(&opts, path)?;
```

### SurrealDB integration

`surrealdb-core` passes the key via `RocksDbConfig::encryption_key: Option<[u8; 32]>`,
set at startup from `AppCrypto::raw_key()`. The `kvs/rocksdb/mod.rs` backend calls
`opts.set_encrypted_env(enc_env)` before opening the DB.

---

## Plan B — Transaction Timestamps (MVCC / versioned datastore) ✅ COMPLETE

### What was built

Exposed `Transaction::set_commit_timestamp` and `Transaction::set_read_timestamp_for_validation`
in the Rust wrapper layer — the only two methods missing to fully enable SurrealDB's
`versioned = true` datastore mode, which provides MVCC point-in-time reads backed by
RocksDB's User-Defined Timestamp (UDT) API.

Committed as: `d02a5df versioned datastorage exposed`

### Background — how RocksDB UDT works

RocksDB User-Defined Timestamps attach an opaque byte sequence to every key at write
time. Comparators that understand this byte sequence allow reads at any past timestamp.
The full workflow per transaction:

1. **Write path** — before `txn.commit()`, call `txn.set_commit_timestamp(ts)`. RocksDB
   records `ts` as the logical write time for every key in this batch.
2. **Read path** — set `ReadOptions::set_timestamp(ts)` to see only keys written at or
   before `ts`. A timestamp of `u64::MAX` means "latest".
3. **GC** — periodically advance `full_history_ts_low` on the column family so RocksDB
   can compact away old versions.

SurrealDB uses 8-byte little-endian `u64` HLC values as timestamps. The column-family
comparator, `ReadOptions::set_timestamp`, `increase_full_history_ts_low`, and the
garbage collector are all already correct in SurrealDB. The only missing piece is this
crate not exposing `set_commit_timestamp`.

### Current state

| Layer | Status |
|---|---|
| RocksDB C++ — `rocksdb_transaction_set_commit_timestamp` | ✅ Compiled in |
| RocksDB C++ — `rocksdb_transaction_set_read_timestamp_for_validation` | ✅ Compiled in |
| `librocksdb-sys` `bindings.rs` — both FFI declarations | ✅ Auto-generated from `c.h` |
| This crate — `Transaction::set_commit_timestamp` Rust wrapper | ❌ Missing |
| This crate — `Transaction::set_read_timestamp_for_validation` Rust wrapper | ❌ Missing |

Both C functions are already declared in `librocksdb-sys/rocksdb/include/rocksdb/c.h`
and are present in the bindgen-generated `bindings.rs`:

```
target/debug/build/surrealdb-librocksdb-sys-*/out/bindings.rs
  → pub fn rocksdb_transaction_set_commit_timestamp(txn: *mut rocksdb_transaction_t, commit_timestamp: u64)
  → pub fn rocksdb_transaction_set_read_timestamp_for_validation(txn: *mut rocksdb_transaction_t, read_timestamp: u64)
```

No C++, no FFI, no bindgen changes needed. Only Rust wrapper code.

### What was done

**File:** `src/transactions/transaction.rs`

Added two methods to `impl<DB> Transaction<'_, DB>`:
- `set_commit_timestamp(&self, ts: u64)` — stamps all writes with an HLC timestamp before commit
- `set_read_timestamp_for_validation(&self, ts: u64)` — sets the read timestamp for conflict validation in `OptimisticTransactionDB`

Both call bindgen-generated FFI directly — no C++ or librocksdb-sys changes were needed as both functions were already in the generated `bindings.rs` from `c.h`.

Smoke tests added in `mod timestamp_tests` at the bottom of the file.

### SurrealDB follow-on ✅ COMPLETE

`surrealdb/core/src/kvs/rocksdb/mod.rs` — `HlcTimeStamp` import and
`inner.set_commit_timestamp(ts.0)` call were intact throughout (no restore needed).
Encryption wiring merged to `main` via `feat/rocksdb-encryption` branch.
