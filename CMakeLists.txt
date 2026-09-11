cmake_minimum_required(VERSION 3.16)
project(db VERSION 1.0 LANGUAGES CXX)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Process src directory
add_subdirectory(src)

# Tests (GoogleTest, fetched via CMake)
enable_testing()
add_subdirectory(tests)
