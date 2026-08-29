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
# COMPLETE-transpile lists (every ;==== code section in the core game-logic banks + full SPL,
# from enumerate_funcs.py) come FIRST so every in-bank jsr/bsr/jmp target resolves to real code
# instead of the mc_unknown_call NO-OP. The TRACE-derived lists (funclist.txt/spl_funclist.txt)
# follow to keep the OTHER banks' traced coverage (bank01/02/0e/0f/10/11/17/18/19) — dedup by
# addr means the complete lists win for the core banks, traced fills the rest. No regression.
FUNCLIST_FULL     = _pick("funclist_full.txt")
SPL_FUNCLIST_FULL = _pick("spl_funclist_full.txt")
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
               (0x8c051bde, 'bank05.asm', 4342, 4349, '-'),   # bf-continuation of loc_8c051bca (pops pr + rts)
               # --- JUMP-START handler (input-response proof): loc_8c04fc9e is the jump-state entry
               # in the per-char state dispatch table (base loc_8c053340, reached via jsr @r3 from
               # loc_8c0532ea). Statically unresolvable (runtime table load) -> was mc_unknown_call ->
               # char +0x60 y_velocity never set (stayed 0 vs flycast 21.696). Clean leaf (0 pr push/pop,
               # 1 rts): reads per-char jump params from resident table 0x8c14fa7c, CPS-scale FP math ->
               # writes +0x60 y_velocity, +0x6C y_drag, +0x5C x_velocity, +0x68 JumpUNK. re_kb candidate.
               (0x8c04fc9e, 'bank04.asm', 38568, 38738, '-'),   # jump-state handler (input-response)
               # --- JUMP-START call tree (the move-SETUP subsystem, only reached on a state
               # TRANSITION -> never exercised by the neutral/idle chain, so never cranked). Trace
               # (runner --trace on _inj_jump): 0x8c0532a8 -> 0x8c0530d8 -> jsr @tbl[move]=0x8c053460
               # (jump handler) -> 0x8c034e8c (load-anim) -> bra 0x8c04fc9e (writer). All GAP
               # (mc_unknown_call). Cranking these lets the jump impulse reach +0x60. re_kb candidate.
               (0x8c0532a8, 'bank05.asm', 8086, 8197, '-'),   # move/anim SETUP dispatcher (jsr @tbl 0x8c14ea7c[move]); caller of 04fc9e (pr=0x532fc)
               (0x8c0530d8, 'bank05.asm', 7709, 8079, '-'),   # dispatcher callee (contains block 0x8c05312c)
               (0x8c053460, 'bank05.asm', 8386, 8487, '-'),   # JUMP move handler (table entry); tail-branches to 0x8c04fc9e
               (0x8c053c8e, 'bank05.asm', 9664, 9726, '-'),   # move-handler callee
               (0x8c034e8c, 'bank03.asm', 11669, 11792, '-'),  # load-animation (Obj anim (re)load on move transition)
               # --- AIRBORNE physics (the jump ARC, reached only once airborne -> separate tree from
               # jump-START). Trace (runner --trace on _airborne_seed): 0x8c052810 -> 0x8c04823e
               # (physics dispatcher) -> [char SPL 0x8ce324d6] -> 0x8c034dee (anim tick) -> 0x8c04fea8
               # (integrator: pos_y += y_vel @+0x38, y_vel -= gravity @+0x60). Both bank04 fns were GAP.
               (0x8c04823e, 'bank04.asm', 19775, 20013, '-'),  # airborne physics dispatcher (blocks 0x8c0482d6..332; caller of integrator, pr=0x48338)
               (0x8c04fea8, 'bank04.asm', 38924, 39068, '-'),  # airborne integrator: pos_y+=y_vel, y_vel-=gravity
               # --- WALK (grounded lateral move; the LIVE-state "injected input does nothing" gap:
               # the ground-state dispatcher is only reached grounded+directional, never by the
               # airborne test seeds). Trace (runner --trace on _inj_walk): 0x8c047f8e (ground/walk
               # state dispatcher) -> 0x8c04fd92 ("Walk forward": x_velocity=5.8333 @+0x5C). Both GAP.
               (0x8c047f8e, 'bank04.asm', 19351, 19773, '-'),  # ground-state / walk dispatcher (blocks 0x8c0480f4..188)
               (0x8c04fd92, 'bank04.asm', 38742, 38889, '-'),  # "Walk forward": sets x_velocity (+0x5C) = walk speed
               # --- WALK move-transition tree (the 6 mc_unknown_call gaps the executor hits on a
               # grounded Right/Left press before it can reach the ground dispatcher; probe on
               # _inj_walk). loc_8c0533fc = walk move handler (tbl 0x14ea7c[1]); the rest are its
               # setup/anim callees. Crank these -> executor follows the real walk path.
               (0x8c0533fc, 'bank05.asm', 8316, 8384, '-'),   # walk move handler (dispatch table[1])
               (0x8c0529ec, 'bank05.asm', 6565, 6657, '-'),   # walk callee
               (0x8c054ee2, 'bank05.asm', 12786, 12887, '-'), # walk callee
               (0x8c055be4, 'bank05.asm', 14896, 14944, '-'), # walk callee
               (0x8c055c3a, 'bank05.asm', 14946, 15051, '-'), # walk callee
               (0x8c049562, 'bank04.asm', 22760, 22768, '-'),  # walk callee (small leaf)
               (0x8c03319e, 'bank03.asm', 7354, 7382, '-'),   # walk callee (last mc_unknown gap on the walk tick)
               # --- PUSHBOX SEPARATION (the walkL/backward-walk f17 divergence): loc_8c04f974 is
               # bsr'd from loc_8c04f94e (r4=self,r5=opp,r6=self-box) on the collision-overlap path
               # (loc_8c04f81a), reached only when pushboxes overlap -> executed on walkL, NEVER walkR
               # (walkR byte-exact). Pure leaf. Writes self.pos_x(+0x34)+=push, opp.pos_x(+0x34)-=push
               # (clamp 0x42555555=53.33), and OR's the corner/touch bitmask into GLOBAL 0x8c28963c.
               # Was mc_unknown_call (no-op) -> push dropped -> pos_x short -> x_opponent_distance(+0x298,
               # recomputed downstream by loc_8c051280) + gs+0x63C wrong. re_kb candidate.
               (0x8c04f974, 'bank04.asm', 38056, 38097, '-'),  # pushbox separation (mutual pos_x push + gs 0x28963C touch flag)
               # --- JUMP FALL-TRANSITION (the jump f19/f24 pos_y+y_velocity divergence, Storm seed):
               # the ROOT is the Storm SPL airborne handler FUN_ce324d6 (-> loc_0ce324d6, S_PL2A reloc 0),
               # dispatched via jsr @rN (anim/move pointer-table @ #data FUN_ce324d6) at the apex->fall
               # keyframe. It SETS y_velocity(+0x60) to the fixed fall speed DAT_ce325ac=0xc0892492=-4.28571
               # (or DAT_ce325b0=0xc1092492) and y_drag(+0x6C), and drifts pos_x(+0x34). Was mc_unknown_call
               # (no-op) -> y_velocity never SET -> integrator kept free-fall gravity -> pos_y/y_velocity
               # diverge (+ downstream anim +0x142/144/154 at f20). NOT a pure leaf: jsr @PTR_ce325a8 =
               # loc_8c0335b0 (below). SPL fn covers offsets past the trace-built worklist. re_kb candidate.
               (0x0ce324d6, 'S_PL2A.asm', 5729, 5831, '5835-5868'),  # Storm airborne fall-velocity handler
               # loc_8c0335b0 = the ground-contact PREDICATE that FUN_ce324d6 jsr's (PTR_ce325a8): reads
               # pos_y+0x38 / y_scale+0x54 / world ground ref *loc_8c033668, returns r0=1 if next-frame Y
               # would touch/pass ground. A CALLEE of the root above (cranking it alone did nothing; kept
               # because the SPL handler calls it). Pure leaf. re_kb candidate.
               (0x8c0335b0, 'bank03.asm', 7941, 7968, '-'),   # ground-contact predicate (callee of FUN_ce324d6)
               # --- CROUCH-START (crouch mode f2, anim block +0x140/142/144/151/152/154): loc_8c053504
               # = crouch state handler (sets stance+0x1f9=1, zeroes y_vel+0x60/y_drag+0x6C, jmp load-anim
               # loc_8c034e8c grp3). loc_8c048474 = bank04 ground input-state driver (jsr-cascade of move
               # predicates) that dispatches into it. Both were mc_unknown_call. re_kb candidates.
               (0x8c053504, 'bank05.asm', 8489, 8528, '-'),   # crouch-start state handler (stance=1, load crouch anim)
               (0x8c048474, 'bank04.asm', 20145, 20256, '-'), # ground input-state driver (move-predicate cascade)
               # --- WALK-STOP / decel (walkStop mode f10, c0+0x38): loc_8c053348 = stop/neutral state
               # handler (zeroes x_vel+0x5C/y_vel+0x60/JumpUNK+0x68/y_drag+0x6C, per-char dispatch). Was no-op.
               (0x8c053348, 'bank05.asm', 8201, 8313, '-'),   # stop/neutral state handler (zero velocities)
               # --- JUMP-BACK (jumpB mode f28, c1/Cable anim +0x130/142/144/154): loc_8c0535c8 = group-4
               # anim state handler (stance=0, zero all vel, jsr+jmp load-anim grp4 anim0). re_kb candidate.
               (0x8c0535c8, 'bank05.asm', 8610, 8635, '-')]   # group-4 anim state handler

