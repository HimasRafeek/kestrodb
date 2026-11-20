@echo off
rem Builds the Node addon and kestro-server with AddressSanitizer (MSVC 2022) for running
rem the Node suites under ASan:
rem   fuzz\asan-addon.bat
rem   set KESTRO_ADDON=%CD%\build-fuzz\asan-addon\kestro.node
rem   set KESTRO_SERVER_BINARY=%CD%\build-fuzz\asan-addon\kestro-server.exe
rem   set PATH=%CD%\build-fuzz\asan-addon;%PATH%
rem   node --test tests\...
rem node.exe itself is not instrumented; the ASan runtime is loaded with the addon.
setlocal
set ROOT=%~dp0..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build-fuzz\asan-addon" mkdir "%ROOT%\build-fuzz\asan-addon"
cd /d "%ROOT%\build-fuzz\asan-addon"
lib /nologo /def:"%ROOT%\cmake\node_api.def" /out:node_api.lib /machine:X64 >nul || exit /b 1
set B3=/DBLAKE3_NO_SSE2 /DBLAKE3_NO_SSE41 /DBLAKE3_NO_AVX2 /DBLAKE3_NO_AVX512 /DBLAKE3_USE_NEON=0
set CXX=/nologo /Zi /O1 /MD /bigobj /EHsc /std:c++17 /fsanitize=address /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%\src" /I"%ROOT%\vendor\blake3"
cl /nologo /c /Zi /O1 /MD /fsanitize=address %B3% /I"%ROOT%\vendor\blake3" "%ROOT%\vendor\blake3\blake3.c" "%ROOT%\vendor\blake3\blake3_dispatch.c" "%ROOT%\vendor\blake3\blake3_portable.c" || exit /b 1
cl %CXX% /LD /I"%ROOT%\vendor\node-api\include" "%ROOT%\src\addon.cpp" blake3.obj blake3_dispatch.obj blake3_portable.obj /Fe:kestro.node /link node_api.lib bcrypt.lib ws2_32.lib || exit /b 1
cl %CXX% "%ROOT%\src\server.cpp" blake3.obj blake3_dispatch.obj blake3_portable.obj /Fe:kestro-server.exe /link /STACK:16777216 bcrypt.lib ws2_32.lib || exit /b 1
for /f "delims=" %%d in ('where cl') do if not defined CLDIR set CLDIR=%%~dpd
copy /y "%CLDIR%clang_rt.asan_dynamic-x86_64.dll" . >nul
echo ASAN_ADDON_OK
