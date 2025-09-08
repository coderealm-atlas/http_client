## Summary

Describe the change and the problem it solves. Add context/links as needed.

## Type of change

- [ ] feat (new feature)
- [ ] fix (bug fix)
- [ ] refactor (no behavior change)
- [ ] tests (add/adjust tests)
- [ ] docs (README/AGENTS updates)
- [ ] chore (build/vcpkg/presets/etc.)

## How to test

Run locally:

```
cmake --preset debug
cmake --build --preset debug
ctest --output-on-failure --test-dir build
```

Run a single test (example):

```
ctest -R io_monad_test --test-dir build
```

## Checklist

- [ ] Follows Repository Guidelines (see AGENTS.md).
- [ ] Code formatted with clang-format.
- [ ] Tests added/updated and pass locally.
- [ ] No secrets/keys committed (OpenSSL, tokens, etc.).
- [ ] Build presets unaffected or updated as needed.
- [ ] If touching `error_codes.ini`, regenerated header via `gen_error.py`.

## Linked issues

Closes #

