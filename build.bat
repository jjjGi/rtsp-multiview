@echo off
rem Build with Visual Studio 2022 C++ compiler and third_party/ffmpeg (BtbN win64 lgpl shared)
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set FF=third_party\ffmpeg
if not exist build mkdir build

cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /I%FF%\include /Fobuild\ /Fe:build\multiview.exe ^
   src\main.cpp src\stream_worker.cpp src\log.cpp ^
   /link /LIBPATH:%FF%\lib avformat.lib avcodec.lib avutil.lib swscale.lib user32.lib gdi32.lib || exit /b 1

cl /nologo /std:c++17 /EHsc /W4 /utf-8 /Fobuild\ /Fe:build\backoff_test.exe tests\backoff_test.cpp || exit /b 1

copy /y %FF%\bin\*.dll build\ >nul
echo build ok
