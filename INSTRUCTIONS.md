Build and Test Quickstart

- Primary preset: use `debug`.
- Alternative: `debug-asan` (AddressSanitizer enabled).

Commands

- Configure (Debug): `cmake --preset debug`
- Build: `cmake --build --preset debug`
- Run all tests: `ctest --output-on-failure --test-dir build`
- Run one test: `ctest -R io_monad_test --test-dir build`

ASan build

- Reconfigure with ASan: `cmake --preset debug-asan`
- Then build/test as above using the same build directory (`build`).

Network tests

- Exclude network-dependent tests: `ctest -LE network --test-dir build`
- Disable network tests at configure time: `HTTPCLIENT_SKIP_NETWORK_TESTS=1 cmake --preset debug`

Environment

- Set `VCPKG_DIR` to your vcpkg clone (presets expect it).
- Optionally set `CORES` to control parallel builds.
- If you hit ccache permission issues locally, try `export CCACHE_DISABLE=1` before building.
