// Copyright 2024 SurrealDB Contributors
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

use crate::ffi;

/// SST file manager for monitoring and managing SST file space usage.
///
/// This struct wraps RocksDB's SST file manager functionality, allowing
/// applications to monitor and enforce disk space limits.
pub struct SstFileManager {
    inner: *mut ffi::rocksdb_sst_file_manager_t,
    /// The env is kept alive here — RocksDB stores a raw pointer to it internally.
    env: *mut ffi::rocksdb_env_t,
}

unsafe impl Send for SstFileManager {}
unsafe impl Sync for SstFileManager {}

impl SstFileManager {
    /// Create a new SST file manager using the default environment.
    ///
    /// # Returns
    /// Result containing the new SST file manager, or an error if creation failed.
    pub fn new() -> Result<Self, crate::Error> {
        let env = unsafe { ffi::rocksdb_create_default_env() };
        if env.is_null() {
            return Err(crate::Error::new("Failed to create default Env".to_owned()));
        }

        let inner = unsafe { ffi::rocksdb_sst_file_manager_create(env) };
        if inner.is_null() {
            unsafe { ffi::rocksdb_env_destroy(env) };
            return Err(crate::Error::new("Failed to create SstFileManager".to_owned()));
        }

        Ok(Self { inner, env })
    }

    /// Get the total size of all SST files managed by this manager.
    ///
    /// # Returns
    /// Total size in bytes.
    pub fn get_total_size(&self) -> u64 {
        unsafe { ffi::rocksdb_sst_file_manager_get_total_size(self.inner) }
    }

    /// Set the maximum allowed space usage for SST files.
    ///
    /// # Arguments
    /// * `max_bytes` - Maximum allowed space in bytes. 0 means unlimited.
    pub fn set_max_allowed_space_usage(&self, max_bytes: u64) {
        unsafe { ffi::rocksdb_sst_file_manager_set_max_allowed_space_usage(self.inner, max_bytes) }
    }

    /// Check if the maximum allowed space has been reached.
    ///
    /// # Returns
    /// true if the maximum allowed space has been reached, false otherwise.
    pub fn is_max_allowed_space_reached(&self) -> bool {
        unsafe { ffi::rocksdb_sst_file_manager_is_max_allowed_space_reached(self.inner) }
    }

    /// Get the inner pointer for FFI operations.
    ///
    /// # Returns
    /// Raw pointer to the underlying SST file manager.
    pub(crate) fn inner_ptr(&self) -> *mut ffi::rocksdb_sst_file_manager_t {
        self.inner
    }
}

impl Drop for SstFileManager {
    fn drop(&mut self) {
        unsafe {
            ffi::rocksdb_sst_file_manager_destroy(self.inner);
            ffi::rocksdb_env_destroy(self.env);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_sst_file_manager_creation() {
        let sfm = SstFileManager::new();
        assert!(sfm.is_ok());
    }

    #[test]
    fn test_sst_file_manager_size() {
        let sfm = SstFileManager::new().unwrap();
        let size = sfm.get_total_size();
        assert_eq!(size, 0);
    }
}