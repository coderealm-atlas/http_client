# Repository Guidelines

This repository is a C++20 HTTP client built with CMake and vcpkg. Use the guidance below to navigate the codebase and contribute changes confidently and consistently.

## Project Structure & Module Organization
- `include/` — public headers (snake_case, `.hpp`).
- `src/` — implementation `.cpp` files.
- `tests/` — GoogleTest units (files end with `_test.cpp`).
- `cmake/` — CMake helpers; `CMakePresets.json` for local/CI builds.
- `build/` — out-of-tree build directory (generated).
- Utilities: `gen_error.py` (see below), `error_codes.ini`.

## Build, Test, and Development Commands
- Configure (Debug): `cmake --preset debug`
- Build: `cmake --build --preset debug`
- Run all tests: `ctest --output-on-failure --test-dir build`
- Run one test: `ctest -R io_monad_test --test-dir build`
- Enable ASan: `cmake --preset debug-asan`
- Generate error codes header: `python3 gen_error.py -i error_codes.ini -o include/httpclient_error_codes.hpp`
Notes: Presets expect vcpkg; set `VCPKG_DIR` and optionally `CORES` for parallel builds.

## Coding Style & Naming Conventions
- Formatting: clang-format (Google-derived). Two-space indent, 80 col limit, sorted includes. Run: `clang-format -i <files>`.
- Language: C++20 (`-std=c++20`).
- Files: headers `.hpp` in `include/`; sources `.cpp` in `src/`.
- Symbols: types/functions use lower_snake_case in this codebase; keep consistency with nearby code.

## Testing Guidelines
- Framework: GoogleTest via CTest (`enable_testing()`); targets live under `tests/` and end with `_test`.
- Add new tests in `tests/`, name files `<area>_test.cpp`, and register through `tests/CMakeLists.txt` if not already globbed.
- Run locally with `ctest` from the build directory; include clear assertions and minimal fixtures.
- Network tests: external network-dependent suites are labeled `network`. Run non-network tests with `ctest -LE network --test-dir build`. To disable network tests at configure time, set `HTTPCLIENT_SKIP_NETWORK_TESTS=1 cmake --preset debug-t630`.

## Commit & Pull Request Guidelines
- Commits: follow Conventional Commits (e.g., `feat:`, `fix:`, `refactor:`) with imperative, concise subjects.
- PRs: include a summary, rationale, and scope; link issues; list notable decisions; include test evidence (commands/output) and any config changes (e.g., presets, vcpkg triplets).

## Security & Configuration Tips
- TLS: relies on OpenSSL; avoid committing secrets/cert private keys.
- Tooling: IWYU supported via `iwyu-wrapper.sh` (optional); `.clangd` points to `build/compile_commands.json`.

Agent-specific note: For any automated edits, run clang-format and `ctest` locally before opening a PR.
