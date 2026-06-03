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

// ── CS-070 (storage-stack #55): CSPRNG-seeded per-file encryption prefix ──────────────────

fn open_encrypted(dir: &std::path::Path) {
    let env = EncryptedEnv::new(test_key()).unwrap();
    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_encrypted_env(env);
    let db = DB::open(&opts, dir).unwrap();
    db.put(b"k", b"v").unwrap(); // identical logical content in both stores
    drop(db);
}

/// Two stores opened with the SAME key produce DIFFERENT on-disk prefixes. The first prefix
/// block holds the per-file IV/initial-counter (stored in clear). With the old time-seeded LCG
/// these could collide; with `RAND_bytes` they are independent. This is the #55 fix.
#[test]
fn test_csprng_prefix_differs_across_files() {
    let d1 = TempDir::new().unwrap();
    let d2 = TempDir::new().unwrap();
    open_encrypted(d1.path());
    open_encrypted(d2.path());

    // CURRENT is written through the encrypted env, so it carries the per-file prefix.
    let c1 = std::fs::read(d1.path().join("CURRENT")).unwrap();
    let c2 = std::fs::read(d2.path().join("CURRENT")).unwrap();
    assert!(c1.len() >= 16 && c2.len() >= 16, "encrypted CURRENT carries a prefix");
    assert_ne!(
        &c1[..16],
        &c2[..16],
        "per-file CSPRNG prefix must differ across two same-key stores (no time-seed collision)"
    );
}

/// Reopening a store written with the CSPRNG prefix reads data back correctly — the prefix is
/// decoded from the stored bytes, so the change is format-compatible.
#[test]
fn test_csprng_prefix_reopen_roundtrip() {
    let dir = TempDir::new().unwrap();
    {
        let env = EncryptedEnv::new(test_key()).unwrap();
        let mut opts = Options::default();
        opts.create_if_missing(true);
        opts.set_encrypted_env(env);
        let db = DB::open(&opts, dir.path()).unwrap();
        db.put(b"persist", b"value").unwrap();
        db.flush().unwrap();
    }
    {
        let env = EncryptedEnv::new(test_key()).unwrap();
        let mut opts = Options::default();
        opts.create_if_missing(false);
        opts.set_encrypted_env(env);
        let db = DB::open(&opts, dir.path()).unwrap();
        assert_eq!(db.get(b"persist").unwrap().as_deref(), Some(b"value".as_ref()));
    }
}