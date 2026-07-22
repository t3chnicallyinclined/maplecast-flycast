#!/usr/bin/env python3
"""Batch-transpile the whole in-match game-tick subtree (scene handler loc_8c0358be)
into one C file with a global {addr -> fn} dispatch table, per the trace-grounded
worklist (funclist.txt). Every executed bank function becomes sub_<addr>(Sh4Ctx*);
jsr @rN / jmp @rN resolve at runtime via call_addr(c, reg) -> the dispatch switch,
exactly like the live engine dispatches through RAM pointers (which are in _ram_f90).

  python3 gen_tick.py           # -> gen_tick_all.c + a per-function failure report

M4 build toward EXECUTOR Test A. Failures (numeric-PC pools, unknown opcodes) are
collected, not fatal, so the report tells us exactly which gaps remain."""
import re, sys, os
from lift import parse_asm, extract_block, slurp_function
from emit_func import emit_function
from codegen import R
from gen_one import normalize_data_pointers

# durable worklists persisted in _handoff/ (regenerate via _handoff/scan_tick.py + scan_spl.py
# from the ticktrace; see HANDOFF-EXECUTOR.md). Falls back to the original session scratchpad.
_HERE = os.path.dirname(os.path.abspath(__file__))
_HANDOFF = os.path.join(_HERE, "_handoff")
_SCR = r"C:/Users/trist/AppData/Local/Temp/claude/c--Users-trist-projects-maplecast-flycast/ffa8329c-44cf-4fbe-a9d0-0eebc4f87c12/scratchpad"
def _pick(name):
    p = os.path.join(_HANDOFF, name)
    return p if os.path.exists(p) else (_SCR + "/" + name)
FUNCLIST     = _pick("funclist.txt")
SPL_FUNCLIST = _pick("spl_funclist.txt")
BANKDIR  = r"C:/Users/trist/projects/_marv_re/build"
SPLDIR   = r"C:/Users/trist/projects/_marv_re/char_prg/code"
ENTRY    = 0x8c0358be

def spl_reloc(bank):
    # this snapshot: Storm S_PL2A in slot A (reloc 0), Cable S_PL17 in slot B (+0x8000)
    return 0x8000 if 'S_PL17' in bank else 0

def normalize_spl_text(text, reloc):
    """Rewrite SPL labels BEG_/FUN_/LAB_/DAT_/PTR_<hex> -> loc_<hex+reloc> (defs, operands,
    and internal #data pointers), leaving bank imports loc_8c.. and raw 0x.. untouched, so
    the existing bank pipeline handles the SPL unchanged."""
    return re.sub(r'\b(BEG_|FUN_|LAB_|DAT_|PTR_)([0-9a-fA-F]+)\b',
                  lambda m: f"loc_{int(m.group(2),16)+reloc:08x}", text)

# resolvers now receive the LATCHED target expression (a C var captured before the
# delay slot), not the raw @rN operand.
def jsr_res(e): return f"call_addr(c, {e});"
def jmp_res(e): return f"call_addr(c, {e});"
def bsr_res(target):
    # bsr loc_X -> route through the dispatch table too (constant addr), so a not-yet
    # transpiled target is a no-op unknown_call, never a link error.
    m = re.fullmatch(r'loc_([0-9a-fA-F]+)', target.lower())
    if m: return f"call_addr(c, 0x{m.group(1)}u);"
    return f"call_addr(c, 0u); /* unresolved named bsr {target} */"

# functions the trace reaches (via computed jump) that the ;==== scanners excluded but
# which transpile fine — e.g. loc_8c031094 is a bra-jump-table, not a braf.
EXTRA_FUNCS = [(0x8c031094, 'bank03.asm', 2411, 2492, '-'),
               (0x8c120220, 'bank12.asm', 1, 20, '-'),   # back-bank XMTRX loader (frchg;16x fmov @r4+;rts;frchg)
               # --- object-pool SPAWN chain (effects/projectiles): the universal spawner loc_8c044f12 is
               # reached via jsr @rN from loc_8c0e3098 (already transpiled) -> was a no-op mc_unknown_call,
               # so pool objects (0x27D734 etc.) were never allocated/spliced. re_kb:object_pool_spawn_chain.
               (0x8c044f12, 'bank04.asm', 11793, 11877, '-'),  # Obj_Alloc: pop free node, memset, +0x03=cat, dispatch bank14 table[listop]
               (0x8c129728, 'bank12.asm', 22327, 22343, '-'),  # memset(dst=r4,val=r5,cnt=r6)  [Obj_Alloc callee]
               (0x8c044fa2, 'bank04.asm', 11880, 11915, '-'),  # list insert-at-head (bank14 table[0]); +0x08=prev/+0x0C=next
               (0x8c129560, 'bank12.asm', 22005, 22088, '-'),  # var long-copy dispatcher (jmp-table cnt<=0x40) [constructor callee]
               (0x8c129600, 'bank12.asm', 22128, 22149, '-'),  # long-copy generic loop (cnt>0x40); has 0x8C12960C
               (0x8c034c38, 'bank03.asm', 11312, 11397, '-'),  # Obj_LoadAnim: +0x158/9 grp/anim, +0x154 cell-data [constructor tail]
               # --- forward coverage: other bank14 listop handlers (non-head-insert categories) ---
               (0x8c044fe0, 'bank04.asm', 11919, 11974, '-'),  # table[1] insert-at-tail (wide range folds locals; #data 11937-58 parsed as data)
               (0x8c045066, 'bank04.asm', 12003, 12031, '-'),  # table[2] insert-after-node
               (0x8c04503e, 'bank04.asm', 11976, 12000, '-'),  # table[3] insert-before-node
               # --- JUMP-PATH stack-balance fix: loc_8c051bca (traced) does `sts.l pr,@-r15` then
               # `bf loc_8c051bde`; the balancing `lds.l @r15+,pr; rts` lives in the SEPARATE fn
               # loc_8c051bde, so the cross-fn bf is a tail-call. When 051bde was uncranked (no-op)
               # the pop never ran -> r15 leaked -4 -> propagated up 047dec..04761c -> loc_8c04761c's
               # epilogue `mov.l @r15+,r13` read the char base -> loc_8c043cdc `jsr @r13` dispatched
               # 0x8C268340 forever (JUMP-input hang). Cranking 051bde restores the pop/rts.
               (0x8c051bde, 'bank05.asm', 4342, 4349, '-')]   # bf-continuation of loc_8c051bca (pops pr + rts)

