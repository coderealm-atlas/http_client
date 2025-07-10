set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CMAKE_CXX_STANDARD 20)
set(VCPKG_CMAKE_CXX_STANDARD_REQUIRED ON)

# Use dynamic CRT (runtime) linkage (for MSVC; has no effect on Linux)
set(VCPKG_CRT_LINKAGE dynamic)

# Use static library linking for all dependencies
set(VCPKG_LIBRARY_LINKAGE static)

set(CMAKE_C_COMPILER "clang")
set(CMAKE_CXX_COMPILER "clang++")