@echo off
rem Builds and runs the portable C++ suite with AddressSanitizer (MSVC 2022).
rem   fuzz\asan-tests.bat     -> build-fuzz\asan\kestro-portable-tests.exe, then runs it
setlocal
set ROOT=%~dp0..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build-fuzz\asan" mkdir "%ROOT%\build-fuzz\asan"
cd /d "%ROOT%\build-fuzz\asan"
set B3=/DBLAKE3_NO_SSE2 /DBLAKE3_NO_SSE41 /DBLAKE3_NO_AVX2 /DBLAKE3_NO_AVX512 /DBLAKE3_USE_NEON=0
cl /nologo /c /Zi /O1 /MD /fsanitize=address %B3% /I"%ROOT%\vendor\blake3" "%ROOT%\vendor\blake3\blake3.c" "%ROOT%\vendor\blake3\blake3_dispatch.c" "%ROOT%\vendor\blake3\blake3_portable.c" || exit /b 1
cl /nologo /Zi /O1 /MD /bigobj /EHsc /std:c++17 /fsanitize=address /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%\src" /I"%ROOT%\vendor\blake3" "%ROOT%\src\tests_portable.cpp" blake3.obj blake3_dispatch.obj blake3_portable.obj /Fe:kestro-portable-tests.exe /link /STACK:16777216 bcrypt.lib ws2_32.lib /MANIFEST:EMBED /MANIFESTINPUT:"%ROOT%\cmake\windows.manifest" || exit /b 1
for /f "delims=" %%d in ('where cl') do if not defined CLDIR set CLDIR=%%~dpd
copy /y "%CLDIR%clang_rt.asan_dynamic-x86_64.dll" . >nul
echo ASAN_BUILD_OK
set ASAN_OPTIONS=detect_stack_use_after_return=1:halt_on_error=1
"%ROOT%\build-fuzz\asan\kestro-portable-tests.exe"
echo ASAN_TESTS_EXIT %ERRORLEVEL%
