@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul

set "LOG_FILE=build\build.log"
set "ERROR_LOG=build\errors.log"
set "ERRTMP=build\_lasterr.log"
if not exist "build" mkdir build
echo [%date% %time%] Build started > "%LOG_FILE%"
type nul > "%ERROR_LOG%"

set "DIVIDER=────────────────────────────────────────────────────────────"
set "CHECK=✓"
set "CROSS=✗"

set /a WARNING_COUNT=0
set /a COMPILED_FILES=0
set /a TOTAL_PHASES=8
set /a PHASE_NUM=0

for /f %%t in ('powershell -NoProfile -Command "(Get-Date).Ticks"') do set BUILD_START=%%t

REM build-windows.bat heap-test -> build and run the GC heap unit tests only
if /i "%~1"=="heap-test" (
    if not exist "build\bin" mkdir build\bin
    clang++ -std=c++20 -Wall -g -O1 -fsanitize=address,undefined -Iinclude -o build\bin\heap-test.exe ^
        tests\gc\heap_test.cpp ^
        src\core\gc\Heap.cpp src\core\gc\HeapBlock.cpp src\core\gc\BlockAllocator.cpp ^
        2> "%ERRTMP%"
    if !ERRORLEVEL! NEQ 0 (
        call :show_error "heap-test"
        exit /b 1
    )
    build\bin\heap-test.exe
    if !ERRORLEVEL! NEQ 0 (
        echo %CROSS% heap-test reported failures
        exit /b 1
    )
    echo %CHECK% heap-test passed
    exit /b 0
)

where clang++ >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo %CROSS% clang++ not found in PATH -- install LLVM from https://llvm.org/
    exit /b 1
)
where clang >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo %CROSS% clang not found in PATH -- install LLVM from https://llvm.org/
    exit /b 1
)
for /f "tokens=3" %%v in ('clang++ --version ^| findstr "version"') do set CLANG_VER=%%v

set "CXXFLAGS=-std=c++20 -Wall -O3 -march=native -mtune=native -DQUANTA_VERSION=\"0.9.0.71926\" -DPROMISE_STABILITY_FIXED -DNATIVE_BUILD -DUTF8PROC_STATIC -DNDEBUG -funroll-loops -finline-functions -fvectorize -fslp-vectorize -msse4.2 -mavx -mavx2 -fomit-frame-pointer -fstrict-aliasing -fstrict-enums -flto=thin"
set "INCLUDES=-Iinclude -Ithird_party/pcre2/src -Ithird_party/utf8proc -Ithird_party/minicoro"
set "LIBS=-lws2_32 -lpowrprof -lsetupapi -lwinmm -lole32 -lshell32"
set "PCRE2FLAGS=-O3 -DPCRE2_CODE_UNIT_WIDTH=16 -DHAVE_CONFIG_H -Ithird_party/pcre2/src -march=native -fomit-frame-pointer"
set "UTF8PROC_FLAGS=-O3 -DUTF8PROC_STATIC -Ithird_party/utf8proc -march=native -fomit-frame-pointer"
set "STACK=-Xlinker /STACK:67108864"
echo [%time%] Compiler flags: %CXXFLAGS% >> "%LOG_FILE%"

set /a TOTAL_FILES=0
for %%f in (src\core\engine\*.cpp src\core\gc\*.cpp src\core\modules\*.cpp src\core\runtime\*.cpp src\core\vm\*.cpp src\lexer\*.cpp src\parser\*.cpp) do set /a TOTAL_FILES+=1
for /r src\core\engine\builtins %%f in (*.cpp) do set /a TOTAL_FILES+=1
for /r src\parser\ast %%f in (*.cpp) do set /a TOTAL_FILES+=1
set /a TOTAL_FILES+=32

echo %DIVIDER%
call :phase "Configure project" phase_configure
if errorlevel 1 exit /b 1
call :phase "Compile third-party" phase_thirdparty
if errorlevel 1 exit /b 1
call :phase "Compile engine" phase_engine
if errorlevel 1 exit /b 1
call :phase "Compile GC" phase_gc
if errorlevel 1 exit /b 1
call :phase "Compile runtime" phase_runtime
if errorlevel 1 exit /b 1
call :phase "Compile front-end" phase_frontend
if errorlevel 1 exit /b 1
call :phase "Compile VM" phase_vm
if errorlevel 1 exit /b 1
call :phase "Link executable" phase_link
if errorlevel 1 exit /b 1
echo %DIVIDER%

for /f %%t in ('powershell -NoProfile -Command "(Get-Date).Ticks"') do set BUILD_END=%%t
for /f %%s in ('powershell -NoProfile -Command "[math]::Round((%BUILD_END% - %BUILD_START%) / 10000000.0, 2)"') do set TOTAL_TIME=%%s
for %%f in (build\bin\quanta.exe) do set /a SIZE_MB=%%~zf/1048576

