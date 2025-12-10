# ----------------------------beast_connection_pool_test.cpp------------------------------
set(T_NAME beast_connection_pool_test)
add_executable(${T_NAME}
    beast_connection_pool_test.cpp
    ${CMAKE_SOURCE_DIR}/src/misc_util.cpp
)
target_include_directories(${T_NAME}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)
target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC 
        Boost::asio
        Boost::beast
    Boost::json
        OpenSSL::SSL
        OpenSSL::Crypto
    )
add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ----------------------------config_sources_test.cpp------------------------------
set(T_CFG config_sources_test)
add_executable(${T_CFG}
    config_sources_test.cpp
)

target_include_directories(${T_CFG}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)

target_link_libraries(
    ${T_CFG}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC
        Boost::json
        ryml::ryml
)

add_test(
        NAME ${T_CFG}.merge
        COMMAND ${T_CFG} --gtest_filter=ConfigSourcesTest.JsonOverrideMergingWorks
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_test(
        NAME ${T_CFG}.missing
        COMMAND ${T_CFG} --gtest_filter=ConfigSourcesTest.MissingJsonReturnsError
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(
        NAME ${T_CFG}.cli_overrides
        COMMAND ${T_CFG} --gtest_filter=ConfigSourcesTest.CliOverridesPropagateToAppProperties
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# ----------------------------config_sources_yaml_test.cpp------------------------------
set(T_CFG_YAML config_sources_yaml_test)
add_executable(${T_CFG_YAML} config_sources_yaml_test.cpp)
target_include_directories(${T_CFG_YAML}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)
target_link_libraries(${T_CFG_YAML}
    PUBLIC GTest::gtest GTest::gtest_main
    PUBLIC Boost::json
    PUBLIC ryml::ryml
)

# Mark as network-dependent and optionally disable via env flag
add_test(
        NAME ${T_CFG_YAML}.yaml_merge
        COMMAND ${T_CFG_YAML} --gtest_filter=ConfigSourcesYamlTest.YamlJsonMixedMergeAndEnvExpansion
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(${T_CFG_YAML}.yaml_merge PROPERTIES LABELS "network")
if("$ENV{HTTPCLIENT_SKIP_NETWORK_TESTS}" STREQUAL "1")
    set_tests_properties(${T_CFG_YAML}.yaml_merge PROPERTIES DISABLED TRUE)
endif()
# Mark as network-dependent and optionally disable via env flag
set_tests_properties(${T_CFG}.merge PROPERTIES LABELS "network")
set_tests_properties(${T_CFG}.missing PROPERTIES LABELS "network")
set_tests_properties(${T_CFG}.cli_overrides PROPERTIES LABELS "network")
if("$ENV{HTTPCLIENT_SKIP_NETWORK_TESTS}" STREQUAL "1")
    set_tests_properties(${T_CFG}.merge PROPERTIES DISABLED TRUE)
    set_tests_properties(${T_CFG}.missing PROPERTIES DISABLED TRUE)
    set_tests_properties(${T_CFG}.cli_overrides PROPERTIES DISABLED TRUE)
endif()
#
# unit tests:
#

# add_definitions(-DTEST_ENV)
add_compile_definitions(TEST_ENV)
add_compile_definitions(TEST_BUILD)
add_compile_definitions(TEST_BUILD_GTEST)
#!!!!!!!!! it's old wide behavier. using target_include_directories instead
# include_directories(${RYML_INCLUDE_DIR})
# include catch

# find_package(unofficial-minizip CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS asio)
find_package(Boost REQUIRED COMPONENTS beast)
find_package(Boost REQUIRED COMPONENTS url)
find_package(Boost REQUIRED COMPONENTS uuid)
find_package(Boost REQUIRED COMPONENTS json)
find_package(Boost REQUIRED COMPONENTS process)
find_package(Boost REQUIRED COMPONENTS iostreams)
find_package(Boost REQUIRED COMPONENTS log)
find_package(Boost REQUIRED COMPONENTS log_setup)
find_package(date CONFIG REQUIRED)
find_package(ryml CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)

# find_package(RocksDB CONFIG REQUIRED)
find_package(date CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(OpenMP REQUIRED)

add_compile_definitions(SERVER_GIT_AGENT="git/2.34.1")
add_compile_definitions(-DBOOST_PROCESS_VERSION=2)
add_compile_definitions(MAX_CHUNK_LENGTH_BYTES=8)
add_compile_definitions(RYML_DEFAULT_CALLBACK_USES_EXCEPTIONS=1)
# add_compile_definitions(-DRYML_DEFAULT_CALLBACKS=0)



option(GTEST_COLOR "Enable colored output for gtest" ON)

file(GLOB LIB_SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)

# ----------------------------httpclient_test.cpp------------------------------
set(T_NAME httpclient_test)


add_executable(${T_NAME}  
    httpclient_test.cpp
    ${LIB_SOURCES}
)
target_include_directories(${T_NAME}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)

# target_precompile_headers(${T_NAME} PRIVATE ./include/pch.hpp)
target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC 
        Boost::asio
        Boost::beast
        Boost::url
        Boost::uuid
        Boost::json
        Boost::process
        Boost::iostreams
        date::date
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        fmt::fmt-header-only
        Boost::log
        Boost::log_setup
    )
add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(${T_NAME} PROPERTIES LABELS "network")
if("$ENV{HTTPCLIENT_SKIP_NETWORK_TESTS}" STREQUAL "1")
  set_tests_properties(${T_NAME} PROPERTIES DISABLED TRUE)
endif()

# ----------------------------io_monad_test.cpp------------------------------
set(T_NAME io_monad_test)
add_executable(${T_NAME}  
    io_monad_test.cpp
    ${LIB_SOURCES}
)
target_include_directories(${T_NAME}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)

# target_precompile_headers(${T_NAME} PRIVATE ./include/pch.hpp)
target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC 
        Boost::asio
        Boost::beast
        Boost::url
        Boost::uuid
        Boost::json
        Boost::process
        Boost::iostreams
        date::date
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        fmt::fmt-header-only
    )
add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ----------------------------retry_poll_regression_test.cpp------------------------------
set(T_NAME retry_poll_regression_test)
add_executable(${T_NAME}
    retry_poll_regression_test.cpp
)
target_include_directories(${T_NAME}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)
target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC 
        Boost::asio
        Boost::beast
        Boost::url
        Boost::uuid
        Boost::json
        Boost::process
        Boost::iostreams
        date::date
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        fmt::fmt-header-only
        Boost::log
        Boost::log_setup
)
add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

#

# ----------------------------urls_test.cpp------------------------------
set(T_NAME urls_test)
add_executable(${T_NAME}  
    urls_test.cpp
)
target_include_directories(${T_NAME}
    PRIVATE ./include
)

target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC 
        Boost::url
        Boost::json
    )
add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ----------------------------json_env_substitute_test.cpp------------------------------
set(T_NAME json_env_substitute_test)
add_executable(${T_NAME}
    json_env_substitute_test.cpp
    ${CMAKE_SOURCE_DIR}/src/json_util.cpp
)

target_include_directories(${T_NAME}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)

target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC
        Boost::json
        fmt::fmt-header-only
)

add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ----------------------------envrc_parse_test.cpp------------------------------
set(T_NAME envrc_parse_test)
add_executable(${T_NAME}
    envrc_parse_test.cpp
    ${CMAKE_SOURCE_DIR}/src/misc_util.cpp
)

target_include_directories(${T_NAME}
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ./include
)

target_link_libraries(
    ${T_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PUBLIC
        Boost::json
)

add_test(
    NAME ${T_NAME}
    COMMAND ${T_NAME}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
