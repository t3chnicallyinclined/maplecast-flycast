@echo off
REM Option-C PoC: generate -> build -> run all validations (single vcvars init).
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

echo === [1/4] LIFT: generate C from SH4 disasm ===
python gen_leaf.py >nul || goto :err
python gen_walker.py || goto :err
python gen_transform.py || goto :err
python make_transform_test.py || goto :err

echo.
echo === [2/4] LEAF loc_8C11E460 (bit-exact vs reference floorf) ===
del *.obj >nul 2>&1
cl /nologo /O2 /fp:precise /Fe:test_leaf.exe gen_leaf.c test_leaf.c >nul 2>&1
.\test_leaf.exe | findstr /C:"exact"

echo.
echo === [3/4] TRANSFORM-CORE loc_8c0347c8..864 (vs ASMTRACE; bit-exact vs ref float) ===
del *.obj >nul 2>&1
cl /nologo /O2 /fp:precise /Fe:test_transform.exe gen_transform.c test_transform.c >nul 2>&1
.\test_transform.exe | findstr /C:"BIT-EXACT" /C:"X:" /C:"Y:"

echo.
echo === [4/4] FULL WALKER loc_8c0344d4 (compile+link+run, stack-balanced) ===
del *.obj >nul 2>&1
cl /nologo /O2 /fp:precise /Fe:test_walker.exe gen_walker.c gen_leaf.c leaves.c test_walker_compile.c >nul 2>&1
.\test_walker.exe | findstr /C:"FULL-WALKER"
goto :eof
:err
echo GEN FAILED
exit /b 1
