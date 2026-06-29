#!/usr/bin/env python3
"""Generate C for the SCALE walker loc_8c0348c8 (bank03) -> gen_walker_scale.c.

The engine per-object dispatcher loc_8c034bea routes sel & 0x8000 (bit15-set) objects
here (super/projectile EFFECT nodes) instead of the tiling body walker loc_8c0344d4.
This routine emits ONE scaled sprite per GFX2 cell record (no inner tile loop), via the
SAME leaves (e460 floor, e2e0 cos, e860 sin, 1244b0 submit) as loc_8c0344d4. Cloned from
gen_walker.py. (re_kb/50 super-freeze over-tile fix.)
"""
import re
from lift import parse_asm, extract_block, slurp_function
from codegen import Emitter, R, FR

BANK03=r"C:\Users\trist\projects\_marv_re\build\bank03.asm"

# loc_8c0348c8 code spans 10800..11256 (epilogue rts at 11255), with #data pools
# interleaved (10854-10878, 11067-11102). Feed the whole contiguous range; the lifter
# treats data-only labels as harmless (same as gen_walker.py).
WALKER_RANGES=[(10800,11256),(11291,11310)]

LEAF_DISPATCH = {
    'bank11.loc_8c11e460': 'leaf_e460(c);',
    'bank11.loc_8c11e2e0': 'leaf_e2e0(c);',
    'bank11.loc_8c11e860': 'leaf_e860(c);',
    'bank12.loc_8c1244b0': 'submit_1244b0(c);',
}

def build():
    text=slurp_function(BANK03,None)
    blk="\n".join(extract_block(text,a,b) for (a,b) in WALKER_RANGES)
    insns,data=parse_asm(blk)

    em=Emitter(data,{})
    body=[]
    tag_for={}
    for k,v in data.items():
        if v.lower().startswith('bank'):
            tag=em.leaf_tag(v); tag_for[tag]=v
    def resolver(regarg):
        reg=regarg.strip('@')
        cases=[]
        for tag,bankref in tag_for.items():
            call=LEAF_DISPATCH.get(bankref, '/*unknown leaf*/;')
            cases.append(f"if(({R(reg)} & 0xFFF00000u)==0x1EA00000u && {R(reg)}==0x{tag:08x}u){{ {call} }}")
        return " else ".join(cases) + " else { /* unresolved jsr */ }"

    i=0; n=len(insns)
    BRANCHES={'bra','bsr','bf','bt','bf.s','bt.s','bf/s','bt/s','jsr','rts','jmp'}
    while i<n:
        ins=insns[i]
        if ins.label: body.append(f"{ins.label}:; /* bb */")
        m=ins.mnem
        if m in BRANCHES:
            delayed = m not in ('bf','bt')
            ds=insns[i+1] if (delayed and i+1<n) else None
            if ds is not None:
                _one(em,ds,body)
            if m=='bra': body.append(f"    goto {ins.args[0].lower()};")
            elif m=='bf': body.append(f"    if(!c->sr_t) goto {ins.args[0].lower()};")
            elif m=='bt': body.append(f"    if(c->sr_t) goto {ins.args[0].lower()};")
            elif m in ('bf.s','bf/s'): body.append(f"    if(!c->sr_t) goto {ins.args[0].lower()};")
            elif m in ('bt.s','bt/s'): body.append(f"    if(c->sr_t) goto {ins.args[0].lower()};")
            elif m=='jsr': body.append("    "+resolver(ins.args[0]))
            elif m=='rts': body.append("    return;")
            else: raise NotImplementedError(ins.raw)
            i += 2 if (delayed and ds is not None) else 1
            continue
        _one(em,ins,body)
        i+=1

    with open("gen_walker_scale.c","w") as f:
        f.write('#include "sh4ctx.h"\n')
        f.write('void leaf_e460(Sh4Ctx*);\nvoid leaf_e2e0(Sh4Ctx*);\nvoid leaf_e860(Sh4Ctx*);\nvoid submit_1244b0(Sh4Ctx*);\n\n')
        f.write("/* AUTO-GENERATED from bank03.asm loc_8c0348c8 (do not edit) */\n")
        f.write("void walker_0348c8(Sh4Ctx *c){\n")
        f.write("\n".join(body))
        f.write("\n}\n")
    print("wrote gen_walker_scale.c  (", len(insns), "insns,", sum(1 for ins in insns if ins.label),"bbs )")

def _one(em,ins,body):
    saved=em.lines; em.lines=[]
    em.emit_insn(ins)
    body.extend("    "+l.strip() if not l.strip().endswith(':; /* bb */') else l for l in em.lines)
    em.lines=saved

if __name__=="__main__":
    build()
