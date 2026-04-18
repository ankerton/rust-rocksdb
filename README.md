# rust-rocksdb — Ankerton Fork

**GitHub:** https://github.com/ankerton/rust-rocksdb

This is the Ankerton fork of [rust-rocksdb](https://github.com/rust-rocksdb/rust-rocksdb), itself based on [SurrealDB's rust-rocksdb fork](https://github.com/surrealdb/rust-rocksdb). It adds two patches to the RocksDB C API layer and the Rust bindings above it:

1. **AES-256-CTR encryption at rest** via RocksDB's `EncryptedEnv`
2. **User-Defined Timestamps (UDT) for `OptimisticTransactionDB`** — enabling MVCC point-in-time reads with correct timestamp stamping on every committed write

These patches are the foundation of the encryption and versioning capabilities in the [Ankerton SurrealDB fork](https://github.com/ankerton/surrealdb).

---

## Table of Contents

1. [Requirements](#requirements)
2. [Building from source](#building-from-source)
3. [Crate features](#crate-features)
4. [Patch 1 — AES-256-CTR encryption at rest](#patch-1--aes-256-ctr-encryption-at-rest)
   - [What it does](#what-it-does)
   - [C API shims](#c-api-shims-added-to-dbcc)
   - [Rust API](#rust-api-encrypted-env)
   - [Lifetime and ownership model](#lifetime-and-ownership-model)
   - [Usage example](#usage-example-encryption)
5. [Patch 2 — UDT timestamp stamping for OptimisticTransactionDB](#patch-2--udt-timestamp-stamping-for-optimistictransactiondb)
   - [Background: User-Defined Timestamps in RocksDB](#background-user-defined-timestamps-in-rocksdb)
   - [The bug: why timestamps were never stamped](#the-bug-why-timestamps-were-never-stamped)
   - [The fix](#the-fix)
   - [C API shims](#c-api-shims-udt)
   - [Rust API](#rust-api-udt)
   - [Usage example](#usage-example-udt)
6. [How SurrealDB uses these patches](#how-surrealdb-uses-these-patches)
7. [Comparator setup for MVCC](#comparator-setup-for-mvcc)
8. [Point-in-time reads with ReadOptions](#point-in-time-reads-with-readoptions)
9. [Upstream features](#upstream-features)

---

## Requirements

- Rust 1.85.0 or later
- Clang and LLVM (required to compile RocksDB)

## Building from source

This crate statically links a specific version of RocksDB. Clone submodules before building:

```shell
git clone https://github.com/ankerton/rust-rocksdb
cd rust-rocksdb
git submodule update --init --recursive
```

To update the RocksDB submodule:

```shell
git submodule update --init --recursive --remote librocksdb-sys/rocksdb
```

---

## Crate features

| Feature | Default | Description |
|---|---|---|
| `snappy` | yes | Snappy compression |
| `lz4` | yes | LZ4 compression |
| `zstd` | yes | Zstd compression |
| `zlib` | yes | Zlib compression |
| `bzip2` | yes | Bzip2 compression |
| `encrypted-env` | no | AES-256-CTR encryption at rest (Ankerton patch) |
| `multi-threaded-cf` | no | `RwLock`-based concurrent column family access |
| `lto` | no | Link-time optimisation (`-flto`; requires clang as CC) |
| `bindgen-runtime` | yes | Dynamically link libclang for bindgen |
| `bindgen-static` | no | Statically link libclang (for musl/Alpine) |
| `mt_static` | no | Windows: use `/MT` static runtime |

Enable only specific compression algorithms:

```toml
[dependencies.surrealdb-rocksdb]
default-features = false
features = ["lz4", "encrypted-env"]
```

---

## Patch 1 — AES-256-CTR encryption at rest

### What it does

When `encrypted-env` is enabled and an `EncryptedEnv` is attached to `Options`, RocksDB routes all file I/O through a C++ `EncryptedEnv` object that wraps the default `Env`. Every byte written to disk — SST files, WAL files, the MANIFEST — is AES-256-CTR encrypted. Reads are decrypted transparently. RocksDB code above the `Env` layer never sees plaintext on disk.

Without the correct 32-byte key, RocksDB either fails to open the database or returns corrupt data on every read.

### C API shims added to `db/c.cc`

RocksDB's public C API (`rocksdb/c.h`) has no bindings for `EncryptedEnv` or `CTREncryptionProvider`. Four shims were added:

```c
// Create a CTREncryptionProvider with a 32-byte AES-256 key.
// Returns an opaque pointer. Caller owns it.
void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len);

// Destroy a CTREncryptionProvider.
void crocksdb_ctr_encryption_provider_destroy(void* provider);

// Create an EncryptedEnv wrapping the default Env with the given provider.
// Returns an opaque pointer to the C++ Env*. Caller owns it.
void* crocksdb_encrypted_env_create(void* provider);

// Destroy an EncryptedEnv.
void crocksdb_encrypted_env_destroy(void* env);
```

A fifth shim reuses the existing `rocksdb_options_set_env` signature to accept a raw `Env*` pointer alongside a full `rocksdb_env_t` wrapper:

```c
// Set the Env on rocksdb_options_t. Options copies out the Env* internally.
void rocksdb_options_set_env(rocksdb_options_t* options, void* env);
```

### Rust API (EncryptedEnv)

```rust
use rocksdb::EncryptedEnv;     // requires feature = "encrypted-env"
use rocksdb::{Options, DB};

// Create from a 32-byte key. The key is zeroed in Rust immediately after
// being handed to C++.
let env = EncryptedEnv::new(key_bytes)?; // key_bytes: Vec<u8>, must be 32 bytes

// Attach to Options before opening the database
let mut opts = Options::default();
opts.create_if_missing(true);
opts.set_encrypted_env(env);

// Open normally — encryption is fully transparent
let db = DB::open(&opts, path)?;
```

`EncryptedEnv` is cheaply cloneable — all clones share ownership of the underlying C++ objects via `Arc`. The C++ `EncryptedEnv` and `CTREncryptionProvider` are destroyed only when the last clone is dropped.

### Lifetime and ownership model

This was a critical correctness fix. The original implementation stored `EncryptedEnv` as `Option<EncryptedEnv>` in `OptionsMustOutliveDB` but set it to `None` in the `Clone` impl — silently dropping the C++ objects while the database was still using them, causing a use-after-free and SIGSEGV.

The fix introduces a two-level ownership structure:

```
EncryptedEnv  (Clone — cheaply cloneable)
 └── Arc<EncryptedEnvInner>
      ├── provider_ptr: NonNull<c_void>  — C++ CTREncryptionProvider*
      └── env_raw:      NonNull<c_void>  — C++ Env* (the EncryptedEnv)
```

`EncryptedEnvInner` is non-`Clone` and destroys the C++ objects in its `Drop` impl. `OptionsMustOutliveDB::clone()` now calls `self.encrypted_env.clone()`, incrementing the `Arc` reference count rather than freeing the C++ objects. The database holds a clone in its outlive list for its entire lifetime — the C++ objects are freed only when both the database and all user-held clones are dropped.

### Usage example (encryption)

```rust
use rocksdb::{DB, EncryptedEnv, Options};

fn open_encrypted(path: &str, key: Vec<u8>) -> Result<DB, rocksdb::Error> {
    let env = EncryptedEnv::new(key)
        .map_err(|e| rocksdb::Error::new(e))?;  // key must be 32 bytes

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_encrypted_env(env);

    DB::open(&opts, path)
}
```

With `OptimisticTransactionDB` (as used by SurrealDB):

```rust
use rocksdb::{EncryptedEnv, Options, OptimisticTransactionDB};

let env = EncryptedEnv::new(key)?;

let mut opts = Options::default();
opts.create_if_missing(true);
opts.set_encrypted_env(env);

let db: OptimisticTransactionDB = OptimisticTransactionDB::open(&opts, path)?;
```

---

## Patch 2 — UDT timestamp stamping for OptimisticTransactionDB

### Background: User-Defined Timestamps in RocksDB

RocksDB's User-Defined Timestamps (UDT) feature embeds a fixed-size timestamp suffix into every user key stored on disk. This enables MVCC: multiple versions of the same key coexist, ordered by timestamp descending within each key. Point-in-time reads are performed by setting `ReadOptions::set_timestamp(ts)`, which instructs RocksDB to skip any version newer than `ts` and return the most recent version with `commit_ts ≤ ts`.

When UDT is enabled on a column family:
- Every `Put(key, value)` stores the key as `[ user_key ][ 8×0x00 ]` — a zero-byte placeholder for the timestamp.
- Before committing, the placeholder must be replaced with the real commit timestamp by calling `WriteBatch::UpdateTimestamps(ts, ts_sz_func)`.
- `UpdateTimestamps` iterates over every key in the batch and overwrites the trailing bytes in-place, but only for column families where `ts_sz_func(cf)` returns a non-zero size.

### The bug: why timestamps were never stamped

For pessimistic (`TransactionDB`) transactions, `GetColumnFamilyToTimestampSize()` returns a map populated as keys are written — `ts_sz_func` correctly returns 8 for the default CF and `UpdateTimestamps` stamps every key.

For **optimistic** (`OptimisticTransactionDB`) transactions, the internal flag `track_timestamp_size_` is `false` by default. The map is never populated. `GetColumnFamilyToTimestampSize()` always returns an empty map. The `ts_sz_func` lambda therefore always returned 0, causing `UpdateTimestamps` to skip every key in the batch — leaving every committed key with an 8-byte zero timestamp.

**Effect:** All versions were stored at timestamp `0`. Every point-in-time read returned the latest version regardless of the timestamp in `ReadOptions`. MVCC was completely non-functional for `OptimisticTransactionDB`.

This is a silent bug — no error is returned; `UpdateTimestamps` clears the `needs_in_place_update_ts_` flag even when no key was actually updated.

### The fix

The fix mirrors what RocksDB's own `WriteCommittedTxn::CommitWithoutPrepareInternal` does internally: when the CF-to-ts-size map is empty, fall back to querying the `WriteBatchWithIndex` comparator directly.

```cpp
// In rocksdb_transaction_assign_commit_timestamp (db/c.cc):

const auto& cf_to_ts_sz = wb->GetColumnFamilyToTimestampSize();

auto ts_sz_func = [&cf_to_ts_sz, wbwi](uint32_t cf) -> size_t {
    // Primary lookup — populated for PessimisticTransaction
    auto it = cf_to_ts_sz.find(cf);
    if (it != cf_to_ts_sz.end()) {
        return it->second;
    }
    // Fallback — works for OptimisticTransaction: the WBWI is constructed
    // with the CF's comparator which carries timestamp_size() == 8 when
    // UDT is enabled on that CF.
    const Comparator* ucmp =
        ROCKSDB_NAMESPACE::WriteBatchWithIndexInternal::GetUserComparator(*wbwi, cf);
    return ucmp ? ucmp->timestamp_size() : 0;
};

Status s = wb->UpdateTimestamps(ts_slice, ts_sz_func);
```

Required addition to `db/c.cc`:

```cpp
#include "utilities/write_batch_with_index/write_batch_with_index_internal.h"
```

### C API shims (UDT)

```c
// Stamp all keys in the transaction's write batch with the given timestamp.
// The timestamp is encoded as an 8-byte little-endian uint64.
// Must be called after all writes are staged and before commit().
// Returns NULL on success, or a malloc'd error string on failure (caller must free).
// Works correctly for both OptimisticTransactionDB and TransactionDB.
char* rocksdb_transaction_assign_commit_timestamp(
    rocksdb_transaction_t* txn,
    uint64_t timestamp
);

// Set the commit timestamp on the transaction object.
// No-op for OptimisticTransactionDB — use assign_commit_timestamp instead.
void rocksdb_transaction_set_commit_timestamp(
    rocksdb_transaction_t* txn,
    uint64_t timestamp
);

// Set the read timestamp for conflict validation during optimistic commit.
// Required for OptimisticTransactionDB when UDT is enabled.
void rocksdb_transaction_set_read_timestamp_for_validation(
    rocksdb_transaction_t* txn,
    uint64_t timestamp
);
```

### Rust API (UDT)

```rust
use rocksdb::{OptimisticTransactionDB, Transaction};

let tx: Transaction<OptimisticTransactionDB> = db.transaction();

// Stage writes
tx.put(b"key", b"value")?;

// Stamp every key in the batch with the commit timestamp.
// Must be called after all writes and before commit().
// ts: u64 HLC value, encoded as 8-byte little-endian in the key suffix.
tx.assign_commit_timestamp(ts)?;

tx.commit()?;
```

For `TransactionDB` (pessimistic), use `set_commit_timestamp` instead — it delegates to the C++ method which is implemented for pessimistic transactions:

```rust
tx.set_commit_timestamp(ts);
tx.commit()?;
```

### Usage example (UDT)

```rust
use rocksdb::{
    Options, OptimisticTransactionDB, ColumnFamilyDescriptor, ReadOptions,
};

fn open_udt_db(path: &str) -> OptimisticTransactionDB {
    let mut cf_opts = Options::default();

    // Register a timestamp-aware comparator.
    // timestamp_size = 8 tells RocksDB each key has an 8-byte timestamp suffix.
    cf_opts.set_comparator_with_ts(
        "surreal.v1",           // comparator name — must match across reopens
        8,                      // timestamp_size in bytes
        Box::new(compare),      // full-key compare: user key ASC, timestamp DESC
        Box::new(compare_ts),   // timestamp-only compare: ASC (older < newer)
        Box::new(compare_no_ts),// key compare stripping the 8-byte suffix
    );

    let cf = ColumnFamilyDescriptor::new("default", cf_opts);

    let mut db_opts = Options::default();
    db_opts.create_if_missing(true);
    db_opts.create_missing_column_families(true);

    OptimisticTransactionDB::open_cf_descriptors(&db_opts, path, vec![cf]).unwrap()
}

fn write(db: &OptimisticTransactionDB, key: &[u8], val: &[u8], ts: u64) {
    let tx = db.transaction();
    tx.put(key, val).unwrap();
    tx.assign_commit_timestamp(ts).unwrap();
    tx.commit().unwrap();
}

fn read_at(db: &OptimisticTransactionDB, key: &[u8], ts: u64) -> Option<Vec<u8>> {
    let mut ro = ReadOptions::default();
    ro.set_timestamp(ts.to_le_bytes().to_vec());
    db.get_opt(key, &ro).unwrap()
}

// write(db, b"cfg", b"v1", 1000);
// write(db, b"cfg", b"v2", 2000);
// read_at(db, b"cfg", 1500) → Some(b"v1")
// read_at(db, b"cfg", 2500) → Some(b"v2")
```

---

## How SurrealDB uses these patches

SurrealDB's RocksDB backend (`surrealdb-core/src/kvs/rocksdb/`) opens an `OptimisticTransactionDB` with a single column family. The two patches combine as follows:

### Database open sequence

```
RocksDbConfig { encryption_key, versioned, sync_mode, retention_ns }
    │
    ├─ encryption_key = Some(key)
    │       EncryptedEnv::new(key)          → C++ CTREncryptionProvider + EncryptedEnv
    │       opts.set_encrypted_env(env)     → all I/O goes through encrypted layer
    │       env stored in OptionsMustOutliveDB (Arc clone) for DB lifetime
    │
    ├─ versioned = true
    │       opts.set_comparator_with_ts(    → UDT comparator registered on default CF
    │           "surreal.v1", 8, …
    │       )
    │
    └─ OptimisticTransactionDB::open_cf_descriptors(opts, path, [cf])
```

Both options are independent — you can use either or both.

### Commit sequence

```
SurrealDB Transaction::commit()
    │
    ├─ inner.assign_commit_timestamp(hlc_ts)
    │       calls rocksdb_transaction_assign_commit_timestamp(txn, ts)
    │       WriteBatch::UpdateTimestamps stamps every key's 8-byte placeholder
    │       with the real HLC timestamp (via WBWI comparator fallback)
    │
    ├─ inner.commit()
    │       RocksDB validates for write-write conflicts and writes to WAL
    │
    └─ coordinator.wait_for_sync()          (sync=every only)
            enqueues a sync request; blocks until flush_wal(true) completes
            multiple transactions' fsync calls are coalesced into one
```

### Read sequence

```
SurrealDB Transaction::get(key, version: Option<u64>)
    │
    ├─ version = None
    │       ReadOptions default → RocksDB returns the latest version
    │
    └─ version = Some(ts)
            ReadOptions::set_timestamp(ts.to_le_bytes())
            RocksDB skips versions where commit_ts > ts
            returns the newest version with commit_ts ≤ ts
```

### HLC timestamp format

```
u64, stored as 8 bytes little-endian in the key suffix:

  bits 63..16  — wall-clock milliseconds (48 bits, ~year 10000+ range)
  bits 15..0   — logical counter (16 bits, 65 535 ticks per millisecond)
```

`HlcTimeStamp::next()` is monotonically increasing and thread-safe. It advances the logical counter within the same millisecond and resets it when the wall clock ticks forward.

---

## Comparator setup for MVCC

The SurrealDB comparator registered via `set_comparator_with_ts` implements three callbacks:

| Callback | Signature | Behaviour |
|---|---|---|
| `compare` | `(a: &[u8], b: &[u8]) → Ordering` | Full key (user bytes + timestamp). User key **ASC**, timestamp **DESC**. |
| `compare_ts` | `(a: &[u8], b: &[u8]) → Ordering` | Timestamp bytes only. **ASC** — older timestamp is "less than" newer. |
| `compare_without_ts` | `(a: &[u8], b: &[u8]) → Ordering` | User key only, stripping the trailing 8 bytes. |

The **descending** timestamp order within each user key means a forward scan naturally encounters the newest version of a key first. RocksDB's `Get` stops at the first matching key and therefore returns the most recent version without scanning all historical versions.

---

## Point-in-time reads with ReadOptions

```rust
let mut ro = ReadOptions::default();

// Tell RocksDB to only return versions with commit_ts <= ts.
// The byte slice must be exactly timestamp_size bytes (8 bytes for SurrealDB).
ro.set_timestamp(ts.to_le_bytes().to_vec());

// All standard read operations respect the timestamp filter:
db.get_opt(b"key", &ro)?;
db.iterator_opt(IteratorMode::Start, ro);
```

Without a timestamp filter, RocksDB returns the latest version as normal.

---

## Upstream features

Everything not described above is inherited unchanged from [rust-rocksdb](https://github.com/rust-rocksdb/rust-rocksdb) and [SurrealDB's fork](https://github.com/surrealdb/rust-rocksdb):

- Full `DB`, `TransactionDB`, `OptimisticTransactionDB` APIs
- Column families, iterators, snapshots, merge operators, prefix extractors
- Compression: Snappy, LZ4, Zstd, Zlib, Bzip2
- Block-based table options, bloom filters, ribbon filters, block cache
- Backup and restore engine
- Multi-threaded column family access (`multi-threaded-cf` feature)
- LTO support (`lto` feature; requires clang as `CC`)
- musl/Alpine static bindgen (`bindgen-static` feature)
- Windows static runtime (`mt_static` feature)
- MSRV: Rust 1.85.0