def parse_worklist():
    funcs = list(EXTRA_FUNCS)
    seen = {a for a, _, _, _, _ in funcs}
    for path in (FUNCLIST_FULL, SPL_FUNCLIST_FULL, FUNCLIST, SPL_FUNCLIST):
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
    tailcall_targets = set()   # every cross-;==== branch rewritten to call_addr(target); return
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
                if hx:
                    tailcall_targets.add(int(hx.group(1), 16))
                    return f"{{ call_addr(c, 0x{hx.group(1)}u); return; }}"
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
    # DURABLE STACK-LEAK GUARD: every cross-;==== branch is emitted as call_addr(target); return.
    # If `target` isn't the entry/mid-block of a transpiled function it hits mc_unknown_call (no-op)
    # -> a `;====` continuation that pops the stack never runs -> r15 leak -> bad indirect call ->
    # hang (the JUMP-input class). List any uncovered target so it can be added to EXTRA_FUNCS.
    uncranked = sorted(t for t in tailcall_targets if t not in block_of)
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
        f.write('#ifdef MC_BALANCE\n unsigned int _sp0=c->r[15];\n#endif\n')
        f.write('#ifdef MC_DHOOK\n mc_dispatch_hook(a,c);\n#endif\n switch(a){\n')
        for ba, entry in sorted(block_of.items()):
            f.write(f'  case 0x{ba:08x}u: fn_{entry:08x}(c, 0x{ba:08x}u); break;\n')
        f.write('  default: mc_unknown_call(a); { extern void mc_unk_regs(u32*); mc_unk_regs(c->r); } break;\n }\n')
        # MC_BALANCE: a dispatched fn that returns with r15 != entry leaked the SH4 stack — a
        # `;==== ` continuation that pops didn't run (uncranked). Flags the executed stack-leak
        # class at runtime (precise; static guard over-reports never-executed branches).
        f.write('#ifdef MC_BALANCE\n if(c->r[15]!=_sp0){ extern void mc_bal(u32,u32,u32); mc_bal(a,_sp0,c->r[15]); }\n#endif\n')
        f.write(' mc_pop();\n}\n')
        f.write(f'\nvoid tick_entry(Sh4Ctx*c){{ fn_{ENTRY:08x}(c, 0x{ENTRY:08x}u); }}\n')

    print(f"worklist: {len(funcs)} | transpiled OK: {len(ok)} | FAILED: {len(fail)}")
    if uncranked:
        print(f"--- {len(uncranked)} UNCRANKED tail-call continuation(s) (stack-leak risk; add to EXTRA_FUNCS) ---")
        for t in uncranked:
            print(f"  loc_{t:08x}  bank{((t>>16)&0xff):02x}.asm")
    else:
        print("stack-leak guard: all cross-;==== branch targets are cranked (no uncranked continuations)")
    if fail:
        print("--- failures (need a codegen/pool gap) ---")
        from collections import Counter
        by = Counter(f"{ex}: {msg}" for _, _, _, ex, msg in fail)
        for k, c in by.most_common(20): print(f"  {c:3d}x  {k}")
    with open('gen_tick_fail.txt', 'w') as f:
        for a, bank, lines, ex, msg in fail: f.write(f"sub_{a:08x} {bank} {lines} {ex}: {msg}\n")

if __name__ == '__main__':
    main()
