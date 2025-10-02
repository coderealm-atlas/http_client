# run_all_tests.sh - Test Runner Guide

## Overview
This script builds and runs all tests for the http_client project using CMake and CTest.

## Key Adaptations for This Project
- **Test directory**: Changed from `gtest/` to `tests/`
- **Build directory**: `build/tests/` for test executables
- **Default runner**: CTest (not manual runner) since this project uses CTest
- **Default preset**: `debug-asan` with AddressSanitizer enabled

## Quick Usage

```bash
# Build and run all tests (default: uses CTest)
./run_all_tests.sh

# Only build tests (no execution)
./run_all_tests.sh -b

# Only run tests (assumes already built)
./run_all_tests.sh -r

# List available tests
./run_all_tests.sh -l

# Run with specific preset
./run_all_tests.sh -p debug

# Run specific test pattern
./run_all_tests.sh -f "io_monad.*"

# Build with more jobs
./run_all_tests.sh -j 16

# Verbose output
./run_all_tests.sh -v

# Use manual test runner instead of CTest
./run_all_tests.sh -M

# Build each test target separately (useful for debugging build failures)
./run_all_tests.sh --each
```

## Features

### Build Options
- `-b, --build-only`: Only build, don't run tests
- `-j N, --jobs N`: Parallel build jobs (default: $(nproc))
- `-p NAME, --preset NAME`: CMake preset (default: debug-asan)
- `--each`: Build each test target separately
- `--targets "a b c"`: Build specific test targets

### Run Options
- `-r, --run-only`: Only run tests (skip build)
- `-C, --ctest`: Use CTest runner (default)
- `-M, --manual`: Use manual test runner
- `-f REGEX, --filter REGEX`: Filter tests by regex pattern
- `-v, --verbose`: Verbose output

### Info Options
- `-l, --list`: List available test sources and built executables
- `-h, --help`: Show help message

## Examples

```bash
# Quick test of a specific test after code change
./run_all_tests.sh -f io_monad_test

# Full verbose rebuild with CTest
./run_all_tests.sh -v -j 8

# Build only, no run (e.g., for CI artifact generation)
./run_all_tests.sh -b

# Run only pre-built tests with filter
./run_all_tests.sh -r -f "json.*"
```

## Integration with VS Code

You can add this to `.vscode/tasks.json`:

```json
{
  "label": "Run All Tests",
  "type": "shell",
  "command": "${workspaceFolder}/run_all_tests.sh",
  "problemMatcher": ["$gcc"],
  "group": {
    "kind": "test",
    "isDefault": true
  }
}
```

## Notes

- The script defaults to `debug-asan` preset which includes AddressSanitizer
- CTest is the default runner since this project uses `enable_testing()`
- Test executables are built in `build/tests/`
- The script automatically detects and configures if the build directory doesn't exist
