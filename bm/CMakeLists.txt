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

# find_package(RocksDB CONFIG REQUIRED)
find_package(date CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
# find_package(spdlog CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(OpenMP REQUIRED)
find_package(ryml CONFIG REQUIRED)
# find_package(libgit2 CONFIG REQUIRED)

# set(PROTO_FILES
#     "${CMAKE_SOURCE_DIR}/bbserver/proto/messages/addressbook.proto"
# )
# protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

file(COPY "${CMAKE_SOURCE_DIR}/fixtures" DESTINATION ${CMAKE_CURRENT_BINARY_DIR})

function(add_bm_executable BM_NAME)

set(PROTO_FILES
    # ../bbserver/proto/messages/addressbook.proto
    ../bbserver/proto/messages/dicmeta.proto
)
message(STATUS "************* Protobuf_INCLUDE_DIRS: ${Protobuf_INCLUDE_DIRS}")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})
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

# if(benchmark_FOUND)
#   file(GLOB BM_SOURCES *_benchmark.cpp *_benchmark.cu)
#   foreach(BM_SOURCE ${BM_SOURCES})
#     get_filename_component(OUTPUT ${BM_SOURCE} NAME_WE)
#     message("building ${OUTPUT} from ${BM_SOURCE}")

#     # for each file, build a binary
#     add_executable(${OUTPUT} ${BM_SOURCE} ../gtest/testutil_host_device.cuh)

#     include_directories(${CMAKE_SOURCE_DIR})
#     target_include_directories(${OUTPUT}
#       PRIVATE ../dicutil/include
#       PRIVATE ../opencorr/include
#     )

#     target_link_libraries(
#       ${OUTPUT}
#       PRIVATE benchmark::benchmark
#       PRIVATE benchmark::benchmark_main
#       PRIVATE CCCL::CCCL
#       PRIVATE ${DIC_LIB_NAME}
#       )
#   endforeach()
# endif()
