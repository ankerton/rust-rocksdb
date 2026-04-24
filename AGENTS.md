# Task: Fix duplicate symbol linker error when encrypted-env feature is enabled

## Goal

Fix a duplicate symbol linker error in `librocksdb-sys` that occurs when the `encrypted-env`
feature is active (which is always the case in the Ankerton surrealdb dependency). The error
prevents `cargo test` on any crate that depends on `surrealdb-rocksdb-ankerton` from linking
in debug mode. The fix is a one-line change to `build.rs`.

## Context

`surrealdb-rocksdb-ankerton` (this crate) is pulled by the Ankerton `surrealdb` fork with
features `["lz4", "snappy", "encrypted-env"]`. The `encrypted-env` feature compiles
`crocksdb/crocksdb.cc` — the TiKV C shim — which re-exports the entire `rocksdb/db/c.cc`
C API under the same symbol names. But `db/c.cc` is also unconditionally included in
`rocksdb_lib_sources.txt`, so both `.o` files end up in `liblibrocksdb_sys`. The linker
rejects the duplicate definitions.

The fix was identified in the orchestrator pre-analysis session. It is a one-liner in
`build.rs`: filter `db/c.cc` out of `lib_sources` when `encrypted-env` is active.

## Current State

`librocksdb-sys/build.rs` line 166-172 builds `lib_sources` from `rocksdb_lib_sources.txt`
and already filters `util/build_version.cc`. It does **not** filter `db/c.cc`.

At line 403-407, `crocksdb/crocksdb.cc` is added when `encrypted-env` is enabled — but
`db/c.cc` is still in `lib_sources`, causing the collision.

**Exact linker error:**
```
/usr/bin/ld: liblibrocksdb_sys-*.rlib(crocksdb.o): in function `rocksdb_options_set_env':
crocksdb/crocksdb.cc:98: multiple definition of `rocksdb_options_set_env';
liblibrocksdb_sys-*.rlib(c.o):
rocksdb/db/c.cc:3647: first defined here
error: could not compile `app-classification` (lib test) due to 1 previous error
```

## Implementation Plan

1. Confirm submodules are populated (see Submodules section below)
2. Open `librocksdb-sys/build.rs`
3. After the `lib_sources` variable is constructed (line 172), add a `retain` filter that
   removes `db/c.cc` when `encrypted-env` is enabled
4. Verify with Podman (see Acceptance Criteria)

## Key Files

| Path | Purpose / what needs to change |
|------|-------------------------------|
| `librocksdb-sys/build.rs` | Add `db/c.cc` exclusion when `encrypted-env` is active |

## Submodules

The repo has two git submodules:

| Path | Source |
|------|--------|
| `librocksdb-sys/rocksdb` | `https://github.com/facebook/rocksdb.git` |
| `librocksdb-sys/snappy` | `https://github.com/google/snappy.git` |

`librocksdb-sys/crocksdb/` (contains `crocksdb.cc` and `crocksdb.h`) is **not** a submodule —
it is directly committed in this repo.

The clone was done with `--recurse-submodules` so submodules should already be populated.
Before building, verify:
```bash
ls librocksdb-sys/rocksdb/AUTHORS   # must exist
ls librocksdb-sys/snappy/snappy.cc  # must exist
```
If either is missing, run: `git submodule update --init --recursive`

## Dependencies

The build requires a C++ compiler and cmake. Use the `rust-builder` Podman image for all
build and test commands — do not rely on native toolchain availability.

## Shared Interface / Contract

None. This is an internal build script fix with no API surface change.

## Verification Command

This machine has no native C/C++ compiler. Run all builds via Podman:

```bash
podman run --rm \
  -v ~/workspaces/rust-rocksdb:/workspace \
  rust-builder \
  bash -lc "cargo build --manifest-path /workspace/librocksdb-sys/Cargo.toml --features encrypted-env"
```

## Acceptance Criteria

- [ ] The Verification Command above succeeds with no duplicate symbol linker errors
- [ ] No changes to public API, feature flags, or `rocksdb_lib_sources.txt`
- [ ] The filter is only active when `encrypted-env` is enabled (builds without the feature are unaffected)

## Constraints

- Only modify `librocksdb-sys/build.rs` — no other files
- Do NOT remove `db/c.cc` from `rocksdb_lib_sources.txt` — that would break non-encrypted-env builds
- Do NOT use `--allow-multiple-definition` linker flag — fix the root cause
- Do NOT modify the `crocksdb/` directory

## Notes

The `cfg!(feature = "encrypted-env")` macro works correctly in `build.rs` build scripts.
The filter belongs immediately after the `lib_sources` block is defined (after line 172) so
the exclusion is visible before the `for file in lib_sources` loop at line 397.

Exact insertion point — after this existing block:
```rust
    let mut lib_sources = include_str!("rocksdb_lib_sources.txt")
        .trim()
        .split('\n')
        .map(str::trim)
        .filter(|file| !matches!(*file, "util/build_version.cc"))
        .collect::<Vec<&'static str>>();
```
Add:
```rust
    if cfg!(feature = "encrypted-env") {
        lib_sources.retain(|f| !matches!(*f, "db/c.cc"));
    }
```

## Completion Protocol

When the task is finished:

1. Run the Verification Command above and confirm it passes
2. Commit all changes to this branch
3. Write `AGENTS-RESULT.md` at the repo root (see QWEN.md for the schema)
4. Commit `AGENTS-RESULT.md`
5. Push this branch to origin
6. Open a **draft pull request**: `cs/fix-rocksdb-debug-linker` → `main`
   ```
   gh pr create --draft --title "cs: fix-rocksdb-debug-linker" --base main
   ```
   The PR description must contain the completion state: `DONE`, `NEEDS-ITERATION`, or `BLOCKER`
7. Your job is done — do NOT merge

Declare exactly one state:
- **DONE** — Podman build passes, fix is minimal and correct
- **NEEDS-ITERATION** — build passes but something needs orchestrator attention
- **BLOCKER** — a constraint or dependency prevents the fix; document fully per QWEN.md
