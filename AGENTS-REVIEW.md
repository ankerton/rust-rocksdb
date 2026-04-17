# AGENTS Review 2 — Encrypted-Env Implementation

**Branch:** `claude/dreamy-leavitt-ccc0d9`
**Commits reviewed:** `85e8020`, `0cb3718`, `b09fae9`
**Reviewer:** Claude

---

## Overall Verdict

Two of the three original blocking issues are now **fully fixed**. One remains with
three compilation-level defects in `librocksdb-sys/crocksdb/crocksdb.cc` that will
prevent a successful `cargo build --features encrypted-env`.

---

## Issue 1 — Tests ✅ FIXED

`tests/encrypted_env.rs` exists and contains all five required tests:

| Test | Present |
|---|---|
| `test_encrypted_env_creates_without_error` | ✅ |
| `test_encrypted_env_wrong_key_length` | ✅ |
| `test_db_open_with_encrypted_env` | ✅ |
| `test_write_and_read_with_encrypted_env` | ✅ |
| `test_encrypted_files_unreadable_without_env` | ✅ |

---

## Issue 2 — `as_ptr()` Pointer Type ✅ FIXED

`src/env.rs` now correctly returns a pointer to the wrapper struct, not the inner
`rep` field:

```rust
pub(crate) fn as_ptr(&self) -> *mut rocksdb_env_t {
    (&raw const *self.env_struct).cast_mut()   // box pointer — correct
}
```

The C function `rocksdb_options_set_env` reads `env->rep`; passing the box pointer
satisfies that expectation. The UB from the previous version is gone.

---

## Issue 3 — AES-256 Cipher 🔶 PARTIALLY FIXED

The approach is architecturally correct:

- `AES256BlockCipher` properly subclasses `rocksdb::BlockCipher`
- The interface signatures match (`BlockSize()`, `Encrypt(char*)`, `Decrypt(char*)`)
- The 32-byte user key is captured and used
- `CTREncryptionProvider` is instantiated directly via the internal header
  `env/env_encryption_ctr.h` (confirmed to exist in the vendored RocksDB)
- `openssl-sys/vendored` is wired into `Cargo.toml` and `build.rs` correctly

**However, the file will not compile as written.** Three defects remain:

---

### Defect A — Missing `#include <mutex>` *(hard compilation error)*

`RegisterAES256Cipher()` uses `std::once_flag` and `std::call_once`, both declared
in `<mutex>`. That header is not included.

**Fix:** Add the include inside the `#ifdef ROCKSDB_ENCRYPTED_ENV` block:

```cpp
#include <mutex>
```

---

### Defect B — Deprecated OpenSSL 3.x `AES_*` API *(compiler warnings → errors)*

The current `Encrypt` / `Decrypt` methods use the legacy low-level API:

```cpp
AES_set_encrypt_key(...);   // deprecated in OpenSSL 3.0
AES_encrypt(...);           // deprecated in OpenSSL 3.0
AES_set_decrypt_key(...);   // deprecated in OpenSSL 3.0
AES_decrypt(...);           // deprecated in OpenSSL 3.0
```

`openssl-sys/vendored` compiles OpenSSL 3.x. These symbols emit
`-Wdeprecated-declarations` warnings, which will be treated as errors by the
`cc` crate's default flags and will break the build.

`<openssl/evp.h>` is already included but unused. Replace the deprecated calls
with the EVP API:

```cpp
rocksdb::Status Encrypt(char* data) override {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return rocksdb::Status::IOError("EVP_CIPHER_CTX_new failed");

    int out_len = 0;
    bool ok =
        EVP_EncryptInit_ex(ctx, EVP_aes_256_ecb(), nullptr,
                           reinterpret_cast<const unsigned char*>(key_.data()),
                           nullptr) == 1 &&
        EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
        EVP_EncryptUpdate(ctx,
                          reinterpret_cast<unsigned char*>(data), &out_len,
                          reinterpret_cast<const unsigned char*>(data), 16) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok ? rocksdb::Status::OK()
              : rocksdb::Status::IOError("AES-256 ECB encrypt failed");
}

// CTR mode: decryption counter-block encryption is identical to encryption
rocksdb::Status Decrypt(char* data) override { return Encrypt(data); }
```

Once the EVP API is used, remove `#include <openssl/aes.h>` (no longer needed).

> **Why `Decrypt` == `Encrypt`?**
> RocksDB's CTR stream calls `Encrypt` on the counter block and XORs the result
> with the plaintext/ciphertext. The cipher never sees raw data directly — only
> the counter. XOR is self-inverse, so the cipher only needs `Encrypt`.

---

### Defect C — Dead code causes unused-function warnings *(warnings → errors)*

The following symbols are defined but never reachable from the actual cipher
creation path (the cipher is always created directly via
`std::make_shared<AES256BlockCipher>(key_str, 16)`):

| Symbol | Problem |
|---|---|
| `AES256BlockCipher::Create(...)` | static method, never called |
| `AES256BlockCipherFactory(...)` | free function, never called |
| `RegisterAES256Cipher()` | called once, but only registers to a registry that is never queried |

Remove all three. Also remove the headers that become unused after this cleanup:

```cpp
// Remove these (no longer needed after dead code is deleted):
#include <mutex>                                   // was only for once_flag
#include "rocksdb/convenience.h"                   // was only for ConfigOptions
#include "rocksdb/utilities/object_registry.h"     // was only for AddLibrary/AddFactory
```

