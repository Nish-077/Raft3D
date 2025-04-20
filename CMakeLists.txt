cmake_minimum_required(VERSION 3.14)
project(Raft3D)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find dependencies
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(RocksDB REQUIRED)
find_package(Catch2 REQUIRED)

# Add external submodules
add_subdirectory(external/crow)
add_subdirectory(external/nuraft)

# Common include directories and libraries
set(RAFT3D_INCLUDE_DIRS
    external/nuraft/include
    external/crow/include
    /usr/include
    /usr/local/include
    src
    include
)
set(RAFT3D_LIBS
    Threads::Threads
    Boost::boost
    Crow
    NuRaft::static_lib
    RocksDB::rocksdb
)

# Source files
file(GLOB_RECURSE SOURCES src/*.cpp)
list(REMOVE_ITEM SOURCES ${CMAKE_SOURCE_DIR}/src/main.cpp)

# Main library
add_library(raft3d_lib ${SOURCES})
target_include_directories(raft3d_lib PRIVATE ${RAFT3D_INCLUDE_DIRS})
target_link_libraries(raft3d_lib PRIVATE ${RAFT3D_LIBS})

# Main executable
add_executable(raft3d src/main.cpp)
target_include_directories(raft3d PRIVATE ${RAFT3D_INCLUDE_DIRS})
target_link_libraries(raft3d PRIVATE raft3d_lib)

# =========================
# Add tests
# =========================
enable_testing()
file(GLOB UNIT_TEST_SOURCES tests/unit/test_*.cpp)
file(GLOB INTEGRATION_TEST_SOURCES tests/integration/test_*.cpp)
set(TEST_SOURCES ${UNIT_TEST_SOURCES} ${INTEGRATION_TEST_SOURCES})

foreach(test_src ${TEST_SOURCES})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_include_directories(${test_name} PRIVATE ${RAFT3D_INCLUDE_DIRS})
    target_link_libraries(${test_name}
        PRIVATE
        raft3d_lib
        ${RAFT3D_LIBS}
        Catch2::Catch2WithMain
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()