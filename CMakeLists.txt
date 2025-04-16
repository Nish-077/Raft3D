cmake_minimum_required(VERSION 3.14)
project(Raft3D)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Find dependencies
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)

# Add external submodules (only if you haven't globally installed them)
add_subdirectory(external/crow)
add_subdirectory(external/nuraft)

# Include directories
include_directories(
    external/crow/include
    external/nuraft/include
    /usr/include        # For globally installed rocksdb headers
    src
)

# Source files
file(GLOB_RECURSE SOURCES src/*.cpp)

# Define executable
add_executable(raft3d ${SOURCES})

# Link against dependencies
target_link_libraries(raft3d
    PRIVATE
    Threads::Threads
    Boost::boost
    Crow
    NuRaft::static_lib
    rocksdb             # This assumes librocksdb.so is globally available in /usr/lib or /usr/lib/x86_64-linux-gnu
)