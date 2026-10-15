@echo off
REM ============================================================
REM  Build ofd_renderer.dll - the C ABI / C++ facade over
REM  invoice-engine (parse an OFD invoice, rasterize page 1 to PNG).
REM
REM  Usage:
REM    build_dll.bat            build x64 release        (default)
REM    build_dll.bat x64        build x64 release
REM    build_dll.bat x86        build x86 release
REM    build_dll.bat all        build both architectures
REM    build_dll.bat test       run the FFI unit tests on the host
REM    build_dll.bat testengine run the engine unit tests
REM    build_dll.bat example [x64^|x86]   build the C++ demo (default x64)
REM
REM  Artifacts land in dist\dll\<arch>\ : the DLL, its MSVC import
REM  library, the header, and the demo / CLI sources.
REM
REM  Everything here is ASCII on purpose. cmd.exe parses a .bat file
REM  using the console ANSI code page, so non-ASCII literals inside a
REM  UTF-8 script turn into mojibake and can break command parsing.
REM ============================================================

setlocal

set "REPO=%~dp0"
set "CRATE=%REPO%crates\ofd-renderer"
set "ENGINE=%REPO%crates\invoice-engine"
set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "CARGOBIN=%USERPROFILE%\.cargo\bin"
set "DIST=%REPO%dist\dll"
REM Captured outside any parenthesised block: a literal "%ProgramFiles(x86)%"
REM inside one would have its ")" read as the end of the block.
set "PF86=%ProgramFiles(x86)%"
if not defined PF86 set "PF86=C:\Program Files (x86)"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=x64"

if /I "%TARGET%"=="test"        goto :test
if /I "%TARGET%"=="testengine"  goto :testengine
if /I "%TARGET%"=="example"     goto :example
if /I "%TARGET%"=="x64"         goto :x64
if /I "%TARGET%"=="x86"         goto :x86
if /I "%TARGET%"=="all"         goto :all

echo Unknown target: %TARGET%
echo Usage: build_dll.bat [x64^|x86^|all^|test^|testengine^|example]
exit /b 1

REM ------------------------------------------------------------
:test
call :setup_msvc x64
if errorlevel 1 exit /b 1
cd /d "%CRATE%"
cargo test
if errorlevel 1 (
  echo [ERROR] Tests failed.
  exit /b 1
)
echo [OK] Tests passed.
exit /b 0

REM ------------------------------------------------------------
REM  Engine tests live in the invoice-engine crate, which the FFI
REM  target above never exercises - run them separately.
:testengine
call :setup_msvc x64
if errorlevel 1 exit /b 1
cd /d "%ENGINE%"
cargo test
if errorlevel 1 (
  echo [ERROR] Engine tests failed.
  exit /b 1
)
echo [OK] Engine tests passed.
exit /b 0

REM ------------------------------------------------------------
:x64
call :setup_msvc x64
if errorlevel 1 exit /b 1
cd /d "%CRATE%"
echo [BUILD] x86_64-pc-windows-msvc ^(release^)
cargo build --release --target x86_64-pc-windows-msvc
if errorlevel 1 (
  echo [ERROR] x64 build failed.
  exit /b 1
)
call :stage x86_64-pc-windows-msvc x64
exit /b 0

REM ------------------------------------------------------------
:x86
call :setup_msvc x86
if errorlevel 1 exit /b 1
echo [SETUP] Ensuring the i686 Rust target is installed...
rustup target add i686-pc-windows-msvc
if errorlevel 1 (
  echo [ERROR] Could not install i686-pc-windows-msvc.
  exit /b 1
)
cd /d "%CRATE%"
echo [BUILD] i686-pc-windows-msvc ^(release^)
cargo build --release --target i686-pc-windows-msvc
if errorlevel 1 (
  echo [ERROR] x86 build failed.
  exit /b 1
)
call :stage i686-pc-windows-msvc x86
exit /b 0

REM ------------------------------------------------------------
:all
call "%~f0" x64
if errorlevel 1 exit /b 1
call "%~f0" x86
if errorlevel 1 exit /b 1
exit /b 0

REM ------------------------------------------------------------
:example
set "EXARCH=x64"
if /I "%~2"=="x86" set "EXARCH=x86"
if not exist "%DIST%\%EXARCH%\ofd_renderer.dll" (
  echo [ERROR] Build the DLL first:  build_dll.bat %EXARCH%
  exit /b 1
)
if not exist "%DIST%\%EXARCH%\ofd_renderer_demo.cpp" (
  echo [ERROR] Demo source not staged yet. Run: build_dll.bat %EXARCH%
  exit /b 1
)
call :setup_msvc %EXARCH%
if errorlevel 1 exit /b 1
cd /d "%DIST%\%EXARCH%"

REM /utf-8 is required, not cosmetic: without it MSVC transcodes narrow string
REM literals to the system ANSI code page (GBK here), so Chinese text compiled
REM into the binary comes out as mojibake the moment stdout is read as UTF-8.

