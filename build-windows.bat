@echo off
setlocal enabledelayedexpansion
REM =============================================================================
REM Quanta JavaScript Engine v0.1.0 - Clang Build Script
REM =============================================================================

REM Setup logging
set "LOG_FILE=build\build.log"
set "ERROR_LOG=build\errors.log"
if not exist "build" mkdir build
echo [%date% %time%] Build started > "%LOG_FILE%"
echo. > "%ERROR_LOG%"

REM Start time
set START_TIME=%time%

cls
echo.
echo ===============================================================
echo   Building with Clang
echo ===============================================================
echo.

REM Check for Clang
echo [INFO] Checking build environment...
echo [%time%] Checking for clang++ >> "%LOG_FILE%"
where clang++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] clang++ not found in PATH
    echo [%time%] ERROR: clang++ not found >> "%LOG_FILE%"
    echo Please install LLVM from https://llvm.org/
    pause
    exit /b 1
)

REM Get Clang version
for /f "tokens=3" %%v in ('clang++ --version ^| findstr "version"') do set CLANG_VER=%%v
echo   [OK] Clang++ %CLANG_VER% detected
echo [%time%] Clang version: %CLANG_VER% >> "%LOG_FILE%"
echo.

REM Create directory structure
echo [INFO] Setting up build directories...
if not exist "build\obj\core\engine" mkdir build\obj\core\engine
if not exist "build\obj\core\gc" mkdir build\obj\core\gc
if not exist "build\obj\core\modules" mkdir build\obj\core\modules
if not exist "build\obj\core\runtime" mkdir build\obj\core\runtime
if not exist "build\obj\lexer" mkdir build\obj\lexer
if not exist "build\obj\parser" mkdir build\obj\parser
if not exist "build\bin" mkdir build\bin
echo   [OK] Directories ready
echo.

REM Compiler flags
set "CXXFLAGS=-std=c++17 -Wall -O3 -march=native -mtune=native -DQUANTA_VERSION=\"0.1.0\" -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD -funroll-loops -finline-functions -fvectorize -fslp-vectorize -msse4.2 -mavx -mavx2 -fomit-frame-pointer -fstrict-aliasing -fstrict-enums -flto=thin"
set "INCLUDES=-Iinclude"
set "LIBS=-lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32"
set "STACK=-Xlinker /STACK:67108864"

echo [%time%] Compiler flags: %CXXFLAGS% >> "%LOG_FILE%"
echo.
echo ===============================================================
echo   Compilation Phase
echo ===============================================================
echo.

REM Track compilation stats
set /a TOTAL_FILES=0
set /a COMPILED_FILES=0
set /a FAILED_FILES=0

REM Count total files
for %%f in (src\core\engine\*.cpp src\core\gc\*.cpp src\core\modules\*.cpp src\core\runtime\*.cpp src\lexer\*.cpp src\parser\*.cpp) do set /a TOTAL_FILES+=1

REM Compile core engine
echo [1/4] Compiling core engine modules...
echo [%time%] === CORE ENGINE === >> "%LOG_FILE%"
for %%f in (src\core\engine\*.cpp) do (
    set /a COMPILED_FILES+=1
    echo   [!COMPILED_FILES!/%TOTAL_FILES%] %%~nf.cpp
    clang++ %CXXFLAGS% %INCLUDES% -c %%f -o build\obj\core\engine\%%~nf.o 2>> "%ERROR_LOG%"
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAILED] %%f
        echo [%time%] ERROR compiling %%f >> "%LOG_FILE%"
        set /a FAILED_FILES+=1
        goto :build_failed
    )
    echo [%time%] OK: %%f >> "%LOG_FILE%"
)

REM Compile GC
echo [1/4] Compiling garbage collector...
echo [%time%] === GARBAGE COLLECTOR === >> "%LOG_FILE%"
for %%f in (src\core\gc\*.cpp) do (
    set /a COMPILED_FILES+=1
    echo   [!COMPILED_FILES!/%TOTAL_FILES%] %%~nf.cpp
    clang++ %CXXFLAGS% %INCLUDES% -c %%f -o build\obj\core\gc\%%~nf.o 2>> "%ERROR_LOG%"
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAILED] %%f
        echo [%time%] ERROR compiling %%f >> "%LOG_FILE%"
        set /a FAILED_FILES+=1
        goto :build_failed
    )
    echo [%time%] OK: %%f >> "%LOG_FILE%"
)

REM Compile modules
echo [1/4] Compiling core modules...
echo [%time%] === CORE MODULES === >> "%LOG_FILE%"
for %%f in (src\core\modules\*.cpp) do (
    set /a COMPILED_FILES+=1
    echo   [!COMPILED_FILES!/%TOTAL_FILES%] %%~nf.cpp
    clang++ %CXXFLAGS% %INCLUDES% -c %%f -o build\obj\core\modules\%%~nf.o 2>> "%ERROR_LOG%"
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAILED] %%f
        echo [%time%] ERROR compiling %%f >> "%LOG_FILE%"
        set /a FAILED_FILES+=1
        goto :build_failed
    )
    echo [%time%] OK: %%f >> "%LOG_FILE%"
)

