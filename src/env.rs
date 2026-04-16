use std::sync::Arc;

use libc::{self, c_int, c_void};

use crate::{ffi, Error};

#[cfg(feature = "encrypted-env")]
use std::ptr::NonNull;

/// A C-compatible rocksdb_env_t struct for use with EncryptedEnv.
/// This is defined here because the FFI bindings from rocksdb/c.h define
/// rocksdb_env_t as an opaque pointer, but we need the rep field to store
/// the C++ Env pointer.
#[repr(C)]
pub struct rocksdb_env_t {
    pub rep: *mut c_void,
}

/// An Env is an interface used by the rocksdb implementation to access
/// operating system functionality like the filesystem etc. Callers
/// may wish to provide a custom Env object when opening a database to
/// get fine gain control; e.g., to rate limit file system operations.
///
/// All Env implementations are safe for concurrent access from
/// multiple threads without any external synchronization.
///
/// Note: currently, C API behinds C++ API for various settings.
/// See also: `rocksdb/include/env.h`
#[derive(Clone)]
pub struct Env(pub(crate) Arc<EnvWrapper>);

pub(crate) struct EnvWrapper {
    pub(crate) inner: *mut ffi::rocksdb_env_t,
}

impl Drop for EnvWrapper {
    fn drop(&mut self) {
        unsafe {
            ffi::rocksdb_env_destroy(self.inner);
        }
    }
}

impl Env {
    /// Returns default env
    pub fn new() -> Result<Self, Error> {
        let env = unsafe { ffi::rocksdb_create_default_env() };
        if env.is_null() {
            Err(Error::new("Could not create mem env".to_owned()))
        } else {
            Ok(Self(Arc::new(EnvWrapper { inner: env })))
        }
    }

    /// Returns a new environment that stores its data in memory and delegates
    /// all non-file-storage tasks to base_env.
    pub fn mem_env() -> Result<Self, Error> {
        let env = unsafe { ffi::rocksdb_create_mem_env() };
        if env.is_null() {
            Err(Error::new("Could not create mem env".to_owned()))
        } else {
            Ok(Self(Arc::new(EnvWrapper { inner: env })))
        }
    }

    /// Returns a new environment which wraps and takes ownership of the provided
    /// raw environment.
    ///
    /// # Safety
    ///
    /// Ownership of `env` is transferred to the returned Env, which becomes
    /// responsible for freeing it. The caller should forget the raw pointer
    /// after this call.
    ///
    /// # When would I use this?
    ///
    /// RocksDB's C++ [Env](https://github.com/facebook/rocksdb/blob/main/include/rocksdb/env.h)
    /// class provides many extension points for low-level database subsystems, such as file IO.
    /// These subsystems aren't covered within the scope of the C interface or this crate,
    /// but from_raw() may be used to hand a pre-instrumented Env to this crate for further use.
    ///
    pub unsafe fn from_raw(env: *mut ffi::rocksdb_env_t) -> Self {
        Self(Arc::new(EnvWrapper { inner: env }))
    }

    /// Sets the number of background worker threads of a specific thread pool for this environment.
    /// `LOW` is the default pool.
    ///
    /// Default: 1
    pub fn set_background_threads(&mut self, num_threads: c_int) {
        unsafe {
            ffi::rocksdb_env_set_background_threads(self.0.inner, num_threads);
        }
    }

    /// Sets the size of the high priority thread pool that can be used to
    /// prevent compactions from stalling memtable flushes.
    pub fn set_high_priority_background_threads(&mut self, n: c_int) {
        unsafe {
            ffi::rocksdb_env_set_high_priority_background_threads(self.0.inner, n);
        }
    }

    /// Sets the size of the low priority thread pool that can be used to
    /// prevent compactions from stalling memtable flushes.
    pub fn set_low_priority_background_threads(&mut self, n: c_int) {
        unsafe {
            ffi::rocksdb_env_set_low_priority_background_threads(self.0.inner, n);
        }
    }

    /// Sets the size of the bottom priority thread pool that can be used to
    /// prevent compactions from stalling memtable flushes.
    pub fn set_bottom_priority_background_threads(&mut self, n: c_int) {
        unsafe {
            ffi::rocksdb_env_set_bottom_priority_background_threads(self.0.inner, n);
        }
    }

    /// Wait for all threads started by StartThread to terminate.
    pub fn join_all_threads(&mut self) {
        unsafe {
            ffi::rocksdb_env_join_all_threads(self.0.inner);
        }
    }

    /// Lowering IO priority for threads from the specified pool.
    pub fn lower_thread_pool_io_priority(&mut self) {
        unsafe {
            ffi::rocksdb_env_lower_thread_pool_io_priority(self.0.inner);
        }
    }

    /// Lowering IO priority for high priority thread pool.
    pub fn lower_high_priority_thread_pool_io_priority(&mut self) {
        unsafe {
            ffi::rocksdb_env_lower_high_priority_thread_pool_io_priority(self.0.inner);
        }
    }