REM API demo - implicit link through the import library.
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:ofd_renderer_demo.exe ^
   ofd_renderer_demo.cpp /link ofd_renderer.dll.lib
if errorlevel 1 (
  echo [ERROR] API demo build failed.
  exit /b 1
)
echo [OK] %DIST%\%EXARCH%\ofd_renderer_demo.exe

REM Standalone CLI - loads the DLL at run time, so no import library needed.
cl /nologo /EHsc /std:c++17 /W4 /utf-8 /Fe:ofd2png.exe ofd2png.cpp
if errorlevel 1 (
  echo [ERROR] ofd2png build failed.
  exit /b 1
)
echo [OK] %DIST%\%EXARCH%\ofd2png.exe

REM cl drops .obj next to the sources; they are build residue, not deliverables.
del /q *.obj >nul 2>&1
exit /b 0

REM ------------------------------------------------------------
REM  Enter a usable MSVC environment.
REM
REM  vcvarsall resolves the Windows SDK by shelling out to reg.exe. Where
REM  that lookup is unavailable - a stripped image, or a policy that blocks
REM  reg.exe - it silently yields nothing, vcvars still exits 0, and the
REM  failure only shows up later as "LNK1181: cannot open input file
REM  'kernel32.lib'". Detect that and append the SDK paths ourselves.
REM
REM  call :setup_msvc x64|x86
:setup_msvc
set "SDKARCH=%~1"
if /I "%SDKARCH%"=="x64" (
  set "VCVARS=vcvars64.bat"
) else (
  set "VCVARS=vcvars32.bat"
)
if not exist "%VS%\VC\Auxiliary\Build\%VCVARS%" (
  echo [ERROR] MSVC toolchain not found at "%VS%\VC\Auxiliary\Build\%VCVARS%"
  echo         Edit the VS variable at the top of this script.
  exit /b 1
)
call "%VS%\VC\Auxiliary\Build\%VCVARS%" >nul
set "PATH=%CARGOBIN%;%PATH%"

REM Absolute path on purpose: with a Git-for-Windows /usr/bin ahead of
REM System32 on PATH, a bare "find" resolves to the Unix one and always
REM fails, which would make this probe report the SDK as missing.
echo %LIB% | "%SystemRoot%\System32\find.exe" /i "Windows Kits" >nul
if not errorlevel 1 exit /b 0

set "SDKROOT=%PF86%\Windows Kits\10"
set "SDKVER="
for /f "delims=" %%D in ('dir /b /ad /o-n "%SDKROOT%\Include" 2^>nul') do if not defined SDKVER set "SDKVER=%%D"
if not defined SDKVER (
  echo [ERROR] No Windows SDK found under "%SDKROOT%"
  exit /b 1
)
echo [SETUP] vcvars left the Windows SDK unresolved; using %SDKVER% directly.
set "LIB=%LIB%;%SDKROOT%\Lib\%SDKVER%\um\%SDKARCH%;%SDKROOT%\Lib\%SDKVER%\ucrt\%SDKARCH%"
set "INCLUDE=%INCLUDE%;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\winrt"
exit /b 0

REM ------------------------------------------------------------
REM  Collect DLL + import library + header + demo into dist\dll\<arch>.
REM  MSVC cdylib emits the import library as "ofd_renderer.dll.lib" on
REM  current Rust; older toolchains used "ofd_renderer.lib". Copy whichever
REM  exists.
:stage
REM Cargo workspace: all members share the root target\ directory.
set "SRC=%REPO%target\%~1\release"
set "DST=%DIST%\%~2"

if not exist "%SRC%\ofd_renderer.dll" (
  echo [ERROR] Expected DLL not found at "%SRC%\ofd_renderer.dll"
  exit /b 1
)

if not exist "%DST%" mkdir "%DST%"
copy /y "%SRC%\ofd_renderer.dll" "%DST%\" >nul
if exist "%SRC%\ofd_renderer.dll.lib" copy /y "%SRC%\ofd_renderer.dll.lib" "%DST%\" >nul
if exist "%SRC%\ofd_renderer.lib" copy /y "%SRC%\ofd_renderer.lib" "%DST%\" >nul
if exist "%CRATE%\include\ofd_renderer.h" copy /y "%CRATE%\include\ofd_renderer.h" "%DST%\" >nul
if exist "%CRATE%\cpp\ofd_renderer_demo.cpp" copy /y "%CRATE%\cpp\ofd_renderer_demo.cpp" "%DST%\" >nul
if exist "%CRATE%\cpp\ofd2png.cpp" copy /y "%CRATE%\cpp\ofd2png.cpp" "%DST%\" >nul

for %%F in ("%DST%\ofd_renderer.dll") do echo [OK] %%~fF  (%%~zF bytes)
echo [OK] Staged into %DST%
exit /b 0
