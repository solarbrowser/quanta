cmake_minimum_required(VERSION 3.15)
project(Quanta VERSION 0.0.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Prefer native compilers
if(WIN32 AND NOT DEFINED CMAKE_GENERATOR_TOOLSET)
    # On Windows, prefer MSVC over MinGW
    message(STATUS "Windows detected - using native MSVC compiler for optimal performance")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/lexer/include
    ${CMAKE_SOURCE_DIR}/parser/include
)

# Compiler flags
if(MSVC)
    # MSVC (Visual Studio) flags
    add_compile_options(
        /W4                     
        /O2                     
        /Oi                     
        /Ot                     
        /GL                    
        /arch:AVX2             
        /fp:fast              
        /MP                 
        /EHsc          
        /std:c++17        
        /D_CRT_SECURE_NO_WARNINGS
        /DQUANTA_VERSION="0.0.2"
        /DPROMISE_STABILITY_FIXED
        /DNATIVE_BUILD
    )
    # Link-time optimization
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /LTCG /STACK:67108864")
    set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} /LTCG")
else()
    # GCC/Clang flags (MinGW on Windows or Linux)
    add_compile_options(
        -Wall
        -O3
        -fPIC
        -march=native
        -mtune=native
        -funroll-loops
        -finline-functions
        -finline-limit=1000
        -ftree-vectorize
        -ftree-loop-vectorize
        -faggressive-loop-optimizations
        -fomit-frame-pointer
        -ffast-math
        -fno-signed-zeros
        -fno-trapping-math
    )
    add_compile_definitions(
        QUANTA_VERSION="0.0.2"
        PROMISE_STABILITY_FIXED
        NATIVE_BUILD
    )
    # Stack size for MinGW
    if(WIN32)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--stack,67108864")
    endif()
endif()

# PCRE2 (bundled, 8-bit, Unicode + JIT)
set(PCRE2_DIR ${CMAKE_SOURCE_DIR}/third_party/pcre2/src)

