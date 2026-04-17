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
#include <memory>
#include <string>
#include <openssl/evp.h>

// AES256BlockCipher - Implements BlockCipher using OpenSSL's EVP API
// This is a simple block cipher that encrypts/decrypts single blocks.
// The CTR mode logic is handled by CTREncryptionProvider/CTRCipherStream.
class AES256BlockCipher final : public rocksdb::BlockCipher {
  private:
    std::string key_;  // 32-byte AES-256 key

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