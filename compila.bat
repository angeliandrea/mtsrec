@echo off
rem Compila mtsrec. La cartella di compilazione sta fuori da Synology, perché pesa e cambia di continuo.
rem     compila.bat            configura se serve e compila
rem     compila.bat pulito     riparte da zero
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set "B=%USERPROFILE%\WinISORecorder-build"
if /i "%1"=="pulito" rmdir /s /q "%B%"
if not exist "%B%\build.ninja" (
  cmake -S "%~dp0." -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" || exit /b 1
)
cmake --build "%B%" || exit /b 1
