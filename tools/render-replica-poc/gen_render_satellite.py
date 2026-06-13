#!/usr/bin/env python3
"""Transpile loc_8c030af8 (bank03:1526) — the CAT 1..4 SATELLITE body-sprite setup, the
sibling of loc_8c03093c ("Render Main Sprite"). The slot-walk loc_8c0308c2 routes
node+0x3==0 -> loc_8c03093c (body) and node+0x3 in [1,5) -> loc_8c030af8 (satellite:
capes/projectiles/drones/assists/extra-limbs). loc_8c030af8 DEPOSITS THE SAME per-frame
fields the body walker loc_8c0344d4 consumes and emits a body sprite through that SAME
walker (ASMTRACE PC 0x8C034864) — so a satellite renders exactly like a body once its
anchor/scale/field deposits are computed.

FAITHFUL FIELD-BY-FIELD vs loc_8c03093c (bank03 line refs):
  SAME (byte-identical math):
    +0x12C visibility gate            (af8 1530-1536  | 93c 1287-1291)
    transform loc_8c122560 read       fr6=+0x3C fr5=+0x38 fr4=+0x34
                                      (af8 1559-1571  | 93c 1303-1315)
    write +0xE0/+0xE4 anchor          (af8 1572-1579  | 93c 1316-1323)
    +0xE8 depth = fmac(0x3dcccccd, node-table@0x8c26a974[idx], ...) (af8 1584-1591 | 93c 1324-1335)
    +0xEC = CpsXScale * node[+0x50]   (af8 1592-1598  | 93c 1336-1342)
    +0xF0 = CpsYScale * node[+0x54]   (af8 1599-1605  | 93c 1343-1349)
    +0x104 = node[+0x48]              (af8 1606-1609  | 93c 1350-1353)
    +0x130 = node[+0x130]  +0x100=0   (af8 1610-1616  | 93c 1354-1360)
    +0x134 = node[+0x4C] +0x136=node[+0x4E] (af8 1617-1624 | 93c 1361-1368)
    +0xEC/+0xF0 /= (node[+0x20]/0x444b16de) (af8 1625-1637 | 93c 1369-1381)
    zoom gate read node[+0x14D]       (af8 1638-1641  | 93c 1382-1385)
  DIFFERENT (the two per-cat deltas — TRANSPILED EXACTLY, not assumed identical):
    (1) NO loc_8c02e1a4 + NO loc_8c1216c0 proj-setup before the transform. loc_8c03093c
        calls both first (93c 1294-1301); loc_8c030af8 goes straight to loc_8c122560
        (af8 1570). Those two are frame-global proj setup -> STUB in both transpiles
        (matrices are read resident in transform_object_122560), so skipping them
        produces IDENTICAL output. (af8 just lacks the dead calls.)
    (2) The +0xE8 depth-table INDEX is taken via a stack scratch slot: node[+0x24] is
        saved to @(0xC,r15) early (af8 1558/1563) then reloaded, <<2, stored @(0x10,r15)
        (af8 1580-1583). Same VALUE (node[+0x24]<<2 into table 0x8c26a974) as 93c's
        inline `mov.b @(0x24,r14); shll2` (93c 1324-1327). Math identical; mechanics differ.
    (3) The ZOOM BRANCH (node[+0x14D]!=0, af8 1642-1701) indexes a DIFFERENT table than
        93c. 93c (1386-1396) reads node[+0x14d] then node[+0x168] (own animations ptr),
        idx = node[+0x14d]*16 into it. af8 (1642-1657) reads node[+0x1a4] (owner index)
        * 0x5A4 (PAIR STRIDE!) -> *(0x8c2684a8 + that) -> the OWNER CHAR STRUCT, then
        + node[+0x24d]*16. i.e. a satellite fetches its zoom anim from its OWNER fighter,
        not from itself. We transpile this verbatim. (It is GATED OFF when node[+0x14D]==0,
        which the validated Cable drone node 0x8C271E54 has — so for that node af8's
        deposits == 93c's deposits == 0.00px; the zoom delta only matters for zoom-anim
        satellites and is faithful regardless.)
  TAIL accumulate (af8 1703-1717 + 1769-1775): fmac into the per-char rparam accum
    (0x8c26a974), same as 93c's tail (1441-1461) -> STUB (global accum, not a walker field).

JSR routing (resolved by pool tag, mirrors gen_render_object.py):
  loc_8c030c80 -> bank12.loc_8c122560 -> transform_object_122560(c,node)  (deposits E0/E4/E8)
  loc_8c030ca4 -> loc_8c034bea          -> STUB (global accum helper)
  loc_8c030ca8 -> work.GameGlobalPointer (data ptr, not a call)

This generator EMITS gen_render_satellite.c: render_object_setup_030af8(c) with r4/r14=node.
"""
import re
from lift import parse_asm, extract_block, slurp_function
from codegen import Emitter, R, FR

