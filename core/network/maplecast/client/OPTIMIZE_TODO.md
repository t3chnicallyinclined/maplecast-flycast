# Client Hot Path Optimizations — TODO

1. **Decode directly into flycast's TA buffer** — eliminate 140KB memcpy from _wasmPrevTA into _wasmCtx.tad.thd_root. Delta-decode straight into the TA context.

2. **Replace std::vector _wasmPrevTA with static buffer** — no heap alloc, cache-friendly.

3. **Fuse small delta run memcpys** — individual memcpy per run has call overhead. Tight inline loop.

4. **PVR register writes** — 16 individual field assignments done twice (hw regs + rend_context). Single struct memcpy.

5. **Pre-allocate WASM receive buffer in JS** — one malloc at init, reuse every frame. No malloc/free per frame.
