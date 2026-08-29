#!/usr/bin/env python3
"""
Function-level codegen: turn a parsed Insn[] into a complete C function with
native control flow. Handles SH4's delayed branches by emitting the delay-slot
instruction's effect, then the transfer.

Branch model (flycast-accurate ordering):
  bra L / bsr L     : delay-slot executes, then goto L (bsr: set pr first)
  bf L              : if(!T) goto L         (no delay slot)
  bt L              : if(T)  goto L
  bf.s L / bt.s L   : delay-slot executes; if(cond) goto L
  bf/s, bt/s        : aliases of bf.s/bt.s
  jsr @rN           : delay-slot executes; call resolved leaf; (pr set)
  rts               : delay-slot executes; return
Fall-through between consecutive labelled blocks is implicit (C labels).
"""
import re
from codegen import Emitter, R, FR

BRANCHES = {'bra','bsr','bf','bt','bf.s','bt.s','bf/s','bt/s','jsr','rts','jmp','braf','bsrf'}

def _default_bsr(target):
    """bsr loc_<hex> -> call the transpiled C function for that address, named
    sub_<hex>(c) by convention. The caller links a matching sub_<hex> (often a thin
    shim to an already-transpiled leaf, e.g. sub_8c11e730 -> rng_e730)."""
    t=target.lower()
    m=re.fullmatch(r'loc_([0-9a-fA-F]+)', t)
    if m: return f"sub_{m.group(1)}(c);"
    raise NotImplementedError(f"bsr target {target}")

def emit_function(insns, data, fname, leaf_call_resolver, bsr_call_resolver=_default_bsr,
                  jmp_call_resolver=None):
    """leaf_call_resolver(jsr_reg_or_target) -> C statement to call the leaf.
    bsr_call_resolver(target_label) -> C statement to call the bsr subroutine.
    jmp_call_resolver(jmp_reg) -> C statement to tail-dispatch a jmp @rN target."""
    em=Emitter(data, {})
    body=[]
    def out(s): body.append("    "+s)
    def outl(s): body.append(s)

    i=0
    n=len(insns)
    while i<n:
        ins=insns[i]
        if ins.label:
            outl(f"{ins.label}:; /* bb */")
        elif ins.pc is not None:
            # label EVERY instruction so computed jumps / jump-tables to arbitrary
            # (unlabeled) addresses are re-enterable via the function entry-switch.
            outl(f"loc_{ins.pc:08x}:;")
        m=ins.mnem
        if m in BRANCHES:
            # collect the (single) delay-slot insn if delayed
            delayed = m in ('bra','bsr','bf.s','bt.s','bf/s','bt/s','jsr','rts','jmp','braf','bsrf')
            ds=None
            if delayed and i+1<n:
                ds=insns[i+1]
            # REGISTER-INDIRECT delayed branches (jsr/jmp/braf/bsrf) latch the target
            # register BEFORE the delay slot runs — and the delay slot may OVERWRITE that
            # register (e.g. jmp @r3 / mov.l @(r0,r2),r3). Capture the target first.
            tgtvar=None
            if m in ('jsr','jmp','braf','bsrf'):
                reg=ins.args[0].lstrip('@')
                if m in ('braf','bsrf'):
                    if ins.pc is None: raise NotImplementedError(m+" needs pc: "+ins.raw)
                    base=f"0x{ins.pc+4:08x}u + "
                else:
                    base=""
                tgtvar=f"_tgt{i}"
                out(f"u32 {tgtvar} = {base}{R(reg)};")
            # emit delay slot (its effect happens before the transfer completes)
            if ds is not None:
                _emit_one(em, ds, body)
            # now the transfer
            if m=='bra':
                out(f"goto {ins.args[0].lower()};")
            elif m=='bsr':
                # bsr loc_X -> call the subroutine (pr save/restore is handled by
                # the caller's sts.l/lds.l pr around the bsr; the C call is enough).
                out(bsr_call_resolver(ins.args[0]))
            elif m=='bf':
                out(f"if(!c->sr_t) goto {ins.args[0].lower()};")
            elif m=='bt':
                out(f"if(c->sr_t) goto {ins.args[0].lower()};")
            elif m in ('bf.s','bf/s'):
                out(f"if(!c->sr_t) goto {ins.args[0].lower()};")
            elif m in ('bt.s','bt/s'):
                out(f"if(c->sr_t) goto {ins.args[0].lower()};")
            elif m=='jsr' or m=='bsrf':
                # jsr @rN / bsrf rN -> call the latched target (call, no return).
                out(leaf_call_resolver(tgtvar))
            elif m=='jmp' or m=='braf':
                # jmp @rN / braf rN -> TAIL CALL to the latched target, then return.
                if jmp_call_resolver is None:
                    raise NotImplementedError(m+" needs jmp_call_resolver: "+ins.raw)
                out(jmp_call_resolver(tgtvar))
                out("return;")
            elif m=='rts':
                out("return;")
            else:
                raise NotImplementedError(ins.raw)
            # BRANCH-INTO-DELAY-SLOT: when the consumed delay-slot insn is ALSO an explicit branch
            # target (e.g. `bra L2; L: nop` where another `bf L` jumps to L), the label L must exist
            # AND a jump to L must run the slot insn then FALL THROUGH to the following code — NOT
            # re-take the branch. The delay slot is consumed above (dropping its label), so a `goto L`
            # would otherwise resolve to nothing -> a no-op tail-call = a silent divergence (same class
            # as a missing function). Emit a labeled fall-through copy of the slot insn. For branch
            # types that FALL THROUGH after the transfer (bsr/jsr/bsrf calls; bf.s/bt.s not-taken), skip
            # the stub on the normal path so the slot isn't executed twice.
            if delayed and ds is not None and ds.label:
                falls = m in ('bsr','jsr','bsrf','bf.s','bt.s','bf/s','bt/s')
                after = f"_dsafter_{i}"
                if falls: out(f"goto {after};")
                outl(f"{ds.label}:; /* labeled delay-slot: jump-in runs slot then falls through */")
                _emit_one(em, ds, body)   # emits the slot insn's EFFECT only (label handled here)
                if falls: outl(f"{after}:;")
            i += 2 if (delayed and ds is not None) else 1
            continue
        # normal insn
        _emit_one(em, ins, body)
        i+=1
    # assemble
    em.lines=[]  # codegen wrote into em.lines via emit; but we routed through body
    return "\n".join(body)

def _emit_one(em, ins, body):
    # temporarily redirect em.lines to capture this insn's C
    saved=em.lines
    em.lines=[]
    em.emit_insn(ins)
    body.extend(em.lines)
    em.lines=saved
