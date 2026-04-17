## Issue 1 — Tests ✅ FIXED

`tests/encrypted_env.rs` exists with all 5 required tests. The tests are well-structured (correct assertions, proper feature gate `#![cfg(feature = "encrypted-env")]`).

---

## Issue 2 — `as_ptr()` pointer type ✅ FIXED

`src/env.rs:252-254` now correctly returns a pointer to the wrapper struct:
```rust
pub(crate) fn as_ptr(&self) -> *mut rocksdb_env_t {
    (&raw const *self.env_struct).cast_mut()   // ✅ box pointer, not rep
}
```
The C function reads `env->rep` — this is now correct. Callers cast to `*mut libc::c_void` as needed.

---

## Issue 3 — AES cipher ❌ STILL BROKEN (differently)

The cipher string was changed from `"ROT13:16"` to `"AES256:16"`, but **`"AES256:16"` is not a registered cipher in this vendored RocksDB**. Grepping the actual `env/env_encryption.cc` confirms only `ROT13` is registered — there is no AES class. `BlockCipher::CreateFromString` will return a non-OK status, `crocksdb_ctr_encryption_provider_create` returns `nullptr`, and `EncryptedEnv::new()` will **always return `Err("Failed to create CTREncryptionProvider")`** — causing all 5 tests to fail at `EncryptedEnv::new(test_key()).unwrap()`.

Also, the 32-byte key is still **not passed to the cipher** — `CreateFromString("AES256:16", ...)` takes no key argument. The key validation and zeroing in Rust is cosmetic.

**The fix needed:** Implement a custom `BlockCipher` subclass in `crocksdb.cc` that actually uses the 32-byte key. Since the vendored RocksDB has no built-in AES and AGENTS.md forbids new dependencies, the cleanest path is a custom XOR/CTR-mode cipher class that takes the key — or fall back to a keyed ROT13 variant. Alternatively, if OpenSSL is already present in the build environment, use `EVP_aes_256_ctr`. Would you like me to implement this fix?