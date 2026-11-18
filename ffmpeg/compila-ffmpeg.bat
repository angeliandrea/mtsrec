@echo off
rem ffmpeg dal sorgente, in librerie statiche per mtsrec.exe (sezione 9.81).
rem     compila-ffmpeg.bat
rem Usa MSYS2 con clang, make e nasm in %USERPROFILE%\WinISORecorder-deps\msys64 e il sorgente di
rem ffmpeg 9.0.2 in %USERPROFILE%\WinISORecorder-deps\ffmpeg-9.0.2. Il risultato va in
rem %USERPROFILE%\WinISORecorder-deps\ffmpeg-statico, che CMakeLists.txt cerca per primo.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set "DEPS=%USERPROFILE%\WinISORecorder-deps"
set MSYSTEM=CLANG64
set MSYS2_PATH_TYPE=inherit
set CHERE_INVOKING=1
"%DEPS%\msys64\usr\bin\bash.exe" -lc "bash \"$(cygpath -u '%~dp0compila-ffmpeg.sh')\" \"$(cygpath -u '%DEPS%\ffmpeg-9.0.2')\" \"$(cygpath -u '%DEPS%\ffmpeg-statico')\""
