@echo off
REM Build the Option-C PoC with MSVC. Usage: build.cmd <out.exe> <src1.c> [src2.c ...]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set OUT=%1
shift
set SRCS=
:loop
if "%1"=="" goto done
set SRCS=%SRCS% %1
shift
goto loop
:done
del *.obj >nul 2>&1
cl /nologo /O2 /fp:precise /Fe:%OUT% %SRCS%
exit /b %ERRORLEVEL%
