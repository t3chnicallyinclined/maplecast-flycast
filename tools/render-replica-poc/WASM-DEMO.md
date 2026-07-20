# Browser executor demo (exec_demo.wasm)

Compiles the transpiled + verified game-tick leaves into a tiny freestanding WASM
so the browser runs real MVC2 SH-4 game code (no ROM, no emulator). Build:

    # 1. generate the verified leaves (local marvelous2 disasm required)
    python3 gen_rng.py
    python3 gen_one.py $BANK/bank03.asm 29 50   accum_3015c   53:69
    python3 gen_one.py $BANK/bank11.asm 35194 35205 nrand_e750 35260:35261
    python3 gen_one.py $BANK/bank02.asm 31833 31875 btnmask_2d1c0 31877:31884

    # 2. tiny stub headers (functions call no libc) + freestanding wasm
    mkdir -p _wasmstub && : > _wasmstub/math.h && : > _wasmstub/string.h
    zig cc -target wasm32-freestanding -nostdlib -Wl,--no-entry -Wl,--export-dynamic -Os \
      -I_wasmstub -I. exec_demo.c gen_rng.c gen_accum_3015c.c gen_nrand_e750.c gen_btnmask_2d1c0.c \
      -o exec_demo.wasm

Result: ~2 KB, zero imports, exports boot/poke*/peek*/run_rng/run_accum/run_btnmask/run_nrand.
Validated byte-exact vs flycast in a WASM host. The transpiled gen_*.c and the .wasm are
NOT committed (derived from the copyrighted game); regenerate them locally.