    /// Lowering CPU priority for threads from the specified pool.
    pub fn lower_thread_pool_cpu_priority(&mut self) {
        unsafe {
            ffi::rocksdb_env_lower_thread_pool_cpu_priority(self.0.inner);
        }
    }

    /// Lowering CPU priority for high priority thread pool.
    pub fn lower_high_priority_thread_pool_cpu_priority(&mut self) {
        unsafe {
            ffi::rocksdb_env_lower_high_priority_thread_pool_cpu_priority(self.0.inner);
        }
    }
}

unsafe impl Send for EnvWrapper {}
unsafe impl Sync for EnvWrapper {}

// FFI declarations for EncryptedEnv (only when feature is enabled)
#[cfg(feature = "encrypted-env")]
pub mod ffi_encrypted {
    use crate::ffi;
    use libc::{self, c_char, size_t};

    extern "C" {
        /// Create a CTREncryptionProvider with the given 32-byte key.
        /// Returns a raw pointer to the provider. Caller owns the pointer.
        /// Key must be exactly 32 bytes (AES-256).
        pub fn crocksdb_ctr_encryption_provider_create(
            key: *const c_char,
            key_len: size_t,
        ) -> *mut libc::c_void;

        /// Destroy a CTREncryptionProvider created by crocksdb_ctr_encryption_provider_create.
        pub fn crocksdb_ctr_encryption_provider_destroy(
            provider: *mut libc::c_void,
        );

        /// Create an EncryptedEnv wrapping the default Env with the given provider.
        /// Returns a raw pointer to the EncryptedEnv. Caller owns the pointer.
        pub fn crocksdb_encrypted_env_create(
            provider: *mut libc::c_void,
        ) -> *mut libc::c_void;

        /// Destroy an EncryptedEnv created by crocksdb_encrypted_env_create.
        pub fn crocksdb_encrypted_env_destroy(
            env: *mut libc::c_void,
        );

        /// Set the Env for rocksdb_options_t.
        /// This is used to set a custom Env (e.g., EncryptedEnv) on database options.
        pub fn rocksdb_options_set_env(
            options: *mut ffi::rocksdb_options_t,
            env: *mut libc::c_void,
        );
    }
}

/// A handle to a RocksDB EncryptedEnv.
/// When passed to Options, all I/O through this DB instance is AES-256-CTR encrypted.
/// The key is consumed at construction — it is zeroed in memory after being passed to C++.
#[cfg(feature = "encrypted-env")]
pub struct EncryptedEnv {
    provider_ptr: NonNull<libc::c_void>,
    env_struct: Box<rocksdb_env_t>,
}

#[cfg(feature = "encrypted-env")]
impl EncryptedEnv {
    /// Create a new EncryptedEnv with a 32-byte AES-256 key.
    /// The key buffer is zeroed after use.
    /// Returns an error if key length is not exactly 32 bytes.
    pub fn new(mut key: Vec<u8>) -> Result<Self, String> {
        if key.len() != 32 {
            return Err(format!(
                "EncryptedEnv key must be exactly 32 bytes, got {}",
                key.len()
            ));
        }

        let provider_ptr = unsafe {
            ffi_encrypted::crocksdb_ctr_encryption_provider_create(
                key.as_ptr() as *const libc::c_char,
                key.len(),
            )
        };

        // Zero the key immediately after passing to C++
        for b in &mut key {
            *b = 0;
        }
        drop(key);

        let provider_ptr = NonNull::new(provider_ptr)
            .ok_or_else(|| "Failed to create CTREncryptionProvider".to_string())?;

        let env_raw = unsafe {
            ffi_encrypted::crocksdb_encrypted_env_create(provider_ptr.as_ptr())
        };

        let env_raw = NonNull::new(env_raw)
            .ok_or_else(|| "Failed to create EncryptedEnv".to_string())?;

        // Create a rocksdb_env_t struct with the rep field set to the C++ Env pointer
        let env_struct = Box::new(rocksdb_env_t { rep: env_raw.as_ptr() });

        Ok(Self { provider_ptr, env_struct })
    }

    /// Returns the raw env pointer for passing to Options.
    /// Do not free this pointer manually — Drop handles cleanup.
    pub(crate) fn as_ptr(&self) -> *mut rocksdb_env_t {
        // Use raw pointer to avoid clippy warnings about borrow_as_ptr and ptr_cast_constness
        (&raw const *self.env_struct).cast_mut()
    }
}

#[cfg(feature = "encrypted-env")]
impl Drop for EncryptedEnv {
    fn drop(&mut self) {
        unsafe {
            // The env_struct is dropped here, but the C++ EncryptedEnv is still alive
            // The crocksdb_encrypted_env_destroy will be called by the FFI function
            // which takes ownership of the C++ Env pointer
            ffi_encrypted::crocksdb_encrypted_env_destroy(self.env_struct.rep);
            ffi_encrypted::crocksdb_ctr_encryption_provider_destroy(self.provider_ptr.as_ptr());
        }
    }
}

// EncryptedEnv is not Clone or Copy — the raw pointer must not be aliased.
#[cfg(feature = "encrypted-env")]
unsafe impl Send for EncryptedEnv {}
