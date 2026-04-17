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
#include <openssl/aes.h>
#include <openssl/evp.h>
#include "rocksdb/convenience.h"
#include "rocksdb/utilities/object_registry.h"

// AES256BlockCipher - Implements BlockCipher using OpenSSL's AES-256
// This is a simple block cipher that encrypts/decrypts single blocks.
// The CTR mode logic is handled by CTREncryptionProvider/CTRCipherStream.
class AES256BlockCipher final : public rocksdb::BlockCipher {
  private:
    std::string key_;  // 32-byte AES-256 key
    size_t block_size_;

  public:
    explicit AES256BlockCipher(const std::string& key, size_t block_size)
        : key_(key), block_size_(block_size) {
      // AES block size is always 16 bytes
    }

    size_t BlockSize() override { return 16; }

    rocksdb::Status Encrypt(char* data) override {
      // AES-256 ECB mode for single block encryption
      // The CTR mode is handled by the caller (CTRCipherStream)
      AES_KEY encrypt_key;
      if (AES_set_encrypt_key(reinterpret_cast<const unsigned char*>(key_.data()),
                              256, &encrypt_key) != 0) {
        return rocksdb::Status::IOError(
            "Failed to set AES-256 encryption key");
      }

      unsigned char ciphertext[AES_BLOCK_SIZE];
      AES_encrypt(reinterpret_cast<const unsigned char*>(data),
                  ciphertext, &encrypt_key);

      // Copy ciphertext back to data
      memcpy(data, ciphertext, AES_BLOCK_SIZE);
      return rocksdb::Status::OK();
    }

    rocksdb::Status Decrypt(char* data) override {
      // AES-256 ECB mode for single block decryption
      // Note: AES_decrypt is deprecated in OpenSSL 3.0, but we use it here
      // for compatibility with the existing RocksDB code. The CTR mode
      // is handled by the caller (CTRCipherStream).
      AES_KEY decrypt_key;
      if (AES_set_decrypt_key(reinterpret_cast<const unsigned char*>(key_.data()),
                              256, &decrypt_key) != 0) {
        return rocksdb::Status::IOError(
            "Failed to set AES-256 decryption key");
      }

      unsigned char plaintext[AES_BLOCK_SIZE];
      AES_decrypt(reinterpret_cast<const unsigned char*>(data),
                  plaintext, &decrypt_key);

      // Copy plaintext back to data
      memcpy(data, plaintext, AES_BLOCK_SIZE);
      return rocksdb::Status::OK();
    }

    // Factory method for creating AES256BlockCipher from string
    static rocksdb::Status Create(const std::string& value,
                                  std::shared_ptr<rocksdb::BlockCipher>* result,
                                  std::string* /*errmsg*/) {
      // Parse "AES256:16" format
      size_t colon_pos = value.find(':');
      size_t block_size = 16;  // Default AES block size

      if (colon_pos != std::string::npos) {
        // Extract block size from "AES256:nn" format
        std::string block_size_str = value.substr(colon_pos + 1);
        // For compatibility, we accept any block_size but always use 16 for AES
        (void)block_size_str;
      }

      // The key is set externally via AddCipher, but for our use case
      // we create a cipher with a placeholder key that will be replaced
      // by the actual key from the C++ shim
      std::string key(32, '\0');  // 32-byte key for AES-256

      result->reset(new AES256BlockCipher(key, block_size));
      return rocksdb::Status::OK();
    }

    // Return the class name for ObjectRegistry
    static const char* kClassName() { return "AES256BlockCipher"; }

    // Name() is required by Customizable base class
    const char* Name() const override { return kClassName(); }
};

// Factory function for registering with ObjectRegistry
static rocksdb::BlockCipher* AES256BlockCipherFactory(
    const std::string& /*uri*/, std::string* /*errmsg*/) {
  // This factory is called during registration, but we don't have
  // the key yet. The key will be set via AddCipher.
  std::string key(32, '\0');
  return new AES256BlockCipher(key, 16);
}

// Register AES256 cipher with the ObjectRegistry
static void RegisterAES256Cipher() {
  static std::once_flag once;
  std::call_once(once, []() {
    auto lib = rocksdb::ObjectRegistry::Default()->AddLibrary("aes256_cipher");
    lib->AddFactory<rocksdb::BlockCipher>(
        rocksdb::ObjectLibrary::PatternEntry(AES256BlockCipher::kClassName(), true)
            .AddNumber(":"),
        [](const std::string& uri, std::unique_ptr<rocksdb::BlockCipher>* guard,
           std::string* /*errmsg*/) {
          size_t colon = uri.find(':');
          size_t block_size = 16;
          if (colon != std::string::npos) {
            std::string block_size_str = uri.substr(colon + 1);
            // For compatibility, we accept any block_size but always use 16
            (void)block_size_str;
          }
          std::string key(32, '\0');
          guard->reset(new AES256BlockCipher(key, block_size));
          return guard->get();
        });
  });
}

#endif  // ROCKSDB_ENCRYPTED_ENV

extern "C" {

void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len) {
#ifdef ROCKSDB_ENCRYPTED_ENV
  // AES-256 requires exactly 32 bytes
  if (key_len != 32) {
    return nullptr;
  }

  // Register AES256 cipher if not already registered
  RegisterAES256Cipher();

  // Create AES-256 block cipher with the provided key
  std::string key_str(key, key_len);
  std::shared_ptr<rocksdb::BlockCipher> cipher_ptr =
      std::make_shared<AES256BlockCipher>(key_str, 16);

  // Create CTR encryption provider with the cipher
  auto provider =
      std::make_shared<rocksdb::CTREncryptionProvider>(cipher_ptr);

  // Store the shared_ptr on the heap and return a raw pointer
  auto* shared_ptr = new std::shared_ptr<rocksdb::EncryptionProvider>(provider);
  return static_cast<void*>(shared_ptr);
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
  auto* shared_ptr = static_cast<std::shared_ptr<rocksdb::EncryptionProvider>*>(provider);
  rocksdb::Env* encrypted_env =
      rocksdb::NewEncryptedEnv(rocksdb::Env::Default(), *shared_ptr);
  return static_cast<void*>(encrypted_env);
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