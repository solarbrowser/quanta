cmake_minimum_required(VERSION 3.10)
project(quanta VERSION 0.0.1 LANGUAGES CXX)

# Set C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(include)

# Collect source files
file(GLOB_RECURSE SOURCES
    "src/*.cpp"
    "src/lexer/*.cpp"
    "src/parser/*.cpp"
    "src/interpreter/*.cpp"
    "src/runtime/*.cpp"
    "src/memory/*.cpp"
)

# Create executable
add_executable(quanta ${SOURCES})

# Add compile options
target_compile_options(quanta PRIVATE -Wall -Wextra) 