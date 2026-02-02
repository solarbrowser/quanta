#!/bin/bash

set -e  

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' 

# Print functions
print_header() {
    echo -e "${BLUE}===============================================================${NC}"
    echo -e "${BLUE}  $1${NC}"
    echo -e "${BLUE}===============================================================${NC}"
}

print_success() {
    echo -e "${GREEN}[OK] $1${NC}"
}

print_error() {
    echo -e "${RED}[ERROR] $1${NC}"
}

print_info() {
    echo -e "${YELLOW}[INFO] $1${NC}"
}

# Detect OS
detect_os() {
    case "$(uname -s)" in
        Linux*)     OS='Linux';;
        Darwin*)    OS='macOS';;
        MINGW*|MSYS*|CYGWIN*)     OS='Windows';;
        *)          OS='Unknown';;
    esac
}

# Detect number of CPU cores
get_cpu_cores() {
    if [[ "$OS" == "Linux" ]]; then
        nproc
    elif [[ "$OS" == "macOS" ]]; then
        sysctl -n hw.ncpu
    elif [[ "$OS" == "Windows" ]]; then
        echo $NUMBER_OF_PROCESSORS
    else
        echo 4  # Default
    fi
}

# Build with Makefile
build_makefile() {
    print_header "Building with Makefile"

    if [ ! -f "Makefile" ]; then
        print_error "Makefile not found!"
        exit 1
    fi

    print_info "Cleaning previous build..."
    make clean 2>/dev/null || true

    print_info "Building Quanta..."
    make -j$(get_cpu_cores)

    print_success "Build completed!"

    if [[ "$OS" == "Windows" ]]; then
        print_info "Executable: build/bin/quanta.exe"
    else
        print_info "Executable: build/bin/quanta"
    fi
    print_info "Library: build/libquanta.a"
}

# Build with CMake
build_cmake() {
    print_header "Building with CMake"

    # Check if CMake is installed
    if ! command -v cmake &> /dev/null; then
        print_error "CMake is not installed!"
        echo "Install CMake from https://cmake.org/download/"
        exit 1
    fi

    # Create and enter build directory
    BUILD_DIR="build-cmake"
    print_info "Creating build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"

    # Configure
    print_info "Configuring CMake project..."

    if [[ "$OS" == "Windows" ]]; then
        if command -v ninja &> /dev/null; then
            print_info "Using Ninja generator..."
            cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release ..
        else
            print_info "Using MinGW Makefiles generator..."
            cmake -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release ..
        fi
    else
        # Unix Makefiles for Linux/macOS
        cmake -DCMAKE_BUILD_TYPE=Release ..
    fi

    # Build
    print_info "Building project..."
    if [[ "$OS" == "Windows" ]]; then
        if command -v ninja &> /dev/null; then
            ninja
        else
            mingw32-make -j$(get_cpu_cores)
        fi
    else
        make -j$(get_cpu_cores)
    fi

    cd ..

    print_success "Build completed!"

    if [[ "$OS" == "Windows" ]]; then
        print_info "Executable: build-cmake/bin/quanta.exe"
        print_info "Library: build-cmake/lib/libquanta.a"
    else
        print_info "Executable: build-cmake/bin/quanta"
        print_info "Library: build-cmake/lib/libquanta.a"
    fi
}

# Show help
show_help() {
    echo "Usage: ./build.sh [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  make, makefile       Build using Makefile (default)"
    echo "  cmake                Build using CMake"
    echo "  clean                Clean all build artifacts"
    echo "  help, -h, --help     Show this help message"
    echo ""
    echo "Examples:"
    echo "  ./build.sh           # Build with Makefile"
    echo "  ./build.sh cmake     # Build with CMake"
    echo "  ./build.sh clean     # Clean all builds"
    echo ""
}

# Clean build artifacts
clean_all() {
    print_header "Cleaning Build Artifacts"

    print_info "Removing Makefile build artifacts..."
    make clean 2>/dev/null || true
    rm -rf build

    print_info "Removing CMake build artifacts..."
    rm -rf build-cmake

    print_success "All build artifacts removed!"
}

# Main script
main() {
    # Detect OS
    detect_os
    print_info "Detected OS: $OS"

    # Parse command line arguments
    BUILD_SYSTEM="makefile"

    for arg in "$@"; do
        case $arg in
            make|makefile)
                BUILD_SYSTEM="makefile"
                ;;
            cmake)
                BUILD_SYSTEM="cmake"
                ;;
            clean)
                clean_all
                exit 0
                ;;
            help|-h|--help)
                show_help
                exit 0
                ;;
            *)
                print_error "Unknown option: $arg"
                show_help
                exit 1
                ;;
        esac
    done

    # Print header
    echo ""
    print_header "Building Quanta"
    echo ""

    # Build
    if [[ "$BUILD_SYSTEM" == "cmake" ]]; then
        build_cmake
    else
        build_makefile
    fi

    echo ""
    print_success "All done!"
    echo ""
}

# Run main function
main "$@"
