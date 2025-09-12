#!/bin/bash

# HTTP Client Library Build Script
# This script helps build the project and examples

set -e

echo "=== HTTP Client Library Build Script ==="
echo

# Check if build directory exists
if [ ! -d "build" ]; then
    echo "Creating build directory..."
    mkdir build
fi

cd build

echo "Configuring CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

echo "Building project..."
cmake --build . -j$(nproc)

echo "Running tests..."
ctest --output-on-failure

echo
echo "=== Build completed successfully! ==="
echo
echo "Available executables:"
echo "  Tests:"
echo "    ./tests/httpclient_test"
echo "    ./tests/io_monad_test"
echo "    ./tests/envrc_parse_test"
echo "    ./tests/json_env_substitute_test"
echo "    ./tests/urls_test"
echo "    ./tests/beast_connection_pool_test"
echo
echo "To build examples:"
echo "  1. Uncomment 'add_subdirectory(examples)' in CMakeLists.txt"
echo "  2. Run: cmake --build . --target basic_usage_example"
echo
echo "For development builds with debug info:"
echo "  cmake .. -DCMAKE_BUILD_TYPE=Debug"
echo