# Initialize submodule and generate required files if missing
if(NOT EXISTS "${PCRE2_DIR}/pcre2.h.generic")
    find_package(Git QUIET)
    if(GIT_FOUND)
        message(STATUS "Initializing PCRE2 submodule...")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} submodule update --init third_party/pcre2
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE GIT_RESULT
        )
        if(NOT GIT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to initialize PCRE2 submodule. Run: git submodule update --init")
        endif()
    else()
        message(FATAL_ERROR "Git not found and PCRE2 submodule is missing. Run: git submodule update --init")
    endif()
endif()
if(NOT EXISTS "${PCRE2_DIR}/config.h")
    configure_file("${PCRE2_DIR}/config.h.generic" "${PCRE2_DIR}/config.h" COPYONLY)
    message(STATUS "Generated PCRE2 config.h")
endif()
if(NOT EXISTS "${PCRE2_DIR}/pcre2.h")
    configure_file("${PCRE2_DIR}/pcre2.h.generic" "${PCRE2_DIR}/pcre2.h" COPYONLY)
    message(STATUS "Generated PCRE2 pcre2.h")
endif()
if(NOT EXISTS "${PCRE2_DIR}/pcre2_chartables.c")
    configure_file("${PCRE2_DIR}/pcre2_chartables.c.dist" "${PCRE2_DIR}/pcre2_chartables.c" COPYONLY)
    message(STATUS "Generated PCRE2 pcre2_chartables.c")
endif()

set(PCRE2_SOURCES
    ${PCRE2_DIR}/pcre2_auto_possess.c
    ${PCRE2_DIR}/pcre2_chartables.c
    ${PCRE2_DIR}/pcre2_chkdint.c
    ${PCRE2_DIR}/pcre2_compile.c
    ${PCRE2_DIR}/pcre2_compile_cgroup.c
    ${PCRE2_DIR}/pcre2_compile_class.c
    ${PCRE2_DIR}/pcre2_config.c
    ${PCRE2_DIR}/pcre2_context.c
    ${PCRE2_DIR}/pcre2_convert.c
    ${PCRE2_DIR}/pcre2_dfa_match.c
    ${PCRE2_DIR}/pcre2_error.c
    ${PCRE2_DIR}/pcre2_extuni.c
    ${PCRE2_DIR}/pcre2_find_bracket.c
    ${PCRE2_DIR}/pcre2_jit_compile.c
    ${PCRE2_DIR}/pcre2_maketables.c
    ${PCRE2_DIR}/pcre2_match.c
    ${PCRE2_DIR}/pcre2_match_data.c
    ${PCRE2_DIR}/pcre2_match_next.c
    ${PCRE2_DIR}/pcre2_newline.c
    ${PCRE2_DIR}/pcre2_ord2utf.c
    ${PCRE2_DIR}/pcre2_pattern_info.c
    ${PCRE2_DIR}/pcre2_script_run.c
    ${PCRE2_DIR}/pcre2_serialize.c
    ${PCRE2_DIR}/pcre2_string_utils.c
    ${PCRE2_DIR}/pcre2_study.c
    ${PCRE2_DIR}/pcre2_substitute.c
    ${PCRE2_DIR}/pcre2_substring.c
    ${PCRE2_DIR}/pcre2_tables.c
    ${PCRE2_DIR}/pcre2_ucd.c
    ${PCRE2_DIR}/pcre2_valid_utf.c
    ${PCRE2_DIR}/pcre2_xclass.c
)

add_library(pcre2-8 STATIC ${PCRE2_SOURCES})
target_compile_definitions(pcre2-8 PRIVATE
    PCRE2_CODE_UNIT_WIDTH=8
    HAVE_CONFIG_H
    SUPPORT_PCRE2_8
    SUPPORT_UNICODE
    SUPPORT_JIT
)
target_include_directories(pcre2-8 PRIVATE ${PCRE2_DIR})
set_target_properties(pcre2-8 PROPERTIES
    C_STANDARD 99
    POSITION_INDEPENDENT_CODE ON
)

# Core source files
file(GLOB_RECURSE CORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/*.cpp
)

# Platform-specific sources
file(GLOB PLATFORM_SOURCES
    ${CMAKE_SOURCE_DIR}/src/core/platform/NativeAPI.cpp
    ${CMAKE_SOURCE_DIR}/src/core/platform/APIRouter.cpp
)

# Add platform sources
list(APPEND CORE_SOURCES ${PLATFORM_SOURCES})

# Exclude WindowsNativeAPI.cpp for now (has compilation issues with MinGW headers)
list(FILTER CORE_SOURCES EXCLUDE REGEX ".*WindowsNativeAPI\\.cpp$")

# Lexer source files
file(GLOB_RECURSE LEXER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/lexer/*.cpp
)

# Parser source files
file(GLOB_RECURSE PARSER_SOURCES
    ${CMAKE_SOURCE_DIR}/src/parser/*.cpp
)

# Create static library
add_library(quantalib STATIC
    ${CORE_SOURCES}
    ${LEXER_SOURCES}
    ${PARSER_SOURCES}
)

# Set library output name
set_target_properties(quantalib PROPERTIES
    OUTPUT_NAME quanta
    ARCHIVE_OUTPUT_NAME quanta
)

# Add include directories
target_include_directories(quantalib PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${PCRE2_DIR}
)

target_link_libraries(quantalib PUBLIC pcre2-8)

# Console executable
add_executable(quanta
    ${CMAKE_SOURCE_DIR}/console.cpp
)

# Link libraries
target_link_libraries(quanta PRIVATE quantalib)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(quanta PRIVATE
        ws2_32
        powrprof
        setupapi
        winmm
        ole32
        shell32
    )
endif()

# Optional: Install targets
install(TARGETS quanta quantalib
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY core/include/quanta
    DESTINATION include
)

install(DIRECTORY lexer/include/quanta
    DESTINATION include
)

install(DIRECTORY parser/include/quanta
    DESTINATION include
)

# Print configuration info
message(STATUS "=== Quanta JavaScript Engine Configuration ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "System: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "==========================================")