REM Compile runtime
echo [2/4] Compiling runtime library...
echo [%time%] === RUNTIME === >> "%LOG_FILE%"
for %%f in (src\core\runtime\*.cpp) do (
    set /a COMPILED_FILES+=1
    echo   [!COMPILED_FILES!/%TOTAL_FILES%] %%~nf.cpp
    clang++ %CXXFLAGS% %INCLUDES% -c %%f -o build\obj\core\runtime\%%~nf.o 2>> "%ERROR_LOG%"
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAILED] %%f
        echo [%time%] ERROR compiling %%f >> "%LOG_FILE%"
        set /a FAILED_FILES+=1
        goto :build_failed
    )
    echo [%time%] OK: %%f >> "%LOG_FILE%"
)

REM Compile lexer
echo [3/4] Compiling lexer...
echo [%time%] === LEXER === >> "%LOG_FILE%"
for %%f in (src\lexer\*.cpp) do (
    set /a COMPILED_FILES+=1
    echo   [!COMPILED_FILES!/%TOTAL_FILES%] %%~nf.cpp
    clang++ %CXXFLAGS% %INCLUDES% -c %%f -o build\obj\lexer\%%~nf.o 2>> "%ERROR_LOG%"
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAILED] %%f
        echo [%time%] ERROR compiling %%f >> "%LOG_FILE%"
        set /a FAILED_FILES+=1
        goto :build_failed
    )
    echo [%time%] OK: %%f >> "%LOG_FILE%"
)

REM Compile parser
echo [4/4] Compiling parser...
echo [%time%] === PARSER === >> "%LOG_FILE%"
for %%f in (src\parser\*.cpp) do (
    set /a COMPILED_FILES+=1
    echo   [!COMPILED_FILES!/%TOTAL_FILES%] %%~nf.cpp
    clang++ %CXXFLAGS% %INCLUDES% -c %%f -o build\obj\parser\%%~nf.o 2>> "%ERROR_LOG%"
    if !ERRORLEVEL! NEQ 0 (
        echo   [FAILED] %%f
        echo [%time%] ERROR compiling %%f >> "%LOG_FILE%"
        set /a FAILED_FILES+=1
        goto :build_failed
    )
    echo [%time%] OK: %%f >> "%LOG_FILE%"
)

echo.
echo ===============================================================
echo   Linking Phase
echo ===============================================================
echo.
echo [LINK] Creating executable with ThinLTO...
echo [%time%] === LINKING === >> "%LOG_FILE%"

clang++ %CXXFLAGS% %INCLUDES% -fuse-ld=lld -DMAIN_EXECUTABLE -o build\bin\quanta.exe console.cpp build\obj\core\engine\*.o build\obj\core\gc\*.o build\obj\core\modules\*.o build\obj\core\runtime\*.o build\obj\lexer\*.o build\obj\parser\*.o %LIBS% %STACK% 2>> "%ERROR_LOG%"

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Linking failed!
    echo [%time%] ERROR: Linking failed >> "%LOG_FILE%"
    goto :build_failed
)

echo   [OK] Executable created
echo [%time%] Linking successful >> "%LOG_FILE%"

REM Calculate build time
set END_TIME=%time%
echo.
echo ===============================================================
echo   Build Success!
echo ===============================================================
echo.
echo   [OK] Files compiled: !COMPILED_FILES! / %TOTAL_FILES%
echo   [OK] Output: build\bin\quanta.exe

REM Get file size
for %%f in (build\bin\quanta.exe) do set SIZE=%%~zf
set /a SIZE_MB=SIZE/1048576
echo   [OK] Size: !SIZE_MB!MB
echo   [OK] Optimizations: O3 + ThinLTO + AVX2

echo.
echo   Build log: build\build.log
echo   Error log: build\errors.log
echo.
echo [%time%] BUILD SUCCESSFUL >> "%LOG_FILE%"
echo [%time%] Compiled files: !COMPILED_FILES!/%TOTAL_FILES% >> "%LOG_FILE%"
pause
exit /b 0

:build_failed
echo.
echo ===============================================================
echo   Build Failed!
echo ===============================================================
echo.
echo   [ERROR] Compilation stopped at file: !COMPILED_FILES! / %TOTAL_FILES%
echo   [ERROR] Check error log: build\errors.log
echo.
echo   Recent errors:
type "%ERROR_LOG%" | findstr /i "error:" | more +0
echo.
echo [%time%] BUILD FAILED >> "%LOG_FILE%"
pause
exit /b 1
