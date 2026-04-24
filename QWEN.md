# QWEN.md — surrealdb-rocksdb-ankerton

Project-wide rules for every coding session on this repo.

- Only modify files listed in AGENTS.md → Key Files. No exceptions.
- If a build or test error originates in a file outside Key Files (submodule source, vendored code, dependency crate), declare a BLOCKER immediately. Do NOT attempt to fix it.
  The BLOCKER must include: exact error output, file and line, root cause analysis, proposed fix, and complexity assessment (trivial / structural).
- Do not add dependencies unless explicitly listed in AGENTS.md
- No unwrap() — use Result<T, E> throughout (tests excepted)
- Prefer minimal patches — do not refactor beyond the task scope
- No native cargo or gcc — this machine has no C/C++ compiler. All builds and tests run via
  Podman using the `rust-builder` image. Use the exact command from AGENTS.md → Verification Command.

## AGENTS-RESULT.md Schema

Commit this file at the repo root as your final step before pushing.

```markdown
# Agent Result

## Status
<DONE | NEEDS-ITERATION | BLOCKER>

## Summary
<One paragraph: what was implemented or attempted.>

## Acceptance Criteria
| Criterion | Met | Notes |
|-----------|-----|-------|
| <criterion> | ✅ / ❌ | <note> |

## Verification
| Check | Result |
|-------|--------|
| Build | ✅ passed / ❌ failed |
| Tests | ✅ passed / ❌ failed — <N> failures |
| Lint  | ✅ passed / ❌ failed |

## Changes
| Action | Path | Reason |
|--------|------|--------|
| created / modified / deleted | `path/to/file` | <why> |

## Notes
<Trade-offs, decisions, things for the orchestrator to be aware of. Empty if nothing to flag.>

## Blocker
<Only present when Status is BLOCKER.>

**What is blocked:** <specific step that cannot proceed>
**Scope:** in-scope | out-of-scope
**Error output:**
```
<exact error — full, untruncated>
```
**File:** `<path>` line <N>
**Root cause:** <analysis>
**Proposed fix:** <what to change and how>
**Complexity:** trivial | structural
**What needs to change:** <what the orchestrator must decide>
```
