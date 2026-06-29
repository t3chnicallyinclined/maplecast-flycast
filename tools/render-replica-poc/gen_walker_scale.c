#include "sh4ctx.h"
void leaf_e460(Sh4Ctx*);
void leaf_e2e0(Sh4Ctx*);
void leaf_e860(Sh4Ctx*);
void submit_1244b0(Sh4Ctx*);

/* AUTO-GENERATED from bank03.asm loc_8c0348c8 (do not edit) */
void walker_0348c8(Sh4Ctx *c){
loc_8c0348c8:; /* bb */
    /* mov.l r14,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[14]);
    /* mov.l r13,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[13]);
    /* mov.l r12,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[12]);
    /* mov.l r11,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[11]);
    /* mov.l r10,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[10]);
    /* mov.l r9,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[9]);
    /* mov.l r8,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->r[8]);
    /* fmov fr15,@-r15 */
    c->r[15]-=4; { float _f=c->fr[15]; w32(c,c->r[15], *(u32*)&_f); }
    /* fmov fr14,@-r15 */
    c->r[15]-=4; { float _f=c->fr[14]; w32(c,c->r[15], *(u32*)&_f); }
    /* fmov fr13,@-r15 */
    c->r[15]-=4; { float _f=c->fr[13]; w32(c,c->r[15], *(u32*)&_f); }
    /* fmov fr12,@-r15 */
    c->r[15]-=4; { float _f=c->fr[12]; w32(c,c->r[15], *(u32*)&_f); }
    /* sts.l pr,@-r15 */
    c->r[15]-=4; w32(c, c->r[15], c->pr);
    /* add 0xA4,r15 */
    c->r[15] += (u32)(s32)(-92);
    /* mov.w @(loc_8c03492a,PC),r0 */
    c->r[0] = 0x160u; /* pool loc_8c03492a */
    /* mov r4,r14 */
    c->r[14] = c->r[4];
    /* mov.w @(loc_8c03492c,PC),r3 */
    c->r[3] = 0x7fffu; /* pool loc_8c03492c */
    /* mov.l @(r0,r14),r4 */
    c->r[4] = r32(c, (c->r[14] + c->r[0]));
    /* add 0xE4,r0 */
    c->r[0] += (u32)(s32)(-28);
    /* mov.l @(r0,r14),r0 */
    c->r[0] = r32(c, (c->r[14] + c->r[0]));
    /* mov.l @(loc_8c034948,PC),r11 */
    c->r[11] = 0x1ea00003u; /* leafptr bank11.loc_8c11e460 */
    /* and r3,r0 */
    c->r[0] &= c->r[3];
    /* shll2 r0 */
    c->r[0] <<= 2;
    /* mov.l @(r0,r4),r13 */
    c->r[13] = r32(c, (c->r[4] + c->r[0]));
    /* mov.w @(loc_8c03492e,PC),r0 */
    c->r[0] = 0x104u; /* pool loc_8c03492e */
    /* add r4,r13 */
    c->r[13] += c->r[4];
    /* mov.l @(r0,r14),r2 */
    c->r[2] = r32(c, (c->r[14] + c->r[0]));
    /* mov.w @r13+,r9 */
    { u32 _a=c->r[13]; c->r[13]+=2; c->r[9] = r16s(c,_a); }
    /* tst r2,r2 */
    c->sr_t = ((c->r[2] & c->r[2])==0);
    /* extu.w r9,r9 */
    c->r[9] = c->r[9] & 0xFFFFu;
    /* mov 0x00,r8 */
    c->r[8] = (u32)(s32)(0);
    if(!c->sr_t) goto loc_8c03491c;
    /* mov.w @(loc_8c034930,PC),r0 */
    c->r[0] = 0xe0u; /* pool loc_8c034930 */
    /* fmov @(r0,r14),fr4 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[4] = *(float*)&_w; }
    if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00001u){ leaf_e860(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* mov.w @(loc_8c034932,PC),r0 */
    c->r[0] = 0xe4u; /* pool loc_8c034932 */
    /* fmov fr0,fr15 */
    c->fr[15] = c->fr[0];
    /* fmov @(r0,r14),fr4 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[4] = *(float*)&_w; }
    if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00001u){ leaf_e860(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* mov r8,r12 */
    c->r[12] = c->r[8];
    /* fmov fr0,fr12 */
    c->fr[12] = c->fr[0];
    /* mov r8,r11 */
    c->r[11] = c->r[8];
    goto loc_8c034986;
loc_8c03491c:; /* bb */
    /* mov.w @(loc_8c034934,PC),r0 */
    c->r[0] = 0x110u; /* pool loc_8c034934 */
    /* mov.l @(r0,r14),r2 */
    c->r[2] = r32(c, (c->r[14] + c->r[0]));
    /* tst r2,r2 */
    c->sr_t = ((c->r[2] & c->r[2])==0);
    if(!c->sr_t) goto loc_8c03494c;
    /* mov.w @(loc_8c034936,PC),r0 */
    c->r[0] = 0x134u; /* pool loc_8c034936 */
    /* mov.w @(r0,r14),r12 */
    c->r[12] = r16s(c, (c->r[14] + c->r[0]));
    goto loc_8c034952;
loc_8c03494c:; /* bb */
    /* mov.w @(loc_8c034aa8,PC),r0 */
    c->r[0] = 0x134u; /* pool loc_8c034aa8 */
    /* mov.w @(r0,r14),r12 */
    c->r[12] = r16s(c, (c->r[14] + c->r[0]));
    /* neg r12,r12 */
    c->r[12] = (u32)(0 - (s32)c->r[12]);
loc_8c034952:; /* bb */
    /* mov.w @(loc_8c034aaa,PC),r0 */
    c->r[0] = 0x136u; /* pool loc_8c034aaa */
    /* mov.w @(r0,r14),r10 */
    c->r[10] = r16s(c, (c->r[14] + c->r[0]));
    /* add 0xAA,r0 */
    c->r[0] += (u32)(s32)(-86);
    /* fmov @(r0,r14),fr4 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[4] = *(float*)&_w; }
    if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00001u){ leaf_e860(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* exts.w r12,r3 */
    c->r[3] = (u32)(s32)(s16)c->r[12];
    /* mov.w @(loc_8c034aac,PC),r0 */
    c->r[0] = 0xecu; /* pool loc_8c034aac */
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* fmov fr0,fr2 */
    c->fr[2] = c->fr[0];
    /* fmov @(r0,r14),fr0 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[0] = *(float*)&_w; }
    /* add 0xF8,r0 */
    c->r[0] += (u32)(s32)(-8);
    /* float fpul,fr3 */
    c->fr[3] = (float)(s32)c->fpul;
    /* fmac fr0,fr3,fr2 */
    c->fr[2] = fmaf(c->fr[0], c->fr[3], c->fr[2]);
    /* fmov fr2,fr15 */
    c->fr[15] = c->fr[2];
    /* fmov @(r0,r14),fr4 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[4] = *(float*)&_w; }
    if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00001u){ leaf_e860(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[11] & 0xFFF00000u)==0x1EA00000u && c->r[11]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* exts.w r10,r3 */
    c->r[3] = (u32)(s32)(s16)c->r[10];
    /* mov.w @(loc_8c034aae,PC),r0 */
    c->r[0] = 0xf0u; /* pool loc_8c034aae */
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* neg r10,r11 */
    c->r[11] = (u32)(0 - (s32)c->r[10]);
    /* fmov fr0,fr2 */
    c->fr[2] = c->fr[0];
    /* neg r12,r12 */
    c->r[12] = (u32)(0 - (s32)c->r[12]);
    /* fmov @(r0,r14),fr0 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[0] = *(float*)&_w; }
    /* float fpul,fr3 */
    c->fr[3] = (float)(s32)c->fpul;
    /* fmac fr0,fr3,fr2 */
    c->fr[2] = fmaf(c->fr[0], c->fr[3], c->fr[2]);
    /* fmov fr2,fr12 */
    c->fr[12] = c->fr[2];
loc_8c034986:; /* bb */
    /* mov.w @(loc_8c034ab0,PC),r0 */
    c->r[0] = 0xe8u; /* pool loc_8c034ab0 */
    /* mov 0xFF,r1 */
    c->r[1] = (u32)(s32)(-1);
    /* mov.l @(loc_8c034ac8,PC),r2 */
    c->r[2] = 0x8c1f9d84u; /* pool loc_8c034ac8 */
    /* mov r8,r10 */
    c->r[10] = c->r[8];
    /* fmov @(r0,r14),fr3 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov 0x18,r0 */
    c->r[0] = (u32)(s32)(24);
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov.w @(loc_8c034ab2,PC),r0 */
    c->r[0] = 0x108u; /* pool loc_8c034ab2 */
    /* fmov @(r0,r14),fr3 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov 0x38,r0 */
    c->r[0] = (u32)(s32)(56);
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x44,r0 */
    c->r[0] = (u32)(s32)(68);
    /* mov.l @r2,r3 */
    c->r[3] = r32(c, c->r[2]);
    /* mov.w @(loc_8c034ab8,PC),r2 */
    c->r[2] = 0x220u; /* pool loc_8c034ab8 */
    /* mov.l r3,@(0x3C,r15) */
    w32(c, (c->r[15] + 0x3cu), c->r[3]);
    /* mov.l r1,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[1]);
    /* mov 0x48,r0 */
    c->r[0] = (u32)(s32)(72);
    /* mov.l r8,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[8]);
    /* mov.w @(loc_8c034ab4,PC),r0 */
    c->r[0] = 0x180u; /* pool loc_8c034ab4 */
    /* mov.w @(loc_8c034ab6,PC),r3 */
    c->r[3] = 0x1ffu; /* pool loc_8c034ab6 */
    /* mov.l @(r0,r14),r5 */
    c->r[5] = r32(c, (c->r[14] + c->r[0]));
    /* add 0xC4,r0 */
    c->r[0] += (u32)(s32)(-60);
    /* mov.l @(r0,r14),r0 */
    c->r[0] = r32(c, (c->r[14] + c->r[0]));
    /* and r3,r0 */
    c->r[0] &= c->r[3];
    /* shll r0 */
    c->r[0] <<= 1;
    /* cmp/pl r9 */
    c->sr_t = ((s32)c->r[9] > 0);
    /* mov.w @(r0,r5),r4 */
    c->r[4] = r16s(c, (c->r[5] + c->r[0]));
    /* add r2,r5 */
    c->r[5] += c->r[2];
    /* extu.w r4,r4 */
    c->r[4] = c->r[4] & 0xFFFFu;
    /* add r4,r5 */
    c->r[5] += c->r[4];
    /* mov.l r5,@(0x8,r15) */
    w32(c, (c->r[15] + 0x8u), c->r[5]);
    /* mov.l r8,@r15 */
    w32(c, c->r[15], c->r[8]);
    if(c->sr_t) goto loc_8c0349cc;
    /* nop */
    ;
    goto loc_8c034bcc;
loc_8c0349cc:; /* bb */
    /* mov.l @(0x8,r15),r3 */
    c->r[3] = r32(c, (c->r[15] + 0x8u));
    /* mov.w @(loc_8c034abc,PC),r0 */
    c->r[0] = 0x1a4u; /* pool loc_8c034abc */
    /* add 0x01,r3 */
    c->r[3] += (u32)(s32)(1);
    /* mov.w @(loc_8c034aba,PC),r4 */
    c->r[4] = 0x390u; /* pool loc_8c034aba */
    /* mov.l r3,@(0x8,r15) */
    w32(c, (c->r[15] + 0x8u), c->r[3]);
    /* add 0xFF,r3 */
    c->r[3] += (u32)(s32)(-1);
    /* mov.b @r3,r2 */
    c->r[2] = r8s(c, c->r[3]);
    /* mov.b @(r0,r14),r3 */
    c->r[3] = r8s(c, (c->r[14] + c->r[0]));
    /* extu.b r2,r2 */
    c->r[2] = c->r[2] & 0xFFu;
    /* mov.w @(loc_8c034abe,PC),r0 */
    c->r[0] = 0x15cu; /* pool loc_8c034abe */
    /* extu.b r3,r3 */
    c->r[3] = c->r[3] & 0xFFu;
    /* fldi0 fr4 */
    c->fr[4] = 0.0f;
    /* shll8 r3 */
    c->r[3] <<= 8;
    /* add r3,r2 */
    c->r[2] += c->r[3];
    /* add r4,r2 */
    c->r[2] += c->r[4];
    /* mov.l r2,@(0xC,r15) */
    w32(c, (c->r[15] + 0xcu), c->r[2]);
    /* mov.l @(r0,r14),r4 */
    c->r[4] = r32(c, (c->r[14] + c->r[0]));
    /* mov.w @(0x6,r13),r0 */
    c->r[0] = r16s(c, (c->r[13] + 0x6u));
    /* extu.w r0,r0 */
    c->r[0] = c->r[0] & 0xFFFFu;
    /* shll2 r0 */
    c->r[0] <<= 2;
    /* mov.l @(r0,r4),r3 */
    c->r[3] = r32(c, (c->r[4] + c->r[0]));
    /* mov 0x24,r0 */
    c->r[0] = (u32)(s32)(36);
    /* fmov fr4,@(r0,r15) */
    { float _f=c->fr[4]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x28,r0 */
    c->r[0] = (u32)(s32)(40);
    /* add r3,r4 */
    c->r[4] += c->r[3];
    /* fmov fr4,@(r0,r15) */
    { float _f=c->fr[4]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov.b @r4,r3 */
    c->r[3] = r8s(c, c->r[4]);
    /* mov.b @(0x2,r4),r0 */
    c->r[0] = r8s(c, (c->r[4] + 0x2u));
    /* extu.b r3,r3 */
    c->r[3] = c->r[3] & 0xFFu;
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* extu.b r0,r3 */
    c->r[3] = c->r[0] & 0xFFu;
    /* float fpul,fr3 */
    c->fr[3] = (float)(s32)c->fpul;
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* mov 0x2C,r0 */
    c->r[0] = (u32)(s32)(44);
    /* float fpul,fr2 */
    c->fr[2] = (float)(s32)c->fpul;
    /* fdiv fr2,fr3 */
    c->fr[3] = c->fr[3] / c->fr[2];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov.b @(0x1,r4),r0 */
    c->r[0] = r8s(c, (c->r[4] + 0x1u));
    /* extu.b r0,r3 */
    c->r[3] = c->r[0] & 0xFFu;
    /* mov.b @(0x3,r4),r0 */
    c->r[0] = r8s(c, (c->r[4] + 0x3u));
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* extu.b r0,r3 */
    c->r[3] = c->r[0] & 0xFFu;
    /* mov 0x30,r0 */
    c->r[0] = (u32)(s32)(48);
    /* float fpul,fr3 */
    c->fr[3] = (float)(s32)c->fpul;
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* float fpul,fr2 */
    c->fr[2] = (float)(s32)c->fpul;
    /* fdiv fr2,fr3 */
    c->fr[3] = c->fr[3] / c->fr[2];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x2C,r0 */
    c->r[0] = (u32)(s32)(44);
    /* fmov @(r0,r15),fr3 */
    { u32 _w=r32(c,(c->r[15] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov.w @(loc_8c034aac,PC),r0 */
    c->r[0] = 0xecu; /* pool loc_8c034aac */
    /* fmov @(r0,r14),fr2 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[2] = *(float*)&_w; }
    /* mov 0x1C,r0 */
    c->r[0] = (u32)(s32)(28);
    /* fmul fr3,fr2 */
    c->fr[2] = c->fr[2] * c->fr[3];
    /* fmov fr2,@(r0,r15) */
    { float _f=c->fr[2]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x30,r0 */
    c->r[0] = (u32)(s32)(48);
    /* fmov @(r0,r15),fr3 */
    { u32 _w=r32(c,(c->r[15] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov.w @(loc_8c034aae,PC),r0 */
    c->r[0] = 0xf0u; /* pool loc_8c034aae */
    /* fmov @(r0,r14),fr2 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[2] = *(float*)&_w; }
    /* mov 0x20,r0 */
    c->r[0] = (u32)(s32)(32);
    /* fmul fr3,fr2 */
    c->fr[2] = c->fr[2] * c->fr[3];
    /* fmov fr2,@(r0,r15) */
    { float _f=c->fr[2]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov.w @(0x2,r13),r0 */
    c->r[0] = r16s(c, (c->r[13] + 0x2u));
    /* mov.w @r13,r8 */
    c->r[8] = r16s(c, c->r[13]);
    /* mov.w r0,@(0x4,r15) */
    w16(c, (c->r[15] + 0x4u), c->r[0]);
    /* mov r11,r7 */
    c->r[7] = c->r[11];
    /* mov.w @(0x4,r15),r0 */
    c->r[0] = r16s(c, (c->r[15] + 0x4u));
    /* mov 0x20,r5 */
    c->r[5] = (u32)(s32)(32);
    /* mov.w @(loc_8c034ac0,PC),r4 */
    c->r[4] = 0x4000u; /* pool loc_8c034ac0 */
    /* sub r0,r7 */
    c->r[7] -= c->r[0];
    /* mov.w @(loc_8c034ac2,PC),r0 */
    c->r[0] = 0x110u; /* pool loc_8c034ac2 */
    /* mov.l @(loc_8c034acc,PC),r6 */
    c->r[6] = 0x8000u; /* pool loc_8c034acc */
    /* mov.l @(r0,r14),r3 */
    c->r[3] = r32(c, (c->r[14] + c->r[0]));
    /* tst r3,r3 */
    c->sr_t = ((c->r[3] & c->r[3])==0);
    /* fldi1 fr4 */
    c->fr[4] = 1.0f;
    if(!c->sr_t) goto loc_8c034ad4;
    /* mov.w @(loc_8c034ac4,PC),r0 */
    c->r[0] = 0x104u; /* pool loc_8c034ac4 */
    /* mov r7,r11 */
    c->r[11] = c->r[7];
    /* mov.l @(loc_8c034ad0,PC),r2 */
    c->r[2] = 0x8c1f9d88u; /* pool loc_8c034ad0 */
    /* mov 0x05,r7 */
    c->r[7] = (u32)(s32)(5);
    /* mov.l @(r0,r14),r3 */
    c->r[3] = r32(c, (c->r[14] + c->r[0]));
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* mov.l r3,@(0x34,r15) */
    w32(c, (c->r[15] + 0x34u), c->r[3]);
    /* mov.l @r2,r3 */
    c->r[3] = r32(c, c->r[2]);
    /* or r7,r3 */
    c->r[3] |= c->r[7];
    /* mov.l r3,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[3]);
    /* mov.w @(0x4,r13),r0 */
    c->r[0] = r16s(c, (c->r[13] + 0x4u));
    /* extu.w r0,r0 */
    c->r[0] = c->r[0] & 0xFFFFu;
    /* tst r4,r0 */
    c->sr_t = ((c->r[4] & c->r[0])==0);
    /* sub r8,r12 */
    c->r[12] -= c->r[8];
    if(c->sr_t) goto loc_8c034a9c;
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* mov.l @(r0,r15),r1 */
    c->r[1] = r32(c, (c->r[15] + c->r[0]));
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* or r5,r1 */
    c->r[1] |= c->r[5];
    /* mov.l r1,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[1]);
    /* mov 0x30,r0 */
    c->r[0] = (u32)(s32)(48);
    /* fmov @(r0,r15),fr3 */
    { u32 _w=r32(c,(c->r[15] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov 0x28,r0 */
    c->r[0] = (u32)(s32)(40);
    /* fneg fr3 */
    c->fr[3] = -c->fr[3];
    /* fadd fr4,fr3 */
    c->fr[3] = c->fr[3] + c->fr[4];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x30,r0 */
    c->r[0] = (u32)(s32)(48);
    /* fmov fr4,@(r0,r15) */
    { float _f=c->fr[4]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
loc_8c034a9c:; /* bb */
    /* mov.w @(0x4,r13),r0 */
    c->r[0] = r16s(c, (c->r[13] + 0x4u));
    /* extu.w r0,r0 */
    c->r[0] = c->r[0] & 0xFFFFu;
    /* tst r6,r0 */
    c->sr_t = ((c->r[6] & c->r[0])==0);
    if(!c->sr_t) goto loc_8c034b16;
    /* nop */
    ;
    goto loc_8c034b32;
loc_8c034ad4:; /* bb */
    /* mov.w @(loc_8c034c18,PC),r0 */
    c->r[0] = 0x104u; /* pool loc_8c034c18 */
    /* mov r7,r11 */
    c->r[11] = c->r[7];
    /* mov 0x07,r7 */
    c->r[7] = (u32)(s32)(7);
    /* mov.l @(r0,r14),r2 */
    c->r[2] = r32(c, (c->r[14] + c->r[0]));
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* neg r2,r2 */
    c->r[2] = (u32)(0 - (s32)c->r[2]);
    /* mov.l r2,@(0x34,r15) */
    w32(c, (c->r[15] + 0x34u), c->r[2]);
    /* mov.l @(loc_8c034c20,PC),r2 */
    c->r[2] = 0x8c1f9d88u; /* pool loc_8c034c20 */
    /* mov.l @r2,r3 */
    c->r[3] = r32(c, c->r[2]);
    /* or r7,r3 */
    c->r[3] |= c->r[7];
    /* mov.l r3,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[3]);
    /* mov.w @(0x4,r13),r0 */
    c->r[0] = r16s(c, (c->r[13] + 0x4u));
    /* extu.w r0,r0 */
    c->r[0] = c->r[0] & 0xFFFFu;
    /* tst r4,r0 */
    c->sr_t = ((c->r[4] & c->r[0])==0);
    /* add r8,r12 */
    c->r[12] += c->r[8];
    if(c->sr_t) goto loc_8c034b0e;
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* mov.l @(r0,r15),r1 */
    c->r[1] = r32(c, (c->r[15] + c->r[0]));
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* or r5,r1 */
    c->r[1] |= c->r[5];
    /* mov.l r1,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[1]);
    /* mov 0x30,r0 */
    c->r[0] = (u32)(s32)(48);
    /* fmov @(r0,r15),fr3 */
    { u32 _w=r32(c,(c->r[15] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov 0x28,r0 */
    c->r[0] = (u32)(s32)(40);
    /* fneg fr3 */
    c->fr[3] = -c->fr[3];
    /* fadd fr4,fr3 */
    c->fr[3] = c->fr[3] + c->fr[4];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x30,r0 */
    c->r[0] = (u32)(s32)(48);
    /* fmov fr4,@(r0,r15) */
    { float _f=c->fr[4]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
loc_8c034b0e:; /* bb */
    /* mov.w @(0x4,r13),r0 */
    c->r[0] = r16s(c, (c->r[13] + 0x4u));
    /* extu.w r0,r0 */
    c->r[0] = c->r[0] & 0xFFFFu;
    /* tst r6,r0 */
    c->sr_t = ((c->r[6] & c->r[0])==0);
    if(!c->sr_t) goto loc_8c034b32;
loc_8c034b16:; /* bb */
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* mov.l @(r0,r15),r2 */
    c->r[2] = r32(c, (c->r[15] + c->r[0]));
    /* mov 0x10,r4 */
    c->r[4] = (u32)(s32)(16);
    /* mov 0x40,r0 */
    c->r[0] = (u32)(s32)(64);
    /* or r4,r2 */
    c->r[2] |= c->r[4];
    /* mov.l r2,@(r0,r15) */
    w32(c, (c->r[15] + c->r[0]), c->r[2]);
    /* mov 0x2C,r0 */
    c->r[0] = (u32)(s32)(44);
    /* fmov @(r0,r15),fr3 */
    { u32 _w=r32(c,(c->r[15] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov 0x24,r0 */
    c->r[0] = (u32)(s32)(36);
    /* fneg fr3 */
    c->fr[3] = -c->fr[3];
    /* fadd fr4,fr3 */
    c->fr[3] = c->fr[3] + c->fr[4];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x2C,r0 */
    c->r[0] = (u32)(s32)(44);
    /* fmov fr4,@(r0,r15) */
    { float _f=c->fr[4]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
loc_8c034b32:; /* bb */
    /* exts.w r12,r3 */
    c->r[3] = (u32)(s32)(s16)c->r[12];
    /* mov.w @(loc_8c034c1a,PC),r0 */
    c->r[0] = 0xecu; /* pool loc_8c034c1a */
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* exts.w r11,r3 */
    c->r[3] = (u32)(s32)(s16)c->r[11];
    /* fmov @(r0,r14),fr2 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[2] = *(float*)&_w; }
    /* add 0x04,r0 */
    c->r[0] += (u32)(s32)(4);
    /* mov.l @(0x34,r15),r8 */
    c->r[8] = r32(c, (c->r[15] + 0x34u));
    /* float fpul,fr3 */
    c->fr[3] = (float)(s32)c->fpul;
    /* lds r3,fpul */
    c->fpul = c->r[3];
    /* tst r8,r8 */
    c->sr_t = ((c->r[8] & c->r[8])==0);
    /* fmov fr3,fr14 */
    c->fr[14] = c->fr[3];
    /* float fpul,fr3 */
    c->fr[3] = (float)(s32)c->fpul;
    /* fmul fr2,fr14 */
    c->fr[14] = c->fr[14] * c->fr[2];
    /* fmov @(r0,r14),fr2 */
    { u32 _w=r32(c,(c->r[14] + c->r[0])); c->fr[2] = *(float*)&_w; }
    /* fmov fr3,fr13 */
    c->fr[13] = c->fr[3];
    /* fmul fr2,fr13 */
    c->fr[13] = c->fr[13] * c->fr[2];
    if(!c->sr_t) goto loc_8c034b66;
    /* fmov fr15,fr3 */
    c->fr[3] = c->fr[15];
    /* fadd fr14,fr3 */
    c->fr[3] = c->fr[3] + c->fr[14];
    /* mov 0x10,r0 */
    c->r[0] = (u32)(s32)(16);
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov 0x14,r0 */
    c->r[0] = (u32)(s32)(20);
    /* fmov fr12,fr3 */
    c->fr[3] = c->fr[12];
    /* fadd fr13,fr3 */
    c->fr[3] = c->fr[3] + c->fr[13];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    goto loc_8c034ba4;
loc_8c034b66:; /* bb */
    /* mov.l @(loc_8c034c24,PC),r2 */
    c->r[2] = 0x1ea00000u; /* leafptr bank11.loc_8c11e2e0 */
    /* mov r8,r4 */
    c->r[4] = c->r[8];
    if((c->r[2] & 0xFFF00000u)==0x1EA00000u && c->r[2]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[2] & 0xFFF00000u)==0x1EA00000u && c->r[2]==0x1ea00001u){ leaf_e860(c); } else if((c->r[2] & 0xFFF00000u)==0x1EA00000u && c->r[2]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[2] & 0xFFF00000u)==0x1EA00000u && c->r[2]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* fmov fr15,fr3 */
    c->fr[3] = c->fr[15];
    /* fmac fr0,fr14,fr3 */
    c->fr[3] = fmaf(c->fr[0], c->fr[14], c->fr[3]);
    /* mov.l @(loc_8c034c28,PC),r3 */
    c->r[3] = 0x1ea00001u; /* leafptr bank11.loc_8c11e860 */
    /* fmov fr3,@-r15 */
    c->r[15]-=4; { float _f=c->fr[3]; w32(c,c->r[15], *(u32*)&_f); }
    /* mov.l @(0x38,r15),r4 */
    c->r[4] = r32(c, (c->r[15] + 0x38u));
    if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00001u){ leaf_e860(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* fmov @r15+,fr2 */
    { u32 _a=c->r[15]; c->r[15]+=4; u32 _w=r32(c,_a); c->fr[2] = *(float*)&_w; }
    /* mov 0x10,r0 */
    c->r[0] = (u32)(s32)(16);
    /* fmov fr0,fr3 */
    c->fr[3] = c->fr[0];
    /* fmov fr13,fr0 */
    c->fr[0] = c->fr[13];
    /* fmac fr0,fr3,fr2 */
    c->fr[2] = fmaf(c->fr[0], c->fr[3], c->fr[2]);
    /* mov.l @(loc_8c034c28,PC),r3 */
    c->r[3] = 0x1ea00001u; /* leafptr bank11.loc_8c11e860 */
    /* fmov fr2,@(r0,r15) */
    { float _f=c->fr[2]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* mov.l @(0x34,r15),r4 */
    c->r[4] = r32(c, (c->r[15] + 0x34u));
    if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00001u){ leaf_e860(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* fmul fr0,fr14 */
    c->fr[14] = c->fr[14] * c->fr[0];
    /* fmov fr12,fr3 */
    c->fr[3] = c->fr[12];
    /* mov.l @(loc_8c034c24,PC),r3 */
    c->r[3] = 0x1ea00000u; /* leafptr bank11.loc_8c11e2e0 */
    /* fsub fr14,fr3 */
    c->fr[3] = c->fr[3] - c->fr[14];
    /* fmov fr3,@-r15 */
    c->r[15]-=4; { float _f=c->fr[3]; w32(c,c->r[15], *(u32*)&_f); }
    /* mov.l @(0x38,r15),r4 */
    c->r[4] = r32(c, (c->r[15] + 0x38u));
    if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00001u){ leaf_e860(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[3] & 0xFFF00000u)==0x1EA00000u && c->r[3]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* fmov @r15+,fr2 */
    { u32 _a=c->r[15]; c->r[15]+=4; u32 _w=r32(c,_a); c->fr[2] = *(float*)&_w; }
    /* mov 0x14,r0 */
    c->r[0] = (u32)(s32)(20);
    /* fmov fr0,fr3 */
    c->fr[3] = c->fr[0];
    /* fmov fr13,fr0 */
    c->fr[0] = c->fr[13];
    /* fmac fr0,fr3,fr2 */
    c->fr[2] = fmaf(c->fr[0], c->fr[3], c->fr[2]);
    /* fmov fr2,@(r0,r15) */
    { float _f=c->fr[2]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
loc_8c034ba4:; /* bb */
    /* mova @(loc_8c034c2c,PC),r0 */
    c->r[0] = 0xF0000000u; c->_pool = 0x3a83126fu; /* mova loc_8c034c2c */
    /* mov.l @(loc_8c034c30,PC),r8 */
    c->r[8] = 0x1ea00002u; /* leafptr bank12.loc_8c1244b0 */
    /* fmov @r0,fr4 */
    { u32 _b=c->_pool; c->fr[4] = *(float*)&_b; }
    /* mov 0x18,r0 */
    c->r[0] = (u32)(s32)(24);
    /* fmov @(r0,r15),fr3 */
    { u32 _w=r32(c,(c->r[15] + c->r[0])); c->fr[3] = *(float*)&_w; }
    /* mov 0x18,r0 */
    c->r[0] = (u32)(s32)(24);
    /* mov r15,r4 */
    c->r[4] = c->r[15];
    /* fadd fr4,fr3 */
    c->fr[3] = c->fr[3] + c->fr[4];
    /* fmov fr3,@(r0,r15) */
    { float _f=c->fr[3]; w32(c,(c->r[15] + c->r[0]), *(u32*)&_f); }
    /* add 0x0C,r4 */
    c->r[4] += (u32)(s32)(12);
    if((c->r[8] & 0xFFF00000u)==0x1EA00000u && c->r[8]==0x1ea00000u){ leaf_e2e0(c); } else if((c->r[8] & 0xFFF00000u)==0x1EA00000u && c->r[8]==0x1ea00001u){ leaf_e860(c); } else if((c->r[8] & 0xFFF00000u)==0x1EA00000u && c->r[8]==0x1ea00002u){ submit_1244b0(c); } else if((c->r[8] & 0xFFF00000u)==0x1EA00000u && c->r[8]==0x1ea00003u){ leaf_e460(c); } else { /* unresolved jsr */ }
    /* mov.l @r15,r3 */
    c->r[3] = r32(c, c->r[15]);
    /* add 0x01,r10 */
    c->r[10] += (u32)(s32)(1);
    /* cmp/gt r10,r9 */
    c->sr_t = ((s32)c->r[9] > (s32)c->r[10]);
    /* add 0x01,r3 */
    c->r[3] += (u32)(s32)(1);
    /* mov.l r3,@r15 */
    w32(c, c->r[15], c->r[3]);
    /* add 0x08,r13 */
    c->r[13] += (u32)(s32)(8);
    if(!c->sr_t) goto loc_8c034bcc;
    /* nop */
    ;
    goto loc_8c0349cc;
loc_8c034bcc:; /* bb */
    /* mov.l @r15,r0 */
    c->r[0] = r32(c, c->r[15]);
    /* add 0x5C,r15 */
    c->r[15] += (u32)(s32)(92);
    /* lds.l @r15+,pr */
    c->pr = r32(c, c->r[15]); c->r[15]+=4;
    /* fmov @r15+,fr12 */
    { u32 _a=c->r[15]; c->r[15]+=4; u32 _w=r32(c,_a); c->fr[12] = *(float*)&_w; }
    /* fmov @r15+,fr13 */
    { u32 _a=c->r[15]; c->r[15]+=4; u32 _w=r32(c,_a); c->fr[13] = *(float*)&_w; }
    /* fmov @r15+,fr14 */
    { u32 _a=c->r[15]; c->r[15]+=4; u32 _w=r32(c,_a); c->fr[14] = *(float*)&_w; }
    /* fmov @r15+,fr15 */
    { u32 _a=c->r[15]; c->r[15]+=4; u32 _w=r32(c,_a); c->fr[15] = *(float*)&_w; }
    /* mov.l @r15+,r8 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[8] = r32(c,_a); }
    /* mov.l @r15+,r9 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[9] = r32(c,_a); }
    /* mov.l @r15+,r10 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[10] = r32(c,_a); }
    /* mov.l @r15+,r11 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[11] = r32(c,_a); }
    /* mov.l @r15+,r12 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[12] = r32(c,_a); }
    /* mov.l @r15+,r13 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[13] = r32(c,_a); }
    /* mov.l @r15+,r14 */
    { u32 _a=c->r[15]; c->r[15]+=4; c->r[14] = r32(c,_a); }
    return;
}
