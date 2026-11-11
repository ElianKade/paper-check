@echo off
rem ============================================================
rem  PaperCheck - one click build and run (Windows / MinGW-w64)
rem  Usage: double click, or run "build.bat" in cmd
rem ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo [1/3] Locating C++ compiler ...
set "CXX="
where g++ >nul 2>nul && set "CXX=g++"
if not defined CXX if exist "D:\MinGW\bin\g++.exe" set "CXX=D:\MinGW\bin\g++.exe"
if not defined CXX if exist "C:\mingw64\bin\g++.exe" set "CXX=C:\mingw64\bin\g++.exe"
if not defined CXX if exist "C:\MinGW\bin\g++.exe" set "CXX=C:\MinGW\bin\g++.exe"
if not defined CXX (
    echo     g++ not found. Please install MinGW-w64 and add its bin folder to PATH.
    pause
    exit /b 1
)
echo     using: %CXX%

if not exist build mkdir build

rem g++ on Windows does not expand wildcards, collect sources here
set "SOURCES="
for %%f in (src\*.cpp) do set "SOURCES=!SOURCES! "%%f""

echo [2/3] Compiling ...
rem -static links libgcc / libstdc++ into the exe so it runs without MinGW DLLs
"%CXX%" -std=c++17 -O2 -Wall -Wextra -Isrc -static -s !SOURCES! -o build\pc_check.exe -lws2_32
if errorlevel 1 (
    echo.
    echo     Build failed. See the messages above.
    pause
    exit /b 1
)
echo     done: build\pc_check.exe

echo [3/3] Starting server at http://127.0.0.1:8080
start "" http://127.0.0.1:8080
build\pc_check.exe --port 8080
