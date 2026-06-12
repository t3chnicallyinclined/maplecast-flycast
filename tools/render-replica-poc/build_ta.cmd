@echo off
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
python build_image_dump.py >nul 2>&1
del *.obj >nul 2>&1
cl /nologo /O2 /fp:precise /Fe:test_ta_emit.exe gen_walker.c gen_leaf.c gen_submit.c test_ta_emit.c >ta_build.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type ta_build.log & exit /b 1 )
.\test_ta_emit.exe
where node >nul 2>&1 && node verify_ta.mjs
