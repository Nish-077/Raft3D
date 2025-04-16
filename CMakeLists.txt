cmake_minimum_required(VERSION 3.10)
project(Raft3D)

set(CMAKE_CXX_STANDARD 17)

# Crow (header-only)
include_directories(${CMAKE_SOURCE_DIR}/external/Crow/include)

# Add your source files
add_executable(raft3d
    src/main.cpp
)

# Boost is required for Crow
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

find_package(Threads REQUIRED)
target_link_libraries(raft3d PRIVATE Threads::Threads)