#!/bin/bash

# Script to build and run the Quanta JavaScript Engine

# Create build directory if it doesn't exist
mkdir -p build
cd build

# Generate build files with CMake
cmake ..

# Build the project
make -j$(nproc)

if [ $? -eq 0 ]; then
    echo "Build successful!"
    
    if [ "$1" == "run" ]; then
        # Run with example file
        echo "Running example:"
        ./quanta ../examples/hello.js
    fi
else
    echo "Build failed!"
    exit 1
fi 