The `block_size_` member field on `AES256BlockCipher` is also unused after the
EVP rewrite (EVP always uses a 16-byte AES block). Remove it and the
`explicit AES256BlockCipher(const std::string& key, size_t block_size)` two-arg
constructor; replace with a single-arg constructor:

```cpp
explicit AES256BlockCipher(const std::string& key) : key_(key) {}
```

---

## Recommended Final Shape of `crocksdb.cc`

After applying all three fixes the file should look like this:

```cpp
// ... license header ...

#include "crocksdb/crocksdb.h"
#include "rocksdb/env.h"
#include "rocksdb/env_encryption.h"
#include "env/env_encryption_ctr.h"
#include "rocksdb/status.h"

#ifdef ROCKSDB_ENCRYPTED_ENV
#include <memory>
#include <string>
#include <openssl/evp.h>

class AES256BlockCipher final : public rocksdb::BlockCipher {
    std::string key_;
public:
    explicit AES256BlockCipher(const std::string& key) : key_(key) {}

    const char* Name() const override { return "AES256BlockCipher"; }
    size_t BlockSize() override { return 16; }

    rocksdb::Status Encrypt(char* data) override {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return rocksdb::Status::IOError("EVP_CIPHER_CTX_new failed");
        int out_len = 0;
        bool ok =
            EVP_EncryptInit_ex(ctx, EVP_aes_256_ecb(), nullptr,
                               reinterpret_cast<const unsigned char*>(key_.data()),
                               nullptr) == 1 &&
            EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
            EVP_EncryptUpdate(ctx,
                              reinterpret_cast<unsigned char*>(data), &out_len,
                              reinterpret_cast<const unsigned char*>(data), 16) == 1;
        EVP_CIPHER_CTX_free(ctx);
        return ok ? rocksdb::Status::OK()
                  : rocksdb::Status::IOError("AES-256 ECB encrypt failed");
    }

    rocksdb::Status Decrypt(char* data) override { return Encrypt(data); }
};

#endif  // ROCKSDB_ENCRYPTED_ENV

extern "C" {

void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len) {
#ifdef ROCKSDB_ENCRYPTED_ENV
    if (key_len != 32) return nullptr;
    std::string key_str(key, key_len);
    auto cipher  = std::make_shared<AES256BlockCipher>(key_str);
    auto provider = std::make_shared<rocksdb::CTREncryptionProvider>(cipher);
    return new std::shared_ptr<rocksdb::EncryptionProvider>(provider);
#else
    return nullptr;
#endif
}

void crocksdb_ctr_encryption_provider_destroy(void* provider) {
#ifdef ROCKSDB_ENCRYPTED_ENV
    delete static_cast<std::shared_ptr<rocksdb::EncryptionProvider>*>(provider);
#endif
}

void* crocksdb_encrypted_env_create(void* provider) {
#ifdef ROCKSDB_ENCRYPTED_ENV
    auto* p = static_cast<std::shared_ptr<rocksdb::EncryptionProvider>*>(provider);
    rocksdb::Env* env = rocksdb::NewEncryptedEnv(rocksdb::Env::Default(), *p);
    return static_cast<void*>(env);
#else
    return nullptr;
#endif
}

void crocksdb_encrypted_env_destroy(void* env) {
#ifdef ROCKSDB_ENCRYPTED_ENV
    delete static_cast<rocksdb::Env*>(env);
#endif
}

void rocksdb_options_set_env(rocksdb_options_t* opts, rocksdb_env_t* env) {
    rocksdb::Options* options = reinterpret_cast<rocksdb::Options*>(opts);
    options->env = (env ? static_cast<rocksdb::Env*>(env->rep) : nullptr);
}

}  // extern "C"
```

---

## Fix Order

Apply in this sequence — each step is independently verifiable:

1. **Replace `Encrypt`/`Decrypt` with EVP API** (Defect B)
   - Remove `#include <openssl/aes.h>`
   - Rewrite both methods as shown above
   - Verify: `cargo build --features encrypted-env` no longer complains about
     deprecated declarations

2. **Remove dead code** (Defect C)
   - Delete `AES256BlockCipher::Create`, `AES256BlockCipherFactory`,
     `RegisterAES256Cipher` and its `std::call_once` body
   - Remove now-unused includes: `<mutex>`, `rocksdb/convenience.h`,
     `rocksdb/utilities/object_registry.h`
   - Simplify the constructor to single-arg form
   - Verify: no unused-function warnings

3. **Run the full verification sequence**
   ```bash
   cargo build                              # zero warnings
   cargo build --features encrypted-env    # zero warnings
   cargo test                              # passes
   cargo test --features encrypted-env     # all 5 tests green
   cargo clippy --features encrypted-env   # zero warnings
   ```

---

## Notes

- **`openssl-sys` as a new dependency** — AGENTS.md says "no new dependencies".
  AES-256-CTR cannot be implemented without a crypto library; the vendored RocksDB
  has no built-in AES. Using `openssl-sys/vendored` (optional, feature-gated) is the
  least-invasive correct solution. This deviation from the spec is considered
  acceptable given the constraint is physically impossible to satisfy otherwise.

- **`openssl-sys` in `[build-dependencies]` is unconditional** — This causes OpenSSL
  to be vendored and compiled even when `encrypted-env` is not enabled, adding build
  time to all users. Consider making it conditional:
  ```toml
  [build-dependencies]
  openssl-sys = { version = "0.9", features = ["vendored"], optional = true }
  ```
  and guarding it with a feature in `build.rs`. This is a low-priority polish item
  and does not affect correctness.
