// Copyright (c) 2024, Tyler Neely
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "crocksdb/crocksdb.h"
#include "rocksdb/env.h"
#include "rocksdb/env_encryption.h"
#include "env/env_encryption_ctr.h"
#include "rocksdb/status.h"

#ifdef ROCKSDB_ENCRYPTED_ENV
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace {

// Per-thread AES-256-ECB context, reused across calls.
//
// RocksDB's CTR stream calls BlockCipher::Encrypt once per *16-byte* block, so
// the previous implementation — EVP_CIPHER_CTX_new / EVP_EncryptInit_ex /
// EVP_CIPHER_CTX_free per call — paid a context allocation, an OpenSSL
// algorithm fetch by name, and a full AES key schedule for every 16 bytes read
// off disk. A 4 KiB RocksDB data block cost 256 of those cycles, and profiling
// (storage-stack #106) showed encrypted reads dominated by evp_generic_fetch /
// ossl_namemap_name2num / CRYPTO_zalloc rather than by AES itself.
//
// The context is created once per thread and re-keyed only when a different
// cipher instance uses it. ECB has no chaining and padding is disabled, so
// repeated EVP_EncryptUpdate calls on one context are stateless per block and
// produce byte-identical output to a freshly created context — the on-disk
// format is unchanged and existing stores stay readable.
struct AesEcbThreadCtx {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    // Identifies the cipher instance this context is currently keyed for. Uses
    // a monotonic id rather than the `this` pointer, which a later instance
    // could reuse after free and silently inherit the wrong key schedule.
    uint64_t bound_instance = 0;

    ~AesEcbThreadCtx() {
        if (ctx != nullptr) EVP_CIPHER_CTX_free(ctx);
    }
};

std::atomic<uint64_t> g_aes_instance_counter{0};

}  // namespace

// AES256BlockCipher - Implements BlockCipher using OpenSSL's EVP API
// This is a simple block cipher that encrypts/decrypts single blocks.
// The CTR mode logic is handled by CTREncryptionProvider/CTRCipherStream.
class AES256BlockCipher final : public rocksdb::BlockCipher {
  private:
    std::string key_;  // 32-byte AES-256 key
    // Non-zero, unique for the lifetime of the process; see AesEcbThreadCtx.
    uint64_t instance_id_;

  public:
    explicit AES256BlockCipher(const std::string& key)
        : key_(key),
          instance_id_(g_aes_instance_counter.fetch_add(1,
                                                        std::memory_order_relaxed) +
                       1) {}

    const char* Name() const override { return "AES256BlockCipher"; }
    size_t BlockSize() override { return 16; }

    rocksdb::Status Encrypt(char* data) override {
        thread_local AesEcbThreadCtx tls;
        if (tls.ctx == nullptr) {
            return rocksdb::Status::IOError("EVP_CIPHER_CTX_new failed");
        }

        // Key schedule and algorithm lookup happen once per thread per cipher
        // instance, not once per 16-byte block.
        if (tls.bound_instance != instance_id_) {
            if (EVP_EncryptInit_ex(tls.ctx, EVP_aes_256_ecb(), nullptr,
                                   reinterpret_cast<const unsigned char*>(key_.data()),
                                   nullptr) != 1 ||
                EVP_CIPHER_CTX_set_padding(tls.ctx, 0) != 1) {
                tls.bound_instance = 0;
                return rocksdb::Status::IOError("AES-256 ECB init failed");
            }
            tls.bound_instance = instance_id_;
        }

        int out_len = 0;
        if (EVP_EncryptUpdate(tls.ctx,
                              reinterpret_cast<unsigned char*>(data), &out_len,
                              reinterpret_cast<const unsigned char*>(data), 16) != 1) {
            // The context may be left in an indeterminate state; force a
            // re-init on this thread's next call rather than trusting it.
            tls.bound_instance = 0;
            return rocksdb::Status::IOError("AES-256 ECB encrypt failed");
        }
        return rocksdb::Status::OK();
    }

    rocksdb::Status Decrypt(char* data) override { return Encrypt(data); }
};

// CsprngCtrEncryptionProvider - AES-256-CTR provider with a CSPRNG-seeded prefix.
//
// The stock rocksdb CTREncryptionProvider::CreateNewPrefix seeds the per-file IV and initial
// counter from `Random((uint32_t)NowMicros())` — a 31-bit, time-seeded, non-cryptographic LCG.
// Under a single lifelong key that is a CTR keystream-reuse risk (storage-stack #55). Override
// CreateNewPrefix to fill the entire prefix with cryptographically secure random bytes
// (OpenSSL RAND_bytes). Decryption is unaffected and format-compatible: CreateCipherStream
// reads the IV/initial-counter back from the stored prefix regardless of how it was generated,
// so files written by the stock provider remain readable.
class CsprngCtrEncryptionProvider final : public rocksdb::CTREncryptionProvider {
  public:
    using rocksdb::CTREncryptionProvider::CTREncryptionProvider;

    const char* Name() const override { return "CsprngCtrEncryptionProvider"; }

    rocksdb::Status CreateNewPrefix(const std::string& /*fname*/, char* prefix,
                                    size_t prefixLength) const override {
        if (RAND_bytes(reinterpret_cast<unsigned char*>(prefix),
                       static_cast<int>(prefixLength)) != 1) {
            return rocksdb::Status::IOError("RAND_bytes failed generating encryption prefix");
        }
        return rocksdb::Status::OK();
    }
};

#endif  // ROCKSDB_ENCRYPTED_ENV

extern "C" {

void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len) {
#ifdef ROCKSDB_ENCRYPTED_ENV
    if (key_len != 32) return nullptr;
    std::string key_str(key, key_len);
    auto cipher  = std::make_shared<AES256BlockCipher>(key_str);
    // CSPRNG-seeded prefix (storage-stack #55) instead of the stock time-seeded one.
    auto provider = std::make_shared<CsprngCtrEncryptionProvider>(cipher);
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

// rocksdb_options_set_env is provided by db/c.cc — not duplicated here.

}  // extern "C"