cmake_minimum_required(VERSION 3.14)
project(Raft3D)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Boost is required for Crow
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

# Add external dependencies
add_subdirectory(external/crow)
add_subdirectory(external/nuraft)
add_subdirectory(external/rocksdb)

# Include directories
include_directories(
    external/crow/include
    external/nuraft/include
    external/rocksdb/include
    src
)

# Source files
file(GLOB_RECURSE SOURCES src/*.cpp)

# Define executable
add_executable(raft3d ${SOURCES})

# Link libraries
target_link_libraries(raft3d
    PRIVATE
    Threads::Threads
    Boost::boost
    crow
    nuraft
    rocksdb
)