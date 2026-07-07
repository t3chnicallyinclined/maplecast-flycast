@echo off
cd /d C:\Users\trist\projects\maplecast-flycast\tools\render-replica-poc\realcore
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set ROOT=C:\Users\trist\projects\maplecast-flycast
set CORE=%ROOT%\core
set INC=/I "%CORE%" /I "%CORE%\deps" /I "%CORE%\deps\nowide\include"
set DEFS=/DTARGET_NO_REC /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
del runner.exe >nul 2>&1
cl /nologo /std:c++17 /EHsc /O2 /fp:precise %DEFS% %INC% /Fe:runner.exe ^
 runner.cpp ^
 "%CORE%\hw\sh4\interpr\sh4_interpreter.cpp" ^
 "%CORE%\hw\sh4\interpr\sh4_opcodes.cpp" ^
 "%CORE%\hw\sh4\interpr\sh4_fpu.cpp" ^
 "%CORE%\hw\sh4\sh4_opcode_list.cpp" ^
 "%CORE%\hw\sh4\sh4_core_regs.cpp" ^
 "%CORE%\hw\sh4\sh4_rom.cpp"
echo BUILD_DONE_EXIT=%ERRORLEVEL%
