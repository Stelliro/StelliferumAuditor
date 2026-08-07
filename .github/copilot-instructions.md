# Copilot Workspace Instructions

> **MANDATORY FIRST READ:** Also read `.github/OPUS_ENGINE_INSTRUCTIONS.md` (project-specific rules for Claude Opus 4.6) before any work.

> **Mandatory. Execute in order. No skipping.**

---

## Step 1: Read Priority Context (FIRST)

1. Open and read `CODEBASE_INDEX.md` at workspace root **before** any other work.
2. It is the **single source of truth** for structure, conventions, and architecture.
3. Read all referenced instruction files before proceeding.
4. Never assume — verify against the index.
5. If `CODEBASE_INDEX.md` doesn't exist, inform the user and offer to create one.

## Step 2: Understand the Request (SECOND)

1. Read the **entire** prompt first.
2. Identify all affected files, modules, and transitive dependencies.
3. Cross-reference against documented conventions.
4. If ambiguous or conflicting, **ask before proceeding**.
5. Determine scope: bug fix, feature, refactor, docs, config, or combination.

## Step 3: Plan Before Coding (THIRD)

1. Outline: files to create/modify/delete, applicable conventions, related files (tests, configs, docs, types, migrations, CI), side effects, breaking changes, rollback path.
2. Must not violate documented conventions.
3. Flag new patterns for documentation (Step 6).
4. For large/risky changes, propose incremental steps and confirm.

## Step 4: Implement

### 4.1 Coding Standards
- Follow all conventions from `CODEBASE_INDEX.md` and linked files.
- Match existing style. Consistency over cleverness.
- Include comments/docstrings per project conventions.
- Follow established directory structure and naming.

### 4.2 Error Handling
- Never silently swallow errors — log, propagate, or surface them.
- Validate inputs at system boundaries.
- Use established error-handling patterns; don't introduce new ones undocumented.
- Fail-fast for programming errors; graceful degradation for runtime errors.
- Ensure proper resource cleanup (try-with-resources, using, defer, RAII, etc.).

### 4.3 Security
- No hard-coded secrets — use env vars or secret managers.
- Sanitize/validate all external input, especially for queries, commands, paths, markup.
- Least privilege. Prevent path traversal. Use parameterized queries.
- Respect existing auth/authz patterns.

### 4.4 Performance
- Minimize loop scope; prefer `map`/`filter`/`reduce` when clearer.
- Avoid unnecessary nested loops — use hash maps/sets for lookups.
- Extract large loop bodies into helper functions.
- No obviously inefficient patterns (O(n²) when O(n) is easy).
- Prefer lazy evaluation for large datasets. Cache repeated expensive computations.

### 4.5 Concurrency
- Follow established async/concurrent patterns.
- Protect shared mutable state; prefer immutability or message-passing.
- Avoid deadlocks: consistent lock order, small critical sections.
- Don't block async contexts with sync I/O.

### 4.6 Dependencies
- No new dependencies without noting it and confirming if required.
- Prefer existing deps over new overlapping ones.
- Organize imports per project style. Remove unused ones.

### 4.7 Backward Compatibility
- Identify breaking vs. non-breaking changes to public APIs, schemas, wire formats.
- Propose migration/deprecation paths for breaking changes.
- Add deprecation notices pointing to replacements.

## Step 5: Update All Affected Files (AFTER EVERY CHANGE)

1. **`CODEBASE_INDEX.md`** — update if structure, conventions, or architecture changed.
2. **Related files** — consumers of changed types, barrel exports, route registrations, configs, `.env.example`, migrations, ORM models, API contracts.
3. **Tests** — update or create tests for changed behavior.
4. **Docs** — READMEs, API docs, changelogs, inline docs.
5. **Config/Infra** — Dockerfiles, CI/CD, build scripts, linter configs, deploy manifests.

## Step 6: Document New Patterns

- New convention? Create an instruction file, reference it in `CODEBASE_INDEX.md`.
- Modified convention? Update the file **and** the index.
- Include: what, why, examples, counter-examples.

## Step 7: Validate Correctness

1. Mental dry-run: one happy path + one error/edge case.
2. Check boundaries: empty, null, zero, max/min, concurrent access.
3. Verify type alignment across signatures and call sites.
4. Confirm idempotency for retryable operations.
5. Ensure codebase is fully valid and buildable after the change.

## Step 8: Self-Review Checklist

- [ ] Read `CODEBASE_INDEX.md` and all referenced files first.
- [ ] Understood full prompt before starting.
- [ ] Planned change and identified all affected files.
- [ ] Code follows documented conventions.
- [ ] Errors handled explicitly.
- [ ] No hard-coded secrets or security issues.
- [ ] No unnecessary dependencies.
- [ ] Backward compatibility considered; breaking changes documented.
- [ ] `CODEBASE_INDEX.md` updated if needed.
- [ ] Related files (tests, docs, configs, types) updated.
- [ ] New patterns documented.
- [ ] Correctness validated for happy and edge cases.
- [ ] Codebase left in valid, consistent state.

---

## General Rules

- **Order matters.** Steps 1–3 before 4. Steps 5–8 after 4.
- **Never skip doc updates.** Stale docs are worse than none.
- **When in doubt, ask.**
- **Consistency over cleverness.**
- **Atomic changes** — every response leaves codebase valid.
- **Least surprise** — no hidden side effects or magic.
- **Single responsibility** per function, class, module, file.
- **DRY within reason** — eliminate true duplication, don't over-abstract.
- **Explicit over implicit** — types, config, returns, error handling.
- **Respect boundaries** — no circular deps or architectural violations.
- **No dead code** without a tracked issue or user acknowledgment.
- **Language-agnostic** — adapt terminology, keep principles.
- **Security first** — never compromise security for convenience.