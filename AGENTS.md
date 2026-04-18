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

## Plan B — Transaction Timestamps (MVCC / versioned datastore)

### What this plan delivers

Expose `Transaction::set_commit_timestamp` and `Transaction::set_read_timestamp_for_validation`
in the Rust wrapper layer. These are the only two methods missing to fully enable
SurrealDB's `versioned = true` datastore mode, which provides MVCC point-in-time reads
backed by RocksDB's User-Defined Timestamp (UDT) API.

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

### Scope

**One file:** `src/transactions/transaction.rs`

Do not touch anything else in this repo.

---

### Step 1 — Add the two wrapper methods

**File:** `src/transactions/transaction.rs`

Find the `impl<DB> Transaction<'_, DB>` block that contains `commit`, `rollback`,
`set_name`, `set_savepoint`, etc. Add the following two methods after `commit`:

```rust
/// Stamp all writes in this transaction with the given commit timestamp.
///
/// Must be called before [`commit`] when the column family was opened with a
/// timestamp-aware comparator (User-Defined Timestamps). The timestamp is an
/// opaque `u64` — SurrealDB uses 8-byte little-endian HLC values.
///
/// Calling this on a transaction against a non-UDT column family is a no-op
/// at the RocksDB level but should be avoided for clarity.
///
/// [`commit`]: Transaction::commit
pub fn set_commit_timestamp(&self, ts: u64) {
    unsafe {
        ffi::rocksdb_transaction_set_commit_timestamp(self.inner, ts);
    }
}

/// Set the read timestamp used for conflict validation.
///
/// `OptimisticTransactionDB` requires this to be set before commit when the
/// column family uses User-Defined Timestamps. RocksDB uses it to detect
/// write–write conflicts against the correct version range.
///
/// Set this immediately after creating the transaction, before any reads or
/// writes. Pass the same timestamp as `ReadOptions::set_timestamp` used for
/// reads within this transaction. Use `u64::MAX` to validate against the
/// latest version.
pub fn set_read_timestamp_for_validation(&self, ts: u64) {
    unsafe {
        ffi::rocksdb_transaction_set_read_timestamp_for_validation(self.inner, ts);
    }
}
```

Both methods are infallible at the Rust level — the underlying C functions have no
error path. The `unsafe` block is required because the functions are `extern "C"`.

---

### Step 2 — Build verification

```bash
cd /Users/zs/Workspaces/rust-rocksdb

# Must pass with zero errors
cargo build

# Must pass — confirms no regression in the downstream consumer
cd /Users/zs/Workspaces/surrealdb
cargo build --no-default-features --features storage-rocksdb
```

If `cargo build` fails with an unresolved symbol, double-check the function name
against the generated bindings:

```bash
grep "set_commit_timestamp\|set_read_timestamp" \
  target/debug/build/surrealdb-librocksdb-sys-*/out/bindings.rs
```

---

### Step 3 — Tests

Add a unit test in `src/transactions/transaction.rs` at the bottom of the file:

```rust
#[cfg(test)]
mod timestamp_tests {
    use super::*;
    use crate::{OptimisticTransactionDB, OptimisticTransactionOptions, Options, WriteOptions};
    use tempfile::TempDir;

    fn open_udt_db(dir: &TempDir) -> OptimisticTransactionDB {
        let mut opts = Options::default();
        opts.create_if_missing(true);
        // UDT requires a timestamp-aware comparator. For this test we use the
        // default comparator — sufficient to verify the FFI call does not crash.
        OptimisticTransactionDB::open(&opts, dir.path()).unwrap()
    }

    #[test]
    fn test_set_commit_timestamp_does_not_panic() {
        let dir = TempDir::new().unwrap();
        let db = open_udt_db(&dir);
        let txn = db.transaction_opt(
            &WriteOptions::default(),
            &OptimisticTransactionOptions::default(),
        );
        // Must not panic or segfault
        txn.set_commit_timestamp(42u64);
        // Commit may fail on a non-UDT DB — we only care that the call itself works
        let _ = txn.commit();
    }

    #[test]
    fn test_set_read_timestamp_for_validation_does_not_panic() {
        let dir = TempDir::new().unwrap();
        let db = open_udt_db(&dir);
        let txn = db.transaction_opt(
            &WriteOptions::default(),
            &OptimisticTransactionOptions::default(),
        );
        txn.set_read_timestamp_for_validation(u64::MAX);
        let _ = txn.commit();
    }
}
```

Run with:

```bash
cd /Users/zs/Workspaces/rust-rocksdb
cargo test transactions
```

---

### Step 4 — Follow-on: restore SurrealDB call site

Once this crate builds cleanly, make the following changes in
`/Users/zs/Workspaces/surrealdb`:

**File:** `surrealdb/core/src/kvs/rocksdb/mod.rs`

**4a — Restore the import** (near the top of the file):
```rust
use crate::kvs::timestamp::HlcTimeStamp;
```

**4b — Replace the stub error** (currently around line 556):
```rust
// Versioned commit timestamps are not supported with this RocksDB build
if self.versioned {
    return Err(Error::Internal(
        "versioned transactions require RocksDB timestamp support, which is not available in this build".into(),
    ));
}
```
With the original logic:
```rust
// When versioned, stamp all writes with the current HLC timestamp
if self.versioned {
    let ts = HlcTimeStamp::next();
    inner.set_commit_timestamp(ts.0);
}
```

**4c — Wire `set_read_timestamp_for_validation` if needed**

If integration tests show a RocksDB panic at commit time on a versioned datastore,
add this immediately after the `inner` transaction is created in `Datastore::begin`:

```rust
if self.versioned {
    inner.set_read_timestamp_for_validation(u64::MAX);
}
```

Only add if tests show it is required.

---

### Definition of done

- [ ] `Transaction::set_commit_timestamp` exists and is callable
- [ ] `Transaction::set_read_timestamp_for_validation` exists and is callable
- [ ] `cargo build` passes in this repo with zero errors
- [ ] `cargo test transactions` passes
- [ ] `cargo build --no-default-features --features storage-rocksdb` passes in surrealdb
- [ ] SurrealDB call site restored (Step 4a + 4b)
- [ ] Opening a SurrealDB datastore with `versioned=true` commits without error

### What is explicitly out of scope for this repo

- Changing anything in `librocksdb-sys` — the bindings are already correct
- Changing the C++ RocksDB source
- Changing SurrealDB's comparator, garbage collector, or `ReadOptions` handling
- Adding UDT support to any backend other than RocksDB
