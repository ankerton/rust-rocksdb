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
#include "rocksdb/status.h"

#ifdef ROCKSDB_ENCRYPTED_ENV
#include <memory>
#include <string>
#include "rocksdb/convenience.h"

#endif  // ROCKSDB_ENCRYPTED_ENV

extern "C" {

void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len) {
#ifdef ROCKSDB_ENCRYPTED_ENV
    // AES-256 requires exactly 32 bytes
    if (key_len != 32) {
        return nullptr;
    }
    
    // Create a ROT13 block cipher with 16-byte block size (for testing)
    // Note: ROT13 is for testing only - use proper AES cipher in production
    rocksdb::ConfigOptions config_opts;
    std::shared_ptr<rocksdb::BlockCipher> cipher_ptr;
    
    // Use CreateFromString to create ROT13 cipher: "ROT13:16" means block size 16
    rocksdb::Status status = rocksdb::BlockCipher::CreateFromString(
        config_opts, "ROT13:16", &cipher_ptr);
    
    if (!status.ok()) {
        return nullptr;
    }
    
    // Create CTR encryption provider with the cipher
    auto provider = rocksdb::EncryptionProvider::NewCTRProvider(cipher_ptr);
    
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
    rocksdb::Env* encrypted_env = rocksdb::NewEncryptedEnv(rocksdb::Env::Default(), *shared_ptr);
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

} // extern "C"
