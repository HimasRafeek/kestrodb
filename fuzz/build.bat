@echo off
rem Builds the libFuzzer targets with AddressSanitizer, using MSVC 2022 (/fsanitize=fuzzer).
rem (LLVM clang-cl 23's Windows libFuzzer cannot catch C++ exceptions thrown by the engine,
rem  so it is not used here.)
rem   fuzz\build.bat            -> build-fuzz\fuzz_*.exe
rem AddressSanitizer frames are several times larger than release ones, so the fuzzers get a
rem 16 MB stack (release builds handle the parser's nesting limits on a 1 MB stack).
rem Run, for example:  build-fuzz\fuzz_parse.exe -max_total_time=300 fuzz\corpus-parse
setlocal
set ROOT=%~dp0..
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build-fuzz" mkdir "%ROOT%\build-fuzz"
cd /d "%ROOT%\build-fuzz"
set COMMON=/nologo /EHsc /std:c++17 /O1 /Zi /MD /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%\src" /I"%ROOT%\vendor\blake3" /fsanitize=address /fsanitize=fuzzer
set B3=/DBLAKE3_NO_SSE2 /DBLAKE3_NO_SSE41 /DBLAKE3_NO_AVX2 /DBLAKE3_NO_AVX512 /DBLAKE3_USE_NEON=0
cl /nologo /c /O1 /MD /fsanitize=address %B3% /I"%ROOT%\vendor\blake3" "%ROOT%\vendor\blake3\blake3.c" "%ROOT%\vendor\blake3\blake3_dispatch.c" "%ROOT%\vendor\blake3\blake3_portable.c" || exit /b 1
for %%t in (fuzz_parse fuzz_protocol fuzz_sql_exec fuzz_open) do (
  echo building %%t
  cl %COMMON% "%ROOT%\fuzz\%%t.cpp" blake3.obj blake3_dispatch.obj blake3_portable.obj /Fe:%%t.exe /link /STACK:16777216 bcrypt.lib ws2_32.lib || exit /b 1
)
rem The AddressSanitizer runtime that matches this MSVC.
for /f "delims=" %%d in ('where cl') do if not defined CLDIR set CLDIR=%%~dpd
copy /y "%CLDIR%clang_rt.asan_dynamic-x86_64.dll" . >nul
echo FUZZ_BUILD_OK