BANK03 = r"C:\Users\trist\projects\_marv_re\build\bank03.asm"
WORK_ASM = r"C:\Users\trist\projects\_marv_re\memory\work.asm"

def load_work_symbols():
    syms = {}
    for line in open(WORK_ASM, errors='replace'):
        m = re.match(r'\s*#symbol\s+(\w+)\s+(0x[0-9a-fA-F]+)', line)
        if m: syms[m.group(1).lower()] = m.group(2).lower()
    return syms

def resolve_work_refs(data, syms):
    for k, v in list(data.items()):
        vl = v.lower()
        if vl.startswith('work.'):
            name = vl.split('.', 1)[1]
            if name in syms: data[k] = syms[name]
    return data

# loc_8c030af8 body (bank03:1526..1717) + its pool block (1719..1766) +
# the tail loc_8c030cac/loc_8c030cb8 (1769..1781) + its two pool words (1923..1926).
RANGES = [(1526, 1717), (1719, 1766), (1769, 1781), (1923, 1926)]

LEAF_DISPATCH = {
    'bank12.loc_8c122560': 'transform_object_122560(c, c->r[14]); /* per-object world->screen, deposits +0xE0/E4/E8 */',
    'loc_8c034bea':        '/* loc_8c034bea global-accum helper (stub) */',
}

def build():
    text = slurp_function(BANK03, None)
    blk = "\n".join(extract_block(text, a, b) for (a, b) in RANGES)
    insns, data = parse_asm(blk)
    data = resolve_work_refs(data, load_work_symbols())

    em = Emitter(data, {})
    body = []
    tag_for = {}
    for k, v in data.items():
        if v.lower().startswith('bank') or v.lower().startswith('loc_'):
            try:
                tag = em.leaf_tag(v); tag_for[tag] = v
            except Exception:
                pass

    def resolver(regarg):
        reg = regarg.strip('@')
        cases = []
        for tag, ref in tag_for.items():
            call = LEAF_DISPATCH.get(ref)
            if call is None: continue
            cases.append(f"if({R(reg)}==0x{tag:08x}u){{ {call} }}")
        if not cases: return "/* unresolved jsr (no known leaf) */"
        return " else ".join(cases) + " else { /* unresolved jsr */ }"

    i = 0; n = len(insns)
    BRANCHES = {'bra', 'bsr', 'bf', 'bt', 'bf.s', 'bt.s', 'bf/s', 'bt/s', 'jsr', 'rts', 'jmp'}
    while i < n:
        ins = insns[i]
        if ins.label: body.append(f"{ins.label}:; /* bb */")
        m = ins.mnem
        if m in BRANCHES:
            delayed = m not in ('bf', 'bt')
            ds = insns[i+1] if (delayed and i+1 < n) else None
            if ds is not None: _one(em, ds, body)
            if m == 'bra': body.append(f"    goto {ins.args[0].lower()};")
            elif m == 'bf': body.append(f"    if(!c->sr_t) goto {ins.args[0].lower()};")
            elif m == 'bt': body.append(f"    if(c->sr_t) goto {ins.args[0].lower()};")
            elif m in ('bf.s', 'bf/s'): body.append(f"    if(!c->sr_t) goto {ins.args[0].lower()};")
            elif m in ('bt.s', 'bt/s'): body.append(f"    if(c->sr_t) goto {ins.args[0].lower()};")
            elif m == 'jsr': body.append("    " + resolver(ins.args[0]))
            elif m == 'rts': body.append("    return;")
            else: raise NotImplementedError(ins.raw)
            i += 2 if (delayed and ds is not None) else 1
            continue
        _one(em, ins, body)
        i += 1

    with open("gen_render_satellite.c", "w") as f:
        f.write('#include "sh4ctx.h"\n')
        f.write('void transform_object_122560(Sh4Ctx*, u32 node_addr);\n\n')
        f.write("/* AUTO-GENERATED from bank03.asm loc_8c030af8 (do not edit) — the CAT 1..4\n")
        f.write("   SATELLITE setup. Entry: r4=node base. Deposits +0xE0/E4/E8 (transform),\n")
        f.write("   +0xEC/F0 (scale), +0x104/110/130/134/136 from the node — SAME walker fields\n")
        f.write("   as loc_8c03093c; the only per-cat deltas are the (gated-off) zoom table and\n")
        f.write("   the skipped proj-setup calls. NO engine-TA / no pinning. See gen_render_satellite.py. */\n")
        f.write("void render_object_setup_030af8(Sh4Ctx *c){\n")
        f.write("\n".join(body))
        f.write("\n}\n")
    print("wrote gen_render_satellite.c  (", len(insns), "insns,",
          sum(1 for ins in insns if ins.label), "bbs )")

def _one(em, ins, body):
    saved = em.lines; em.lines = []
    em.emit_insn(ins)
    body.extend("    " + l.strip() if not l.strip().endswith(':; /* bb */') else l for l in em.lines)
    em.lines = saved

if __name__ == '__main__':
    build()
