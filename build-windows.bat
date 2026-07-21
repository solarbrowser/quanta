@echo off
REM =============================================================================
REM Quanta JavaScript Engine - Windows Native Build Script
REM =============================================================================

REM Check if CMake is installed
where cmake >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: CMake is not installed or not in PATH
    echo Please install CMake from https://cmake.org/download/
    pause
    exit /b 1
)

REM Create build directory
if not exist "build-cmake" mkdir build-cmake
cd build-cmake

echo.
echo [1/3] Configuring CMake project...
echo.

REM Configure with CMake - Force Visual Studio (MSVC) for native Windows build
echo Using Visual Studio generator for native MSVC compilation...
cmake -G "Visual Studio 17 2022" -A x64 ..
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Visual Studio 2022 not found, trying Visual Studio 2019...
    cmake -G "Visual Studio 16 2019" -A x64 ..
)
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Visual Studio 2019 not found, trying Visual Studio 2017...
    cmake -G "Visual Studio 15 2017" -A x64 ..
)

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: CMake configuration failed
    cd ..
    pause
    exit /b 1
)

echo.
echo [2/3] Building project...
echo.

REM Build the project
cmake --build . --config Release --parallel

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ERROR: Build failed
    cd ..
    pause
    exit /b 1
)

echo.
echo [3/3] Build completed successfully!
echo.

REM Show build output location
echo Executable location:
if exist "bin\Release\quanta.exe" (
    echo   build-cmake\bin\Release\quanta.exe
) else if exist "bin\quanta.exe" (
    echo   build-cmake\bin\quanta.exe
) else (
    echo   ERROR: Could not find executable
)

echo.
echo Library location:
if exist "lib\Release\quanta.lib" (
    echo   build-cmake\lib\Release\quanta.lib
) else if exist "lib\quanta.lib" (
    echo   build-cmake\lib\quanta.lib
) else (
    echo   ERROR: Could not find library
)

cd ..

echo.
echo ========================================
echo Build process completed!
echo ========================================
echo.

pause
