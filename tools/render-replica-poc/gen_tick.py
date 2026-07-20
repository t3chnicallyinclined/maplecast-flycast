#!/usr/bin/env python3
"""Batch-transpile the whole in-match game-tick subtree (scene handler loc_8c0358be)
into one C file with a global {addr -> fn} dispatch table, per the trace-grounded
worklist (funclist.txt). Every executed bank function becomes sub_<addr>(Sh4Ctx*);
jsr @rN / jmp @rN resolve at runtime via call_addr(c, reg) -> the dispatch switch,
exactly like the live engine dispatches through RAM pointers (which are in _ram_f90).

  python3 gen_tick.py           # -> gen_tick_all.c + a per-function failure report

M4 build toward EXECUTOR Test A. Failures (numeric-PC pools, unknown opcodes) are
collected, not fatal, so the report tells us exactly which gaps remain."""
import re, sys
from lift import parse_asm, extract_block, slurp_function
from emit_func import emit_function
from codegen import R
from gen_one import normalize_data_pointers

FUNCLIST = r"C:/Users/trist/AppData/Local/Temp/claude/c--Users-trist-projects-maplecast-flycast/ffa8329c-44cf-4fbe-a9d0-0eebc4f87c12/scratchpad/funclist.txt"
BANKDIR  = r"C:/Users/trist/projects/_marv_re/build"
ENTRY    = 0x8c0358be

def reg_of(a): return a.lstrip('@').strip()
def jsr_res(a): return f"call_addr(c, {R(reg_of(a))});"
def jmp_res(a): return f"call_addr(c, {R(reg_of(a))});"
def bsr_res(target):
    # bsr loc_X -> route through the dispatch table too (constant addr), so a not-yet
    # transpiled target is a no-op unknown_call, never a link error.
    m = re.fullmatch(r'loc_([0-9a-fA-F]+)', target.lower())
    if m: return f"call_addr(c, 0x{m.group(1)}u);"
    raise NotImplementedError(f"bsr target {target}")

def parse_worklist():
    funcs = []
    for line in open(FUNCLIST, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#'): continue
        m = re.match(r'(loc_[0-9a-fA-F]+)\s+(\S+)\s+body_lines=(\d+)-(\d+)\s+data=(\S+)', line)
        if not m: continue
        funcs.append((int(m.group(1)[4:], 16), m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)))
    return funcs

_txt = {}
def gettext(bank):
    if bank not in _txt: _txt[bank] = slurp_function(BANKDIR + '/' + bank, None)
    return _txt[bank]

_globalpools = None
def global_pools(banks):
    """Union of every bank's #data pool labels -> value, normalized to addresses once.
    Fixes 'pool loc_X not in data' when a pool is defined outside the function's
    extracted line range (or in a physically-adjacent bank file)."""
    global _globalpools
    if _globalpools is None:
        _globalpools = {}
        for bank in sorted(set(banks)):
            _, d = parse_asm(gettext(bank))
            _globalpools.update(d)
        normalize_data_pointers(_globalpools)
    return _globalpools

def main():
    funcs = parse_worklist()
    pools = global_pools([b for _, b, _, _, _ in funcs])
    ok, fail = [], []
    for addr, bank, first, last, data in funcs:
        fn = f"sub_{addr:08x}"
        try:
            text = gettext(bank)
            blk = extract_block(text, first, last)
            if data != '-':
                a, b = int(data.split('-')[0]), int(data.split('-')[1])
                if not (a >= first and b <= last):     # append pools only if OUTSIDE the body
                    blk += '\n' + extract_block(text, a, b)
            insns, d = parse_asm(blk)
            normalize_data_pointers(d)
            merged = dict(pools); merged.update(d)   # global fills gaps, local wins
            body = emit_function(insns, merged, fn, jsr_res, bsr_call_resolver=bsr_res, jmp_call_resolver=jmp_res)
            # cross-;==== branch to a label outside this function == a tail-call to that
            # function entry -> route through the dispatch table then return.
            defined = set(re.findall(r'^(loc_[0-9a-f]+):', body, re.M))
            def _fixgoto(m):
                lbl = m.group(1)
                return m.group(0) if lbl in defined else f"{{ call_addr(c, 0x{lbl[4:]}u); return; }}"
            body = re.sub(r'goto (loc_[0-9a-f]+);', _fixgoto, body)
            ok.append((addr, fn, body))
        except Exception as e:
            fail.append((addr, bank, f"{first}-{last}", type(e).__name__, str(e)[:90]))

    okaddrs = {a for a, _, _ in ok}
    with open('gen_tick_all.c', 'w') as f:
        f.write('#include "sh4ctx.h"\n')
        f.write('void mc_unknown_call(u32 a);\n')
        f.write('void call_addr(Sh4Ctx*c,u32 a);\n')
        for a, fn, _ in ok: f.write(f'void {fn}(Sh4Ctx*c);\n')
        for a, fn, body in ok:
            f.write(f'\nvoid {fn}(Sh4Ctx*c){{\n{body}\n}}\n')
        f.write('\nextern int mc_call_guard(void);\n')
        f.write('void call_addr(Sh4Ctx*c,u32 a){\n if(mc_call_guard()) return;\n switch(a){\n')
        for a, fn, _ in ok: f.write(f'  case 0x{a:08x}u: {fn}(c); return;\n')
        f.write('  default: mc_unknown_call(a); return;\n }\n}\n')
        f.write(f'\nvoid tick_entry(Sh4Ctx*c){{ sub_{ENTRY:08x}(c); }}\n')

    print(f"worklist: {len(funcs)} | transpiled OK: {len(ok)} | FAILED: {len(fail)}")
    if fail:
        print("--- failures (need a codegen/pool gap) ---")
        from collections import Counter
        by = Counter(f"{ex}: {msg}" for _, _, _, ex, msg in fail)
        for k, c in by.most_common(20): print(f"  {c:3d}x  {k}")
    with open('gen_tick_fail.txt', 'w') as f:
        for a, bank, lines, ex, msg in fail: f.write(f"sub_{a:08x} {bank} {lines} {ex}: {msg}\n")

if __name__ == '__main__':
    main()
