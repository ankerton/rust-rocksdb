# Verification: EncryptedEnv Implementation vs AGENTS.md

## Context

The `encryption-env` feature was implemented on branch `claude/dreamy-leavitt-ccc0d9` (commit 85e8020). This plan verifies whether it meets the AGENTS.md specification.

---

## Verdict: FAILS — 3 blocking issues

---

## ❌ Issue 1 (CRITICAL): `tests/encrypted_env.rs` does not exist

AGENTS.md requires exactly 5 tests:
- `test_encrypted_env_creates_without_error`
- `test_encrypted_env_wrong_key_length`
- `test_db_open_with_encrypted_env`
- `test_write_and_read_with_encrypted_env`
- `test_encrypted_files_unreadable_without_env` ← marked critical

**None exist.** `cargo test --features encrypted-env` will not run any of the required tests.

**Fix:** Create `tests/encrypted_env.rs` with all 5 tests.

---

## ❌ Issue 2 (CRITICAL): Type mismatch — `as_ptr()` returns wrong pointer

**File:** [`src/env.rs:252-254`](src/env.rs)

```rust
pub(crate) fn as_ptr(&self) -> *mut libc::c_void {
    self.env_struct.rep   // ← returns the inner rocksdb::Env* pointer
}
```

**File:** [`librocksdb-sys/crocksdb/crocksdb.cc:82-85`](librocksdb-sys/crocksdb/crocksdb.cc)

```c
void rocksdb_options_set_env(rocksdb_options_t* opts, rocksdb_env_t* env) {
    options->env = env ? static_cast<rocksdb::Env*>(env->rep) : nullptr;
    //                                               ^^^^^^^^
    // C expects rocksdb_env_t* and reads ->rep
}
```

The C function receives `self.env_struct.rep` (a raw `rocksdb::Env*`) but treats it as `rocksdb_env_t*`, then reads `->rep` from it — reading the vtable pointer of the C++ class. This is undefined behavior; `options->env` will be set to garbage.

Both callers have this bug:
- `db_options.rs:1421` — `rocksdb_options_set_env(self.inner, env.as_ptr())`
- `db.rs:727,778` — `rocksdb_options_set_env(opts.inner, encrypted_env.as_ptr() as *mut _)`

**Fix (Option A — simplest):** Change `as_ptr()` to return the box pointer:
```rust
pub(crate) fn as_ptr(&self) -> *mut rocksdb_env_t {
    &*self.env_struct as *const rocksdb_env_t as *mut rocksdb_env_t
}
```
Update callers to cast appropriately.

**Fix (Option B):** Change the C function to take `void*` directly as `rocksdb::Env*` without reading `->rep`, eliminating the wrapper indirection. Then `as_ptr()` returning `self.env_struct.rep` is correct.

---

## ❌ Issue 3 (SECURITY): ROT13 used — not AES-256-CTR

**File:** [`librocksdb-sys/crocksdb/crocksdb.cc:36-43`](librocksdb-sys/crocksdb/crocksdb.cc)

```cpp
// Create a ROT13 block cipher with 16-byte block size (for testing)
rocksdb::Status status = rocksdb::BlockCipher::CreateFromString(
    config_opts, "ROT13:16", &cipher_ptr);
```

The 32-byte key passed from Rust is validated and zeroed, but **never used** — ROT13 has no key. Data is XOR-rotated by 13, not AES-encrypted. AGENTS.md requires AES-256-CTR encryption.

The comment in the header also explicitly states: _"For production use with AES-256, implement a custom BlockCipher."_

This means `test_encrypted_files_unreadable_without_env` would fail (ROT13 files can be read/decoded trivially) and the feature offers no real security.

**Fix:** Use `rocksdb::NewAES256CTRCipherStream` or implement a `BlockCipher` subclass that uses the supplied 32-byte key with AES-256. RocksDB's `CTREncryptionProvider` already accepts any `BlockCipher`; only the cipher creation needs to change.

---

## ⚠️ Issue 4 (MINOR): Redundant double `rocksdb_options_set_env` call

`set_encrypted_env()` in [`src/db_options.rs:1418-1423`](src/db_options.rs) calls `rocksdb_options_set_env` and stores the env. Then `open_raw()` and `open_cf_raw()` in [`src/db.rs`](src/db.rs) call it a second time. Harmless, but contradicts the AGENTS.md design of a single integration point.

---

## Passing Criteria

| Criterion | Status |
|---|---|
| `encrypted-env` feature flag in Cargo.toml | ✅ |
| Feature propagated to `librocksdb-sys/Cargo.toml` | ✅ |
| `src/env.rs` created with `EncryptedEnv` struct | ✅ |
| C shim in `crocksdb/crocksdb.h` + `.cc` | ✅ |
| `build.rs` compiles `crocksdb.cc` under feature | ✅ |
| `src/lib.rs` re-exports `EncryptedEnv` behind feature flag | ✅ |
| No `unwrap()` in new Rust code | ✅ |
| 32-byte key validation in Rust | ✅ |
| Key material zeroed after passing to C++ | ✅ |
| All new code gated behind `#[cfg(feature = "encrypted-env")]` | ✅ |
| `Drop` impl cleans up C++ resources | ✅ |
| No new Cargo dependencies | ✅ |
| `tests/encrypted_env.rs` with 5 tests | ❌ |
| Correct pointer passed to `rocksdb_options_set_env` | ❌ |
| Actual AES-256-CTR cipher (not ROT13) | ❌ |

---

## Files to Fix

| File | Change |
|---|---|
| `src/env.rs:252-254` | Fix `as_ptr()` to return box pointer OR change C function to accept `rocksdb::Env*` directly |
| `librocksdb-sys/crocksdb/crocksdb.cc:36-43` | Replace ROT13 with AES-256 using the supplied key |
| `librocksdb-sys/crocksdb/crocksdb.h:49` | Update signature if C function changes |
| `tests/encrypted_env.rs` | Create with all 5 required tests |

---

## Verification Steps (after fixes)

```bash
cargo build                              # must pass, zero warnings
cargo build --features encrypted-env    # must pass, zero warnings
cargo test                              # must pass
cargo test --features encrypted-env     # all 5 tests must pass
cargo clippy --features encrypted-env   # zero warnings
```