#![cfg(feature = "encrypted-env")]

use rocksdb::{DB, EncryptedEnv, Options};
use tempfile::TempDir;

/// Create a 32-byte test key. Do not use in production.
fn test_key() -> Vec<u8> {
    vec![0x42u8; 32]
}

#[test]
fn test_encrypted_env_creates_without_error() {
    let env = EncryptedEnv::new(test_key());
    assert!(env.is_ok(), "EncryptedEnv creation failed: {:?}", env.err());
}

#[test]
fn test_encrypted_env_wrong_key_length() {
    let result = EncryptedEnv::new(vec![0u8; 16]);
    assert!(result.is_err());
    match result {
        Err(msg) => assert!(msg.contains("32 bytes")),
        Ok(_) => panic!("Expected error, got Ok"),
    }
}

#[test]
fn test_db_open_with_encrypted_env() {
    let dir = TempDir::new().unwrap();
    let env = EncryptedEnv::new(test_key()).unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_encrypted_env(env);

    let db = DB::open(&opts, dir.path());
    assert!(db.is_ok(), "DB::open with EncryptedEnv failed: {:?}", db.err());
}

#[test]
fn test_write_and_read_with_encrypted_env() {
    let dir = TempDir::new().unwrap();
    let env = EncryptedEnv::new(test_key()).unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_encrypted_env(env);

    let db = DB::open(&opts, dir.path()).unwrap();
    db.put(b"key1", b"value1").unwrap();
    let result = db.get(b"key1").unwrap();
    assert_eq!(result.as_deref(), Some(b"value1".as_ref()));
    drop(db);
}

#[test]
fn test_encrypted_files_unreadable_without_env() {
    let dir = TempDir::new().unwrap();

    // Write with encrypted env
    {
        let env = EncryptedEnv::new(test_key()).unwrap();
        let mut opts = Options::default();
        opts.create_if_missing(true);
        opts.set_encrypted_env(env);
        let db = DB::open(&opts, dir.path()).unwrap();
        db.put(b"secret", b"data").unwrap();
    }

    // Attempt to open without encrypted env — should fail or return garbage
    {
        let mut opts = Options::default();
        opts.create_if_missing(false);
        let result = DB::open(&opts, dir.path());
        // Either open fails entirely, or the key is not found, or the value is corrupt
        if let Ok(db) = result {
            let val = db.get(b"secret").unwrap();
            // Must not return the plaintext value
            assert_ne!(
                val.as_deref(),
                Some(b"data".as_ref()),
                "Encrypted data was readable without EncryptedEnv — encryption is not working"
            );
        }
        // Open failing entirely is also an acceptable outcome
    }
}