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

#ifndef CROCKSDB_ENCRYPTED_ENV_H
#define CROCKSDB_ENCRYPTED_ENV_H

// Include the main RocksDB C header to get ROCKSDB_LIBRARY_API
#include "rocksdb/c.h"

// Define rocksdb_env_t with the rep field so we can access it
struct rocksdb_env_t {
  void* rep;  // This will hold rocksdb::Env*
};

#ifdef __cplusplus
extern "C" {
#endif

/// Create a CTREncryptionProvider with a ROT13 cipher (for testing).
/// For production use with AES-256, implement a custom BlockCipher.
/// Returns a raw pointer to the provider. Caller owns the pointer.
/// The key parameter is used to set the block size for ROT13 cipher.
/// For AES-256, you would pass a 32-byte key and implement a custom BlockCipher.
ROCKSDB_LIBRARY_API void* crocksdb_ctr_encryption_provider_create(const char* key, size_t key_len);

/// Destroy a CTREncryptionProvider created by crocksdb_ctr_encryption_provider_create.
ROCKSDB_LIBRARY_API void crocksdb_ctr_encryption_provider_destroy(void* provider);

/// Create an EncryptedEnv wrapping the default Env with the given provider.
/// Returns a raw pointer to the EncryptedEnv. Caller owns the pointer.
ROCKSDB_LIBRARY_API void* crocksdb_encrypted_env_create(void* provider);

/// Destroy an EncryptedEnv created by crocksdb_encrypted_env_create.
ROCKSDB_LIBRARY_API void crocksdb_encrypted_env_destroy(void* env);

/// Set the Env for rocksdb_options_t.
/// This is used to set a custom Env (e.g., EncryptedEnv) on database options.
ROCKSDB_LIBRARY_API void rocksdb_options_set_env(rocksdb_options_t*, rocksdb_env_t*);

#ifdef __cplusplus
}
#endif

#endif // CROCKSDB_ENCRYPTED_ENV_H