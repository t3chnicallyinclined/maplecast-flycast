@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
cmake --build C:\Users\trist\projects\maplecast-flycast\build --config Release --target flycast -j4
echo BUILD_EXIT_CODE=%errorlevel%
