cmake_minimum_required(VERSION 3.16)

cmake_policy(SET CMP0104 NEW)
cmake_policy(SET CMP0146 OLD)
cmake_policy(SET CMP0167 NEW)

project(http_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_PCH_ENABLE OFF)

include(cmake/CPM.cmake)
find_package(Git REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

# ---------------- Version from Git ----------------
execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_DESCRIBE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
configure_file(${CMAKE_SOURCE_DIR}/version.h.in ${CMAKE_BINARY_DIR}/version.h)
include_directories(${CMAKE_BINARY_DIR})

message(STATUS "GIT_DESCRIBE: ${GIT_DESCRIBE}")
message(STATUS "CMAKE_BINARY_DIR: ${CMAKE_BINARY_DIR}")

# ---------------- Compiler-specific Setup ----------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    if(MSVC)  # clang-cl
        message(STATUS "Using clang-cl under MSVC.")
        add_compile_options(/clang:-ftime-trace)
    else()  # POSIX Clang (Linux/macOS)
        set(THREADS_PTHREADS_WEAK_SEARCH FALSE)
        set(CMAKE_THREAD_LIBS_INIT "-pthread")
        add_compile_options(-pthread -ftime-trace -fno-omit-frame-pointer -fno-optimize-sibling-calls)
        add_link_options(-pthread)
        message(STATUS "********************Clang POSIX flags enabled******************************")
    endif()
endif()

if(MSVC)
    message(STATUS "Configuring MSVC runtime")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Windows-specific setup
if(WIN32)
add_compile_definitions(_WIN32_WINNT=0x0602)
add_compile_definitions(-DGOOGLE_PROTOBUF_NO_CONSTINIT)
add_compile_definitions(-DC4_MSVC)
endif()

# ---------------- ASAN Setup ----------------
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    set(ASAN_FLAGS "-fsanitize=address -fno-omit-frame-pointer -g")
    add_compile_options(${ASAN_FLAGS})
    add_link_options(${ASAN_FLAGS})
else()
    message(STATUS "AddressSanitizer disabled")
endif()

# ---------------- Build Type Flags ----------------
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_definitions(-DDEBUG_BUILD)
    set(CMAKE_CXX_FLAGS_DEBUG "-O1 -gline-tables-only")
    set(CMAKE_C_FLAGS_DEBUG "-O1 -gline-tables-only")
    option(BUILD_SHARED_LIBS "Build using shared libraries" ON)

elseif(CMAKE_BUILD_TYPE STREQUAL "Test")
    add_definitions(-DTEST_ENV)
    set(CMAKE_CXX_FLAGS_TEST "-O2 -g")
    set(CMAKE_C_FLAGS_TEST "-O2 -g")

else()
    add_definitions(-DRELEASE_BUILD)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -flto=thin -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -flto=thin -DNDEBUG")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        set(CMAKE_EXE_LINKER_FLAGS_RELEASE "-fuse-ld=lld")
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
    set(OpenMP_CXX_FLAGS "-fopenmp")
    set(OpenMP_CXX_LIB_NAMES "omp")
    set(OpenMP_omp_LIBRARY omp)
endif()


# ---------------- Definitions and Flags ----------------
add_definitions(-DEIGEN_NO_DEBUG)
add_definitions(-DBOOST_PROCESS_VERSION=2)
add_compile_definitions(BUFFERSIZE=2048)

message(STATUS "Debug flags: ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "CORES: $ENV{CORES}")
message(STATUS "CMAKE_BUILD_PARALLEL_LEVEL: $ENV{CMAKE_BUILD_PARALLEL_LEVEL}")

# ---------------- Optional IWYU ----------------
if(FALSE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    find_program(IWYU_PATH NAMES include-what-you-use iwyu)
    if(IWYU_PATH)
        message(STATUS "IWYU found at ${IWYU_PATH}")
        set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE
            "${CMAKE_SOURCE_DIR}/iwyu-wrapper.sh;-Xiwyu;--mapping_file=${CMAKE_SOURCE_DIR}/iwyu_mappings.imp;")
    else()
        message(WARNING "IWYU not found")
    endif()
endif()

# ---------------- JS Insert Function ----------------
function(insert_js_into_cpp js_file cpp_file placeholder)
    file(READ ${js_file} JS_CONTENT)
    file(READ ${cpp_file} CPP_CONTENT)
    string(REGEX REPLACE "//js-content-start.*//js-content-end" "${placeholder}" CPP_CONTENT "${CPP_CONTENT}")
    string(REPLACE "${placeholder}" "${JS_CONTENT}" CPP_CONTENT "${CPP_CONTENT}")
    file(WRITE ${cpp_file} "${CPP_CONTENT}")
endfunction()

# ---------------- Source Files ----------------
file(GLOB LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)

# ---------------- Subdirectories ----------------
enable_testing()
add_subdirectory(tests)