echo %CHECK% Build completed successfully
echo.
echo Profile   : release
echo Platform  : windows-%PROCESSOR_ARCHITECTURE%
echo Compiler  : clang %CLANG_VER%
echo Time      : %TOTAL_TIME%s
echo Binary    : build\bin\quanta.exe (%SIZE_MB%MB^)
echo Files     : %COMPILED_FILES%/%TOTAL_FILES%
echo Warnings  : %WARNING_COUNT%
echo Errors    : 0
echo Logs      : %LOG_FILE%, %ERROR_LOG%

echo [%time%] BUILD SUCCESSFUL >> "%LOG_FILE%"
echo [%time%] Compiled files: %COMPILED_FILES%/%TOTAL_FILES% >> "%LOG_FILE%"
exit /b 0

REM ===================================================================
REM Subroutines below -- called via `call :label`, no per-call setlocal
REM (variables set here are visible to the whole script on purpose).
REM ===================================================================

:phase
set /a PHASE_NUM+=1
for /f %%t in ('powershell -NoProfile -Command "(Get-Date).Ticks"') do set PT0=%%t
call :%~2
if errorlevel 1 exit /b 1
for /f %%t in ('powershell -NoProfile -Command "(Get-Date).Ticks"') do set PT1=%%t
for /f %%s in ('powershell -NoProfile -Command "[math]::Round((%PT1% - %PT0%) / 10000000.0, 2)"') do set PELAPSED_STR=%%s
echo [!PHASE_NUM!/%TOTAL_PHASES%] %~1 %CHECK% %PELAPSED_STR%s
goto :eof

:phase_configure
if not exist "third_party\pcre2\src\pcre2.h.generic" (
    git submodule update --init --recursive third_party/pcre2 || (echo %CROSS% PCRE2 submodule init failed & exit /b 1)
)
if not exist "third_party\utf8proc\utf8proc.c" (
    git submodule update --init --recursive third_party/utf8proc || (echo %CROSS% utf8proc submodule init failed & exit /b 1)
)
if not exist "third_party\pcre2\src\config.h" copy "third_party\pcre2_configs\config.h" "third_party\pcre2\src\config.h" >nul
if not exist "third_party\pcre2\src\pcre2.h" copy "third_party\pcre2\src\pcre2.h.generic" "third_party\pcre2\src\pcre2.h" >nul
if not exist "third_party\pcre2\src\pcre2_chartables.c" copy "third_party\pcre2\src\pcre2_chartables.c.dist" "third_party\pcre2\src\pcre2_chartables.c" >nul
if not exist "build\obj\core\engine" mkdir build\obj\core\engine
if not exist "build\obj\core\engine\builtins" mkdir build\obj\core\engine\builtins
if not exist "build\obj\core\gc" mkdir build\obj\core\gc
if not exist "build\obj\core\modules" mkdir build\obj\core\modules
if not exist "build\obj\core\runtime" mkdir build\obj\core\runtime
if not exist "build\obj\core\vm" mkdir build\obj\core\vm
if not exist "build\obj\lexer" mkdir build\obj\lexer
if not exist "build\obj\parser" mkdir build\obj\parser
if not exist "build\obj\parser\ast" mkdir build\obj\parser\ast
if not exist "build\obj\pcre2" mkdir build\obj\pcre2
if not exist "build\obj\utf8proc" mkdir build\obj\utf8proc
if not exist "build\bin" mkdir build\bin
goto :eof

:phase_thirdparty
for %%f in (third_party\pcre2\src\pcre2_auto_possess.c third_party\pcre2\src\pcre2_chartables.c third_party\pcre2\src\pcre2_chkdint.c third_party\pcre2\src\pcre2_compile.c third_party\pcre2\src\pcre2_compile_cgroup.c third_party\pcre2\src\pcre2_compile_class.c third_party\pcre2\src\pcre2_config.c third_party\pcre2\src\pcre2_context.c third_party\pcre2\src\pcre2_convert.c third_party\pcre2\src\pcre2_dfa_match.c third_party\pcre2\src\pcre2_error.c third_party\pcre2\src\pcre2_extuni.c third_party\pcre2\src\pcre2_find_bracket.c third_party\pcre2\src\pcre2_jit_compile.c third_party\pcre2\src\pcre2_maketables.c third_party\pcre2\src\pcre2_match.c third_party\pcre2\src\pcre2_match_data.c third_party\pcre2\src\pcre2_match_next.c third_party\pcre2\src\pcre2_newline.c third_party\pcre2\src\pcre2_ord2utf.c third_party\pcre2\src\pcre2_pattern_info.c third_party\pcre2\src\pcre2_script_run.c third_party\pcre2\src\pcre2_serialize.c third_party\pcre2\src\pcre2_string_utils.c third_party\pcre2\src\pcre2_study.c third_party\pcre2\src\pcre2_substitute.c third_party\pcre2\src\pcre2_substring.c third_party\pcre2\src\pcre2_tables.c third_party\pcre2\src\pcre2_ucd.c third_party\pcre2\src\pcre2_valid_utf.c third_party\pcre2\src\pcre2_xclass.c) do (
    call :compile_pcre2 "%%f" "build\obj\pcre2\%%~nf.o"
    if errorlevel 1 exit /b 1
)
call :compile_utf8proc "third_party\utf8proc\utf8proc.c" "build\obj\utf8proc\utf8proc.o"
if errorlevel 1 exit /b 1
goto :eof

