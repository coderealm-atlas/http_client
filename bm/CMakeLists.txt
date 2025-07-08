# include google benchmark
find_package(benchmark)
find_package(fmt CONFIG REQUIRED)

find_package(Boost REQUIRED COMPONENTS asio)
find_package(Boost REQUIRED COMPONENTS beast)
find_package(Boost REQUIRED COMPONENTS url)
find_package(Boost REQUIRED COMPONENTS uuid)
find_package(Boost REQUIRED COMPONENTS json)
find_package(Boost REQUIRED COMPONENTS process)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(Boost REQUIRED COMPONENTS iostreams)
find_package(date CONFIG REQUIRED)

find_package(PkgConfig REQUIRED)

find_package(date CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(OpenMP REQUIRED)
find_package(ryml CONFIG REQUIRED)

function(add_bm_executable BM_NAME)

add_compile_definitions(SERVER_GIT_AGENT="git/2.34.1")
add_compile_definitions(MAX_CHUNK_LENGTH_BYTES=8)
add_compile_definitions(-DBOOST_PROCESS_VERSION=2)

add_executable(${BM_NAME} 
    ${BM_NAME} 
    ${BBSERVER_SOURCES}
    ${MUSTACHE_SOURCES}
    ${PROTO_SRCS}
    ${PROTO_HDRS}
    )
target_include_directories(${BM_NAME} 
    PRIVATE ../bbserver/include
    PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    PRIVATE ${MUSTACHE_INCLUDE_DIRS}
    )

target_precompile_headers(${BM_NAME} PRIVATE ../bbserver/include/pch.hpp)

target_link_libraries(
    ${BM_NAME}
    PRIVATE GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main
    PRIVATE Boost::asio
    PRIVATE Boost::beast
    PRIVATE Boost::url
    PRIVATE Boost::json
    PRIVATE Boost::uuid
    PRIVATE Boost::process
    PRIVATE Boost::iostreams
    PRIVATE Boost::program_options
    PRIVATE date::date date::date-tz
    PRIVATE ZLIB::ZLIB 
    # PRIVATE RocksDB::rocksdb
    PRIVATE OpenSSL::SSL 
    PRIVATE  OpenSSL::Crypto 
    PRIVATE protobuf::libprotoc
    PRIVATE protobuf::libprotobuf
    # PRIVATE libgit2::libgit2package
    PRIVATE ${SHARED_LIB_NAME}
    PRIVATE benchmark::benchmark
    PRIVATE benchmark::benchmark_main
    )
endfunction()

add_bm_executable(parse_one_line_bm.cpp)