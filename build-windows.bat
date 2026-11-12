@echo off
rem One-click Windows build. Needs Visual Studio 2022 or newer with "Desktop development with C++"
rem (free Community edition is fine) and an internet connection the first time (downloads JUCE).
setlocal
where cmake >nul 2>nul
if not errorlevel 1 goto :build
for /d %%V in ("%ProgramFiles%\Microsoft Visual Studio\*") do for /d %%E in ("%%V\*") do if exist "%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "VSCMAKE=%%E\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if not defined VSCMAKE goto :nocmake
set "PATH=%VSCMAKE%;%PATH%"
:build
cmake -B build -A x64 || goto :fail
cmake --build build --config Release --parallel || goto :fail
copy /y build\Ostinato_artefacts\Release\Ostinato.exe Ostinato.exe >nul
echo.
echo Done: Ostinato.exe is in this folder. Double-click it to run.
pause
exit /b 0
:nocmake
echo CMake was not found. Install Visual Studio Community with "Desktop development with C++".
pause
exit /b 1
:fail
echo Build failed. Scroll up for the first error message.
pause
exit /b 1
