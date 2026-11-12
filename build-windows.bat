@echo off
rem One-click Windows build. Needs Visual Studio 2022 with "Desktop development with C++"
rem (free Community edition is fine) and an internet connection the first time (downloads JUCE).
setlocal
where cmake >nul 2>nul
if errorlevel 1 (
  if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    set "PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
  ) else (
    echo CMake was not found. Install Visual Studio 2022 Community with "Desktop development with C++".
    pause
    exit /b 1
  )
)
cmake -B build -G "Visual Studio 17 2022" -A x64 || goto :fail
cmake --build build --config Release --parallel || goto :fail
copy /y build\Ostinato_artefacts\Release\Ostinato.exe Ostinato.exe >nul
echo.
echo Done: Ostinato.exe is in this folder. Double-click it to run.
pause
exit /b 0
:fail
echo Build failed. Scroll up for the first error message.
pause
exit /b 1
