#!/usr/bin/env python3
"""
SH4 -> C code emitter for the Option-C PoC.

Consumes parsed Insn[] + data{} (from lift.py) and emits a C function body.
Per-opcode semantics mirror flycast's interpreter (determinism-validated):
  - fmac      -> std::fmaf  (FUSED multiply-add, single rounding) [sh4_fpu.cpp:559]
  - ftrc      -> (u32)(s32)f with the 0x7fffff80 overflow clamp     [sh4_fpu.cpp:522]
  - float     -> (float)(s32)fpul
  - exts.w    -> (s32)(s16)
  - extu.w/b  -> zero-extend
  - muls.w    -> macl = (s32)(s16)a * (s32)(s16)b
  - mov.w @(d,r),Rn (sign-ext 16), mov.b sign-ext 8, mov.l 32-bit
Big-endian guest loads/stores against flat ram[] (translate(a)=a&0x00FFFFFF for
area-3 RAM; this PoC only touches RAM + the synthesized node/tables).

Control flow: each labelled insn becomes a C `case`/label in a computed-goto-free
dispatch; SH4 delay slots are emitted BEFORE the branch's control transfer.
For these two functions (no indirect data-driven jumps within the lifted body
except the `jsr @r9` leaf calls, which we resolve to the named leaf), straight
structured C with goto labels suffices.
"""
import re

# ---- register access helpers in emitted C ----------------------------------
def R(r):   # integer reg name -> ctx field
    assert r.startswith('r'); return f"c->r[{int(r[1:])}]"
def FR(f):  # fr0..fr15
    assert f.startswith('fr'); return f"c->fr[{int(f[2:])}]"

def is_reg(x): return re.fullmatch(r'r\d+', x) is not None
def is_freg(x): return re.fullmatch(r'fr\d+', x) is not None

def imm(x):
    """Parse an immediate like 0x20, 0xFF, 0x00, 0xE4."""
    x=x.strip()
    return int(x,16) if x.lower().startswith('0x') else int(x,16) if re.fullmatch(r'[0-9a-fA-F]+',x) else int(x)