def parse_worklist():
    funcs = list(EXTRA_FUNCS)
    seen = {a for a, _, _, _, _ in funcs}
    for path in (FUNCLIST, SPL_FUNCLIST):
        try: lines = open(path, encoding='utf-8').readlines()
        except FileNotFoundError: continue
        for line in lines:
            line = line.strip()
            if not line or line.startswith('#'): continue
            m = re.match(r'(loc_[0-9a-fA-F]+)\s+(\S+)\s+body_lines=(\d+)-(\d+)\s+data=(\S+)', line)
            if not m: continue
            addr = int(m.group(1)[4:], 16)
            if addr in seen: continue          # dedup (EXTRA_FUNCS / cross-list overlap)
            seen.add(addr)
            funcs.append((addr, m.group(2), int(m.group(3)), int(m.group(4)), m.group(5)))
    return funcs

_txt = {}
def gettext(bank):
    if bank not in _txt:
        if bank.startswith('S_PL'):
            _txt[bank] = normalize_spl_text(slurp_function(SPLDIR + '/' + bank, None), spl_reloc(bank))
        else:
            _txt[bank] = slurp_function(BANKDIR + '/' + bank, None)
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
                tgt = m.group(1)
                if tgt in defined: return m.group(0)   # local label: keep the goto
                hx = re.search(r'loc_([0-9a-fA-F]+)', tgt)   # loc_<hex> or bankNN.loc_<hex>
                if hx: return f"{{ call_addr(c, 0x{hx.group(1)}u); return; }}"
                return m.group(0)
            body = re.sub(r'goto ([A-Za-z0-9_.]+);', _fixgoto, body)
            blocks = sorted(int(b[4:], 16) for b in defined)   # every basic-block addr in this fn
            ok.append((addr, fn, body, blocks))
        except Exception as e:
            fail.append((addr, bank, f"{first}-{last}", type(e).__name__, str(e)[:90]))

    # map EVERY basic-block address (entry + mid-function) -> its enclosing function entry,
    # so computed jumps/jump-tables that land inside a function dispatch correctly (re-enter
    # the function at that block via the entry-switch). First function claiming a block wins.
    block_of = {}
    for addr, fn, body, blocks in ok:
        for ba in blocks:
            block_of.setdefault(ba, addr)
    with open('gen_tick_all.c', 'w') as f:
        f.write('#include "sh4ctx.h"\n')
        f.write('void mc_unknown_call(u32 a);\nextern int mc_call_guard(void);\n')
        f.write('void call_addr(Sh4Ctx*c,u32 a);\n')
        for addr, fn, body, blocks in ok: f.write(f'void fn_{addr:08x}(Sh4Ctx*c,u32 _e);\n')
        for addr, fn, body, blocks in ok:
            f.write(f'\nvoid fn_{addr:08x}(Sh4Ctx*c,u32 _e){{\n switch(_e){{\n')
            for ba in blocks:
                f.write(f'  case 0x{ba:08x}u: goto loc_{ba:08x};\n')
            f.write('  default: break;\n }\n')
            f.write(body)
            f.write('\n}\n')
        f.write('\nextern u32 mc_curfn; extern void mc_push(u32); extern void mc_pop(void);\n')
        f.write('#ifdef MC_DHOOK\nextern void mc_dispatch_hook(u32,Sh4Ctx*);\n#endif\n')
        f.write('void call_addr(Sh4Ctx*c,u32 a){\n if(mc_call_guard()) return;\n mc_curfn=a; mc_push(a);\n')
        f.write('#ifdef MC_DHOOK\n mc_dispatch_hook(a,c);\n#endif\n switch(a){\n')
        for ba, entry in sorted(block_of.items()):
            f.write(f'  case 0x{ba:08x}u: fn_{entry:08x}(c, 0x{ba:08x}u); break;\n')
        f.write('  default: mc_unknown_call(a); { extern void mc_unk_regs(u32*); mc_unk_regs(c->r); } break;\n }\n mc_pop();\n}\n')
        f.write(f'\nvoid tick_entry(Sh4Ctx*c){{ fn_{ENTRY:08x}(c, 0x{ENTRY:08x}u); }}\n')

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