:phase_engine
for %%f in (src\core\engine\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\core\engine\%%~nf.o"
    if errorlevel 1 exit /b 1
)
for /r src\core\engine\builtins %%f in (*.cpp) do (
    call :compile_cpp "%%f" "build\obj\core\engine\builtins\%%~nf.o"
    if errorlevel 1 exit /b 1
)
for %%f in (src\core\modules\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\core\modules\%%~nf.o"
    if errorlevel 1 exit /b 1
)
goto :eof

:phase_gc
for %%f in (src\core\gc\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\core\gc\%%~nf.o"
    if errorlevel 1 exit /b 1
)
goto :eof

:phase_runtime
for %%f in (src\core\runtime\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\core\runtime\%%~nf.o"
    if errorlevel 1 exit /b 1
)
goto :eof

:phase_frontend
for %%f in (src\lexer\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\lexer\%%~nf.o"
    if errorlevel 1 exit /b 1
)
for %%f in (src\parser\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\parser\%%~nf.o"
    if errorlevel 1 exit /b 1
)
for /r src\parser\ast %%f in (*.cpp) do (
    call :compile_cpp "%%f" "build\obj\parser\ast\%%~nf.o"
    if errorlevel 1 exit /b 1
)
goto :eof

:phase_vm
for %%f in (src\core\vm\*.cpp) do (
    call :compile_cpp "%%f" "build\obj\core\vm\%%~nf.o"
    if errorlevel 1 exit /b 1
)
goto :eof

:phase_link
REM cmd.exe does not expand *.o wildcards on a plain command line (only
REM inside a `for` loop does) -- build the object list explicitly first.
set "OBJLIST="
for %%d in (core\engine core\engine\builtins core\gc core\modules core\runtime core\vm lexer parser parser\ast pcre2 utf8proc) do (
    for %%f in (build\obj\%%d\*.o) do set "OBJLIST=!OBJLIST! "%%f""
)
clang++ %CXXFLAGS% %INCLUDES% -fuse-ld=lld -DMAIN_EXECUTABLE -o build\bin\quanta.exe console.cpp ^
    !OBJLIST! %LIBS% %STACK% 2> "%ERRTMP%"
set "RC=%ERRORLEVEL%"
call :fold_errors
if not "%RC%"=="0" (
    call :show_error "console.cpp (link)"
    exit /b 1
)
goto :eof

:compile_cpp
clang++ %CXXFLAGS% %INCLUDES% -c "%~1" -o "%~2" 2> "%ERRTMP%"
goto :post_compile

:compile_pcre2
clang %PCRE2FLAGS% -c "%~1" -o "%~2" 2> "%ERRTMP%"
goto :post_compile

:compile_utf8proc
clang %UTF8PROC_FLAGS% -c "%~1" -o "%~2" 2> "%ERRTMP%"
goto :post_compile

:post_compile
set "RC=%ERRORLEVEL%"
call :fold_errors
if not "%RC%"=="0" (
    call :show_error "%~1"
    exit /b 1
)
set /a COMPILED_FILES+=1
goto :eof

:fold_errors
if exist "%ERRTMP%" for %%s in ("%ERRTMP%") do if %%~zs GTR 0 (
    type "%ERRTMP%" >> "%ERROR_LOG%"
    for /f %%c in ('findstr /c:"warning:" "%ERRTMP%" 2^>nul ^| find /c /v ""') do set /a WARNING_COUNT+=%%c
)
del "%ERRTMP%" 2>nul
goto :eof

:show_error
echo.
echo %CROSS% Compile failed
echo.
echo %~1
echo.
for /f "usebackq delims=" %%l in (`findstr /c:"error:" "%ERROR_LOG%" 2^>nul`) do (
    echo %%l
    goto :eof
)
goto :eof