class Emitter:
    def __init__(self, data, leafmap):
        self.data=data            # label -> "0x..." or "bankNN.loc_.."
        self.leafmap=leafmap      # data-word value -> C function name to call
        self.lines=[]
        self.tmp=0
        self._leaftags={}         # bankref -> synthetic tag value
        self._leaforder=[]
    def leaf_tag(self, bankref):
        if bankref not in self._leaftags:
            self._leaftags[bankref]=(0x1EA00000 | len(self._leaftags))
            self._leaforder.append(bankref)
        return self._leaftags[bankref]
    def t(self):
        self.tmp+=1; return f"t{self.tmp}"
    def emit(self,s): self.lines.append("    "+s)
    def label(self,l): self.lines.append(f"{l}:; /* bb */")

    # ---- resolve a pool data reference @(loc_X,PC) to its constant ----------
    def pool_const(self, locref):
        # locref like 'loc_8c0345fc' -> data value
        key=locref.lower()
        v=self.data.get(key)
        if v is None: raise KeyError(f"pool {locref} not in data")
        if v.lower().startswith('0x'): return int(v,16)
        # bank ref -> not a constant; handled separately
        return v

    def emit_insn(self, ins):
        m=ins.mnem; a=ins.args; raw=ins.raw
        self.emit(f"/* {raw} */")
        # ---------------- moves ----------------
        if m=='mov':
            # mov #imm,Rn  OR  mov Rm,Rn
            if is_reg(a[0]):
                self.emit(f"{R(a[1])} = {R(a[0])};")
            else:
                v=imm(a[0]);
                # sign-extend 8-bit immediate (SH4 mov #imm is signed 8-bit)
                if v & 0x80: v = v - 0x100 if v<=0xFF else v
                self.emit(f"{R(a[1])} = (u32)(s32)({v});")
        elif m=='mov.w':
            self._mem(ins, 'w')
        elif m=='mov.l':
            self._mem(ins, 'l')
        elif m=='mov.b':
            self._mem(ins, 'b')
        elif m=='mova':
            # mova @(loc_X,pc),r0  -> r0 = address-of-pool. We store a synthetic
            # tag so a following fmov.s @r0 loads the data word. We model r0 as a
            # special "POOLADDR|<const idx>". Simplest: stash the float bits.
            locref=re.search(r'@\(([^,]+),', a[0]).group(1)
            val=self.pool_const(locref)
            if isinstance(val, str) or not isinstance(val, int):
                # jump-table / unresolvable-value pool: r0 = the pool's real guest ADDRESS
                # so @(r0,rN) or @r0 dereferences read the table from RAM (bank data resident).
                try: addr=int(str(locref)[4:],16)
                except ValueError: addr=0
                self.emit(f"c->r[0] = 0x{addr:08x}u; c->_pool = 0x{addr:08x}u; /* mova {locref} table-addr */")
            else:
                self.emit(f"c->r[0] = 0xF0000000u; c->_pool = 0x{val:08x}u; /* mova {locref} */")
        # ---------------- fmov family ----------------
        elif m=='fmov' or m=='fmov.s':
            self._fmov(ins)
        # ---------------- fp arithmetic ----------------
        elif m=='fadd':
            self.emit(f"{FR(a[1])} = {FR(a[1])} + {FR(a[0])};")
        elif m=='fsub':
            self.emit(f"{FR(a[1])} = {FR(a[1])} - {FR(a[0])};")
        elif m=='fmul':
            self.emit(f"{FR(a[1])} = {FR(a[1])} * {FR(a[0])};")
        elif m=='fdiv':
            self.emit(f"{FR(a[1])} = {FR(a[1])} / {FR(a[0])};")
        elif m=='fmac':
            # fmac FR0, FRm, FRn : FRn = fma(FR0, FRm, FRn)  (single rounding!)
            self.emit(f"{FR(a[2])} = fmaf({FR('fr0')}, {FR(a[1])}, {FR(a[2])});")
        elif m=='fabs':
            self.emit(f"{FR(a[0])} = fabsf({FR(a[0])});")
        elif m=='fneg':
            self.emit(f"{FR(a[0])} = -{FR(a[0])};")
        elif m=='fldi0':
            self.emit(f"{FR(a[0])} = 0.0f;")
        elif m=='fldi1':
            self.emit(f"{FR(a[0])} = 1.0f;")
        elif m=='float':
            # float FPUL, FRn : FRn = (float)(s32)fpul
            self.emit(f"{FR(a[1])} = (float)(s32)c->fpul;")
        elif m=='ftrc':
            # ftrc FRm, FPUL : fpul = (u32)(s32)fr ; clamp
            fr=FR(a[0])
            self.emit(f"{{ float _f={fr}; if(_f!=_f) c->fpul=0x80000000u; else {{ c->fpul=(u32)(s32)_f; if((s32)c->fpul>0x7fffff80) c->fpul=0x7fffffffu; }} }}")
        elif m=='lds':
            # lds Rm, FPUL
            if a[1].upper()=='FPUL': self.emit(f"c->fpul = {R(a[0])};")
            else: raise NotImplementedError(raw)
        elif m=='flds':
            # flds FRm, FPUL : fpul = raw bits of FRm
            self.emit(f"c->fpul = ((union {{ float f; u32 u; }}){{ .f = {FR(a[0])} }}).u;")
        elif m=='fsts':
            # fsts FPUL, FRn : FRn bits = fpul
            self.emit(f"{FR(a[1])} = ((union {{ float f; u32 u; }}){{ .u = c->fpul }}).f;")
        elif m=='fsqrt':
            # sqrtf (not __builtin_sqrtf): resolves via CRT on MSVC, libm on clang, and the
            # freestanding-wasm stub — all correctly-rounded IEEE sqrt, so byte-exact.
            self.emit(f"{FR(a[0])} = sqrtf({FR(a[0])});")
        elif m=='sts':
            # sts FPUL,Rn  or sts MACL,Rn
            src=a[0].upper()
            if src=='FPUL': self.emit(f"{R(a[1])} = c->fpul;")
            elif src=='MACL': self.emit(f"{R(a[1])} = c->macl;")
            elif src=='MACH': self.emit(f"{R(a[1])} = c->mach;")
            elif src=='PR': self.emit(f"{R(a[1])} = c->pr;")
            else: raise NotImplementedError(raw)
        elif m=='stc':
            # control regs: SR is game-logic-inert (IMASK/BL don't affect gameplay); GBR real.
            src=a[0].lower()
            if src=='sr':   self.emit(f"{R(a[1])} = 0x60000100u; /* stc sr (inert) */")
            elif src=='gbr':self.emit(f"{R(a[1])} = c->gbr;")
            elif src=='vbr':self.emit(f"{R(a[1])} = 0u; /* stc vbr */")
            else: raise NotImplementedError(raw)
        elif m=='ldc':
            dst=a[1].lower()
            if dst=='sr':   self.emit(f"/* ldc {a[0]},sr inert no-op */")
            elif dst=='gbr':self.emit(f"c->gbr = {R(a[0])};")
            elif dst=='vbr':self.emit(f"/* ldc {a[0]},vbr no-op */")
            else: raise NotImplementedError(raw)
        elif m=='stc.l':
            src=a[0].lower()
            if src=='sr':   self.emit("c->r[15]-=4; w32(c, c->r[15], 0x60000100u);")
            elif src=='gbr':self.emit("c->r[15]-=4; w32(c, c->r[15], c->gbr);")
            elif src=='vbr':self.emit("c->r[15]-=4; w32(c, c->r[15], 0u);")
            else: raise NotImplementedError(raw)
        elif m=='ldc.l':
            dst=a[1].lower()
            if dst=='sr':   self.emit("c->r[15]+=4; /* ldc.l @r15+,sr inert */")
            elif dst=='gbr':self.emit("c->gbr = r32(c, c->r[15]); c->r[15]+=4;")
            elif dst=='vbr':self.emit("c->r[15]+=4;")
            else: raise NotImplementedError(raw)
        elif m=='sts.l':
            # sts.l pr,@-r15  or  sts.l macl,@-r15
            src=a[0].lower()
            if src=='pr': self.emit("c->r[15]-=4; w32(c, c->r[15], c->pr);")
            elif src=='macl': self.emit("c->r[15]-=4; w32(c, c->r[15], c->macl);")
            else: raise NotImplementedError(raw)
        elif m=='lds.l':
            dst=a[1].lower()
            if dst=='pr': self.emit("c->pr = r32(c, c->r[15]); c->r[15]+=4;")
            elif dst=='macl': self.emit("c->macl = r32(c, c->r[15]); c->r[15]+=4;")
            else: raise NotImplementedError(raw)
        # ---------------- integer alu ----------------
        elif m=='add':
            if is_reg(a[0]): self.emit(f"{R(a[1])} += {R(a[0])};")
            else:
                v=imm(a[0]);
                if v & 0x80: v = v-0x100
                self.emit(f"{R(a[1])} += (u32)(s32)({v});")
        elif m=='sub':
            self.emit(f"{R(a[1])} -= {R(a[0])};")
        elif m=='neg':
            self.emit(f"{R(a[1])} = (u32)(0 - (s32){R(a[0])});")
        elif m=='and':
            if is_reg(a[0]): self.emit(f"{R(a[1])} &= {R(a[0])};")
            else: self.emit(f"{R(a[1])} &= 0x{imm(a[0]):x}u;")
        elif m=='or':
            if is_reg(a[0]): self.emit(f"{R(a[1])} |= {R(a[0])};")
            else: self.emit(f"{R(a[1])} |= 0x{imm(a[0]):x}u;")
        elif m=='shll2':
            self.emit(f"{R(a[0])} <<= 2;")
        elif m=='shll':
            self.emit(f"{R(a[0])} <<= 1;")
        elif m=='shlr16':
            self.emit(f"{R(a[0])} >>= 16;")
        elif m=='extu.b':
            self.emit(f"{R(a[1])} = {R(a[0])} & 0xFFu;")
        elif m=='extu.w':
            self.emit(f"{R(a[1])} = {R(a[0])} & 0xFFFFu;")
        elif m=='exts.w':
            self.emit(f"{R(a[1])} = (u32)(s32)(s16){R(a[0])};")
        elif m=='exts.b':
            self.emit(f"{R(a[1])} = (u32)(s32)(s8){R(a[0])};")
        elif m=='muls.w':
            self.emit(f"c->macl = (u32)((s32)(s16){R(a[0])} * (s32)(s16){R(a[1])});")
        elif m=='mulu.w':
            self.emit(f"c->macl = (u32)(((u32)((u16){R(a[0])})) * ((u32)((u16){R(a[1])})));")
        # ---------------- integer: mul.l / dt / movt / dynamic shift / carry / divide idiom
        #                  (game-tick opcode set; flycast interpr/sh4_opcodes.cpp) ----------
        elif m=='mul.l':
            self.emit(f"c->macl = (u32)((s32){R(a[1])} * (s32){R(a[0])});")
        elif m=='dt':
            self.emit(f"{R(a[0])} -= 1; c->sr_t = ({R(a[0])}==0);")
        elif m=='movt':
            self.emit(f"{R(a[0])} = c->sr_t;")
        elif m=='shld':
            self.emit(f"{{ u32 _s={R(a[0])}; if(!(_s&0x80000000u)) {R(a[1])} <<= (_s&0x1Fu); else if((_s&0x1Fu)==0) {R(a[1])}=0; else {R(a[1])} = ({R(a[1])} >> ((~_s&0x1Fu)+1)); }}")
        elif m=='addc':
            self.emit(f"{{ u32 _t1={R(a[1])}+{R(a[0])}, _t0={R(a[1])}; {R(a[1])}=_t1+c->sr_t; c->sr_t=(_t0>_t1)||(_t1>{R(a[1])}); }}")
        elif m=='subc':
            self.emit(f"{{ u32 _t1={R(a[1])}-{R(a[0])}, _t0={R(a[1])}; {R(a[1])}=_t1-c->sr_t; c->sr_t=(_t0<_t1)||(_t1<{R(a[1])}); }}")
        elif m=='rotcl':
            self.emit(f"{{ u32 _t=c->sr_t; c->sr_t={R(a[0])}>>31; {R(a[0])}=({R(a[0])}<<1)|_t; }}")
        elif m=='rotcr':
            self.emit(f"{{ u32 _t={R(a[0])}&1u; {R(a[0])}=({R(a[0])}>>1)|(c->sr_t<<31); c->sr_t=_t; }}")
        elif m=='div0s':
            self.emit(f"c->sr_q={R(a[1])}>>31; c->sr_m={R(a[0])}>>31; c->sr_t=c->sr_m^c->sr_q;")
        elif m=='div0u':
            self.emit("c->sr_q=0; c->sr_m=0; c->sr_t=0;")
        elif m=='div1':
            self.emit(f"{{ u32 _oq=c->sr_q; c->sr_q=(({R(a[1])}&0x80000000u)!=0); u32 _orm={R(a[0])}; {R(a[1])}<<=1; {R(a[1])}|=c->sr_t; u32 _orn={R(a[1])}; "
                      f"if(_oq==0){{ if(c->sr_m==0){{ {R(a[1])}-=_orm; c->sr_q^=({R(a[1])}>_orn); }} else {{ {R(a[1])}+=_orm; c->sr_q=(!c->sr_q)^({R(a[1])}<_orn); }} }} "
                      f"else {{ if(c->sr_m==0){{ {R(a[1])}+=_orm; c->sr_q^=({R(a[1])}<_orn); }} else {{ {R(a[1])}-=_orm; c->sr_q=(!c->sr_q)^({R(a[1])}>_orn); }} }} "
                      f"c->sr_t=(c->sr_q==c->sr_m); }}")
        # ---------------- compares (set T) ----------------
        elif m=='tst':
            if is_reg(a[0]): self.emit(f"c->sr_t = (({R(a[0])} & {R(a[1])})==0);")
            else: self.emit(f"c->sr_t = (({R(a[1])} & 0x{imm(a[0]):x}u)==0);")
        elif m=='cmp/gt':
            self.emit(f"c->sr_t = ((s32){R(a[1])} > (s32){R(a[0])});")
        elif m=='cmp/ge':
            self.emit(f"c->sr_t = ((s32){R(a[1])} >= (s32){R(a[0])});")
        elif m=='cmp/pl':
            self.emit(f"c->sr_t = ((s32){R(a[0])} > 0);")
        elif m=='cmp/pz':
            self.emit(f"c->sr_t = ((s32){R(a[0])} >= 0);")
        elif m=='cmp/eq':
            if is_reg(a[0]): self.emit(f"c->sr_t = ({R(a[1])}=={R(a[0])});")
            else:
                v=imm(a[0])
                if v & 0x80: v=v-0x100  # cmp/eq #imm,R0 sign-extends 8-bit imm
                self.emit(f"c->sr_t = ({R(a[1])}==(u32)(s32)({v}));")
        elif m=='fcmp/gt':
            self.emit(f"c->sr_t = ({FR(a[1])} > {FR(a[0])});")
        elif m=='fcmp/eq':
            self.emit(f"c->sr_t = ({FR(a[1])} == {FR(a[0])});")
        # ---------------- matrix / fp-bank state (bank12 transform+submit) ---
        elif m=='ftrv':
            # ftrv XMTRX, FVn : FVn = XMTRX (4x4) * FVn  (column-major, single round)
            # FVn = fr[n], fr[n+1], fr[n+2], fr[n+3]. XMTRX = the XF bank (xf[16]),
            # laid out column-major: result_i = sum_k xf[i + 4*k] * fv[k].
            # flycast sh4_fpu.cpp ftrv: uses the *secondary* (XF) bank as the matrix.
            n=int(a[1][2:])  # FVn -> base fr index (0,4,8,12)
            self.emit("{")
            self.emit(f"  float _v0={FR('fr'+str(n))}, _v1={FR('fr'+str(n+1))}, _v2={FR('fr'+str(n+2))}, _v3={FR('fr'+str(n+3))};")
            for i in range(4):
                # column-major: out_i = M[i+0]*v0 + M[i+4]*v1 + M[i+8]*v2 + M[i+12]*v3
                terms=" + ".join(f"c->xf[{i+4*k}]*_v{k}" for k in range(4))
                self.emit(f"  {FR('fr'+str(n+i))} = {terms};")
            self.emit("}")
        elif m=='frchg':
            # swap FR <-> XF banks (and toggle FPSCR.FR)
            self.emit("{ float _t; for(int _i=0;_i<16;_i++){ _t=c->fr[_i]; c->fr[_i]=c->xf[_i]; c->xf[_i]=_t; } c->fpscr ^= 0x00200000u; }")
        elif m=='fschg':
            # toggle FPSCR.SZ (single/pair transfer size). We model SZ for fmov pair mode.
            self.emit("c->fpscr ^= 0x00100000u;")
        elif m=='fsca':
            # fsca FPUL, DRn : fr[n]=sin(2pi*fpul/65536), fr[n+1]=cos(...)
            # flycast sh4_fpu.cpp: angle = (fpul & 0xFFFF) / 65536 * 2pi (single precision)
            n=int(a[1][2:]) if a[1].startswith('fr') else int(a[1][2:])
            self.emit("{")
            self.emit("  float _ang = (float)( (double)(c->fpul & 0xFFFFu) * (3.14159265358979323846/32768.0) );")
            self.emit(f"  {FR('fr'+str(n))}   = sinf(_ang);")
            self.emit(f"  {FR('fr'+str(n+1))} = cosf(_ang);")
            self.emit("}")
        elif m=='fcnvsd' or m=='fcnvds':
            # single<->double convert (1 isolated site per render scope). Model as no-op
            # on the single value since the tree is single-precision throughout.
            self.emit("; /* fcnvsd/ds: single-precision scope, modeled identity */")
        # ---------------- shifts (submit PVR control-word assembly) ----------
        elif m=='shll16':
            self.emit(f"{R(a[0])} <<= 16;")
        elif m=='shll8':
            self.emit(f"{R(a[0])} <<= 8;")
        elif m=='shlr16':
            self.emit(f"{R(a[0])} >>= 16;")
        elif m=='shlr8':
            self.emit(f"{R(a[0])} >>= 8;")
        elif m=='shlr2':
            self.emit(f"{R(a[0])} >>= 2;")
        elif m=='shlr':
            self.emit(f"{R(a[0])} >>= 1;")
        elif m=='shar':
            # arithmetic right shift by 1; bit0 -> T
            self.emit(f"c->sr_t = ({R(a[0])} & 1u); {R(a[0])} = (u32)((s32){R(a[0])} >> 1);")
        elif m=='shad':
            # shad Rm,Rn : if Rm>=0 logical-left by Rm; else arithmetic-right by -Rm
            self.emit(f"{{ s32 _s=(s32){R(a[0])}; if(_s>=0) {R(a[1])} <<= (_s&0x1F); else {{ int _n=((~_s)&0x1F)+1; {R(a[1])} = (u32)((s32){R(a[1])} >> _n); }} }}")
        elif m=='xor':
            if is_reg(a[0]): self.emit(f"{R(a[1])} ^= {R(a[0])};")
            else:
                dst=a[1] if len(a)>1 and is_reg(a[1]) else 'r0'
                self.emit(f"{R(dst)} ^= 0x{imm(a[0]):x}u;")
        elif m=='not':
            self.emit(f"{R(a[1])} = ~{R(a[0])};")
        elif m=='swap.w':
            self.emit(f"{R(a[1])} = (({R(a[0])} << 16) | ({R(a[0])} >> 16));")
        elif m=='swap.b':
            self.emit(f"{R(a[1])} = ({R(a[0])} & 0xFFFF0000u) | (({R(a[0])} & 0xFFu) << 8) | (({R(a[0])} >> 8) & 0xFFu);")
        elif m in ('ocbi','ocbp','ocbwb'):
            self.emit(f"; /* {m} @rN: cache op, no-op in flat model */")
        elif m=='pref':
            self.emit("; /* pref @rN: cache prefetch, no-op in flat model */")
        elif m=='cmp/hs':
            self.emit(f"c->sr_t = ({R(a[1])} >= {R(a[0])});")  # unsigned
        elif m=='cmp/hi':
            self.emit(f"c->sr_t = ({R(a[1])} > {R(a[0])});")   # unsigned
        # ---------------- nops/branches handled by control layer ------------
        elif m in ('nop',):
            self.emit(";")
        else:
            raise NotImplementedError(f"opcode not lifted: {m}  ({raw})")

    def _addr(self, mem):
        """Decode an SH4 memory operand string into (C-address-expr, postinc, predec, basereg)."""
        mem=mem.strip()
        mp=re.fullmatch(r'@\(([^,]+),(r\d+|PC|pc)\)', mem)
        if mp:
            d, base = mp.group(1), mp.group(2)
            if base.lower()=='pc':
                # pool load handled by caller (constant)
                return ('POOL', d, None)
            if is_reg(d):
                # R0-indexed addressing: @(r0,rN)
                return (f"({R(base)} + {R(d)})", None, base)
            # displacement immediate
            dv = imm(d)
            return (f"({R(base)} + 0x{dv:x}u)", None, base)
        mp=re.fullmatch(r'@(r\d+)\+', mem)
        if mp: return (R(mp.group(1)), '+', mp.group(1))
        mp=re.fullmatch(r'@-(r\d+)', mem)
        if mp: return (R(mp.group(1)), '-', mp.group(1))
        mp=re.fullmatch(r'@(r\d+)', mem)
        if mp: return (R(mp.group(1)), None, mp.group(1))
        raise NotImplementedError(f"addr {mem}")

    def _mem(self, ins, sz):
        m=ins.mnem; a=ins.args
        src,dst=a[0],a[1]
        szc={'b':'8','w':'16','l':'32'}[sz]
        # pool load: mov.w/mov.l @(loc_X,PC),Rn -> constant
        mp=re.fullmatch(r'@\(([^,]+),(PC|pc)\)', src) if not is_reg(src) else None
        if mp:
            locref=mp.group(1)
            key=locref.lower()
            v=self.data.get(key)
            if v is not None and not v.lower().startswith('0x'):
                # bank ref (e.g. bank11.loc_8c11e460): a code pointer. Tag the reg
                # with a synthetic LEAFTAG so a later `jsr @rN` resolves it.
                tag=self.leaf_tag(v)
                self.emit(f"{R(dst)} = 0x{tag:08x}u; /* leafptr {v} */")
                return
            val=self.pool_const(locref)
            if sz=='w': val = val & 0xFFFF
            self.emit(f"{R(dst)} = 0x{val:x}u; /* pool {locref} */")
            return
        # load: src is memory, dst is reg
        if not is_reg(src):
            addr,mode,base=self._addr(src)
            ldr={'8':'r8s','16':'r16s','32':'r32'}[szc]
            if mode=='+':
                self.emit(f"{{ u32 _a={addr}; {R(base)}+={1 if sz=='b' else 2 if sz=='w' else 4}; {R(dst)} = {ldr}(c,_a); }}")
            else:
                self.emit(f"{R(dst)} = {ldr}(c, {addr});")
            return
        # store: src is reg, dst is memory
        if not is_reg(dst):
            addr,mode,base=self._addr(dst)
            wr={'8':'w8','16':'w16','32':'w32'}[szc]
            if mode=='-':
                dec={'8':1,'16':2,'32':4}[szc]
                self.emit(f"{R(base)}-={dec}; {wr}(c, {R(base)}, {R(src)});")
            else:
                self.emit(f"{wr}(c, {addr}, {R(src)});")
            return
        raise NotImplementedError(ins.raw)

    def _fmov(self, ins):
        # SZ-aware fmov/fmov.s (source-verified vs flycast sh4_fpu.cpp:137-313). Under
        # FPSCR.SZ=1 (set by fschg), transfers are 8-byte register PAIRS; the raw float
        # field's LOW BIT selects the bank (0->fr/DR, 1->xf/XD), upper 3 bits = pair index.
        a=ins.args; src,dst=a[0],a[1]
        SZ="(c->fpscr & 0x00100000u)"
        rawn=lambda f:int(f[2:])
        bank=lambda r:('xf' if (r & 1) else 'fr', r & 0xE)   # (bankname, pair base index)
        # fmov FRm,FRn  (reg-reg)
        if is_freg(src) and is_freg(dst):
            rs,rd=rawn(src),rawn(dst); sb,sp=bank(rs); db,dp=bank(rd)
            self.emit(f"if ({SZ}) {{ c->{db}[{dp}]=c->{sb}[{sp}]; c->{db}[{dp}+1]=c->{sb}[{sp}+1]; }}"
                      f" else {{ c->fr[{rd}]=c->fr[{rs}]; }}")
            return
        # load: @Rm / @Rm+ / @(R0,Rm) / @r0(pool) -> FRn
        if is_freg(dst) and not is_freg(src):
            rd=rawn(dst); db,dp=bank(rd)
            addr,mode,base=self._addr_or_pool(src)
            if addr=='POOL_R0':
                self.emit(f"{{ u32 _b=c->_pool; c->fr[{rd}] = *(float*)&_b; }}")
                return
            inc8=f" {R(base)}+=8;" if mode=='+' else ""
            inc4=f" {R(base)}+=4;" if mode=='+' else ""
            self.emit(
                f"if ({SZ}) {{ u32 _a={addr};{inc8} u32 _w0=r32(c,_a), _w1=r32(c,_a+4);"
                f" c->{db}[{dp}]=*(float*)&_w0; c->{db}[{dp}+1]=*(float*)&_w1; }}"
                f" else {{ u32 _a={addr};{inc4} u32 _w=r32(c,_a); c->fr[{rd}]=*(float*)&_w; }}")
            return
        # store: FRm -> @Rn / @-Rn / @(R0,Rn)   (float reg is M; bank from M's low bit)
        if is_freg(src) and not is_freg(dst):
            rs=rawn(src); sb,sp=bank(rs)
            addr,mode,base=self._addr_or_pool(dst)
            if mode=='-':
                self.emit(
                    f"if ({SZ}) {{ {R(base)}-=8; u32 _a={R(base)};"
                    f" float _f0=c->{sb}[{sp}], _f1=c->{sb}[{sp}+1];"
                    f" w32(c,_a,*(u32*)&_f0); w32(c,_a+4,*(u32*)&_f1); }}"
                    f" else {{ {R(base)}-=4; float _f=c->fr[{rs}]; w32(c,{R(base)},*(u32*)&_f); }}")
            else:
                self.emit(
                    f"if ({SZ}) {{ u32 _a={addr};"
                    f" float _f0=c->{sb}[{sp}], _f1=c->{sb}[{sp}+1];"
                    f" w32(c,_a,*(u32*)&_f0); w32(c,_a+4,*(u32*)&_f1); }}"
                    f" else {{ float _f=c->fr[{rs}]; w32(c,{addr},*(u32*)&_f); }}")
            return
        raise NotImplementedError(ins.raw)

    def _addr_or_pool(self, mem):
        mem=mem.strip()
        if mem=='@r0':
            return ('POOL_R0', None, None)  # special: after mova, r0 is pool
        return self._addr(mem)
