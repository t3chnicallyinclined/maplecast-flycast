USE NS re DB kb;

-- ============================================================================
-- 2026-09-02 — node+0x104 ROTATION path of BOTH body walkers, fully specified
-- from the ROM (bank03 loc_8c0344d4 / loc_8c0348c8, bank12 loc_8C1244B0 ->
-- loc_8C124AB0, bank11 loc_8C11E2E0 / loc_8C11E860, bank13 loc_8c13F548).
-- Trigger: Steam D3D capture of Storm Lightning Storm — bolt nodes with
-- (DC +0x104) bit15 set were absent from the axis-aligned sprite stream.
-- ============================================================================

UPSERT source:marv_rotation_path_disasm SET kind='disassembly',
  ref='_marv_re/build/bank03.asm loc_8c0344d4 (10218-10778; rotation branch loc_8c03481c 10701-10737; +0x104 gate 10665-10672), loc_8c0348c8 (10800-11245; rotation branch loc_8c034b66 11154-11185), loc_8c03093c (1349-1352 writes +0x104) + loc_8c030af8 (1605-1609 writes +0x104); bank12.asm loc_8C1244B0 (9781-10118, sin/cos calls 9924-9932, corner call 9944) + loc_8C124AB0 (10647-10700 corner builder); bank11.asm loc_8C11E2E0 (34418 cos = sin(0x4000-a)) + loc_8C11E860 (35388 sin, u16 angle, 2pi/65536, series); bank13.asm loc_8c13F548 (13947 unit-corner table {1,1},{3,1},{3,3},{1,3}); bank04.asm loc_8c044f12 (11793 allocator: memclr loc_8C129728 of 0x1D0 bytes -> +0x48==0 at alloc)',
  note='Read 2026-09-02 for the Storm-bolt rotation question. All PCs are label==PC in marvelous2.';

UPSERT field:node_0x104 SET name='render_angle', offset='0x104', width='u32 (trig consumes low u16)',
  semantics='RENDER-PHASE COPY of node+0x48 (the logical angle). 0 => axis-aligned fast path; ANY nonzero (32-bit tst) => rotation path. Angle unit: u16, 0x10000 = 360deg (0x4000 = 90deg); sign: positive = counter-clockwise on the y-down screen (basis X -> (cos,-sin)). Negated (32-bit neg, then masked u16 by the trig leaf) when facing@+0x110 != 0.',
  writers='loc_8c03093c bank03:1349-1352 (+0x104 = *(u32)(node+0x48)) and loc_8c030af8 bank03:1605-1609 (same copy for CAT1-4 satellites) — every render frame. No other #data 0x0104 site in any bank writes it.',
  status='confirmed';

UPSERT field:node_0x48 SET name='logical_angle', offset='0x48', width='u32 (u16 angle, s32-negative values legal: low 16 bits are used)',
  semantics='Per-object logical rotation angle, source of node+0x104. Zeroed at alloc (loc_8c044f12 -> memclr loc_8C129728 0x1D0 bytes). Written by per-object behaviour code with literal angles or tables, e.g. bank02 loc_8c021d6a (=0x8000, HUD list-0x0B variant 7), loc_8c021dee (=0x0AAB=15deg), bank07 loc_8C07EC62 (=0x2000), loc_8C07EC74 (=0xE000), bank07 loc_8C07ED98/EDA2 (+= / -= r5 per frame = spin), bank0d loc_8C0D124A (=0xE000), bank0d 4827 (=0xF000), bank08 1643-1648 (table[(b-0x26)*4]), bank08 21006 (table 0x8C15454c[b*4]), bank0b loc_8C0BFF10 (table 0x8C15B2Fc[u16]), bank0c loc_8c0cbb76 28563 (child inherits parent +0x48 then sin/cos-offsets its position).',
  status='confirmed';

UPSERT field:node_0x108 SET name='render_intensity_f32', offset='0x108', width='f32',
  semantics='NOT an angle. Float initialised to 1.0f by bank04 spawners (loc_8c049b02 23640 fldi1 -> +0x108, and 7 siblings 25533/26351/26815/26976/27210/27511), tweened by +=/-= const in bank03 loc_8c03fa78..loc_8c040170 family, passed to the submit as struct+0x2C and compared vs 1.0 (loc_8C124534) -> feeds the vertex-colour pack (loc_8c124b40/loc_8c124d80). Irrelevant to geometry.',
  status='confirmed', confidence='medium';

UPSERT routine:loc_8c11e2e0 SET label='cos_u16', pc='0x8C11E2E0', bank='bank11', line=34418,
  note='cos(a): a&=0xFFFF; if a>0x8000 a=0x10000-a; tail-call loc_8C11E860(0x4000-a). Pool: 0x8000@E3C4, 0x10000@E3C8, 0x4000@E3C0.';
UPSERT routine:loc_8c11e860 SET label='sin_u16', pc='0x8C11E860', bank='bank11', line=35388,
  note='sin(a): a&=0xFFFF; rad=a*6.2831855/65536; half-angle range-reduce by pi/2 (n=trunc(rad/pi+0.5)); tan continued fraction 11,9,7,5,3; sin=2t/(1+t^2); negate if n odd. Pool E8DC=2pi, E8E0=65536.0, E8E4=pi/2, E8E8=0.5. Also the HUD breathing-wave fn (re_kb/57).';
UPSERT routine:loc_8c124ab0 SET label='submit_corner_builder', pc='0x8C124AB0', bank='bank12', line=10647,
  note='In: fr4=hw fr5=hh fr6=sin fr7=cos r4=struct r5=outX[4] r6=outY[4]. Copies unit table 0x8C13F548 (8 floats {1,1},{3,1},{3,3},{1,3}) to stack; sel xs=flags&3, ys=(flags>>2)&3 (both 1.0 if flags&0xF==0); per corner ex=tbl.x-xs, ey=tbl.y-ys (each 0 or +-2); X_i = X + ex*hw*cos + ey*hh*sin ; Y_i = Y - ex*hw*sin + ey*hh*cos. Pivot = struct X/Y (the walker per-tile anchor), NOT the part centre.';

UPSERT finding:rotation_path_node104 SET
  statement='CONFIRMED 2026-09-02 (ROM read, no capture). BOTH walkers rotate with ONE angle A = (facing ? -node[+0x104] : node[+0x104]) (u16 turns, 0x10000=360deg, CCW-positive on the y-down screen). +0x104 is the per-frame render copy of node+0x48 (loc_8c03093c 1349-1352 / loc_8c030af8 1605-1609). GATE = 32-bit nonzero test of +0x104 (loc_8c0344d4 10665-10672; loc_8c0348c8 10824-10828), NOT bit15 — a bit15 correlation means the stored angle is >=0x8000 (e.g. 0x8000=180deg) or an s32-negative angle (0xFFFFxxxx). TWO STAGES, same A: (1) WALKER pen rotation about the hotspot pivot P: when +0x104!=0 the walker origin is P = (floor(node+0xE0) + scaleX*(facing?-hotX:hotX), floor(node+0xE4) + scaleY*hotY) with hotX=(s16)node[+0x134], hotY=(s16)node[+0x136] (loc_8c03453a / loc_8c03491c), the pen starts at (-(+-hotX), -hotY), and each tile anchor = P + R(A).(px*scaleX, py*scaleY) with R(A).(x,y) = (x*cosA + y*sinA, -x*sinA + y*cosA) (loc_8c03481c 10701-10737; loc_8c034b66 11154-11185). When +0x104==0 the origin is (floor(E0),floor(E4)) with NO hotspot term. (2) SUBMIT quad rotation about that tile anchor by the SAME A (struct+0x28): loc_8C1244B0 computes sin/cos once (9924-9932: sin via loc_8C11E860, cos via loc_8C11E2E0), hw = struct+0x10 * rectab[+0x18] * 0.5, hh = struct+0x14 * rectab[+0x1C] * 0.5 (rectab = *(0x8C2DAD4c)+idx*32, idx=u16 idxtab[*(0x8C2DAD3c)+alloc*2]), then loc_8C124AB0 builds the 4 corners: corner_i = anchor + ex_i*hw*(cosA,-sinA) + ey_i*hh*(sinA,cosA), ex_i = {1,3,3,1}[i]-xs, ey_i = {1,1,3,3}[i]-ys, xs = 1 (facing 0) or 3 (facing 1), ys = 3 (body walker, no 0x4000) / 1 (body walker with 0x4000; scale walker always). So A/B/C/D = TL,TR,BR,BL of the un-rotated tile, W=2hw, H=2hh, rotated rigidly about the anchor corner. After the corners the loop loc_8C124620 scales X by *(0x8C2DAD50)*640.0 and Y by *(0x8C2DAD54)*480.0 (=1.0 at the standard viewport, INFERRED). Primitive is IDENTICAL on both paths: one PVR ParaType-5 sprite param (loc_8c124856+: A(x,y,z) B(x,y,z) C(x,y,z) D(x,y) + packed u16 UVs), z = 1/(node+0xE8 + 0.001*(k+1)) for part k (the walker adds 0.001 before each submit, loc_8c034864 / loc_8c034ba4), so W/z bookkeeping does NOT differ. The rotation path emits the same submit call from the same PC 0x8C034864 (ASMTRACE sees it; screenX/Y logged there are the ROTATED tile anchor). Fields the rotated reproduction needs: +0x104 (or +0x48), +0x110 facing, +0xE0/+0xE4, +0xEC/+0xF0, +0x134/+0x136 hotspot, +0xE8 depth, the GFX2 records (dx,dy,flags,sel), part W/H (from the GFX1 descriptor for the scale walker: W=desc[0]/desc[2]*scaleX*texW, UV u1=desc[0]/desc[2]).',
  status='confirmed', confidence='high', date='2026-09-02';

RELATE finding:rotation_path_node104->cites->source:marv_rotation_path_disasm;
RELATE finding:rotation_path_node104->about->routine:loc_8c0344d4;
RELATE finding:rotation_path_node104->about->routine:loc_8c0348c8;
RELATE finding:rotation_path_node104->about->routine:loc_8c1244b0;
RELATE finding:rotation_path_node104->about->routine:loc_8c124ab0;
RELATE finding:rotation_path_node104->about->field:node_0x104;
RELATE finding:rotation_path_node104->about->field:node_0x48;

UPSERT finding:storm_bolt_angle_writer SET
  statement='OPEN 2026-09-02. Storm Lightning Storm bolts (spawner bank09 loc_8c09beec, cb loc_8c09bf9a, states loc_8C09BFBC/loc_8c09C18C/loc_8c09C1CE, table 0x8C156064) contain NO write to node+0x48 (grep of every 0x48 form in bank09 28240-28700 and in S_PL2A.asm = none); the node is memclr-ed at alloc, so the bolt angle seen on Steam (+0x104/+0x148 bit15) is written by something outside that family — candidates: a bank15/bank0x per-frame effect handler keyed by node+0x26=0x2A02, or the anim-tick side (loc_8c034dee) via a cell field. NEXT STEP (derive-before-capture exhausted): a write-watch on node+0x48 of one bolt node (Oracle probe on the pool node, or a Steam hardware write-breakpoint on the DC+0x48-equivalent) — the faulting PC names the writer. Also log the FULL u32 of +0x104: 0x00008000 => 180deg (still axis-aligned but reflected through the anchor), 0xFFFFxxxx => negative angle (genuinely tilted).',
  status='open', confidence='medium', date='2026-09-02';

RELATE finding:storm_bolt_angle_writer->cites->source:marv_rotation_path_disasm;
RELATE finding:storm_bolt_angle_writer->about->routine:loc_8c09beec;
RELATE finding:storm_bolt_angle_writer->about->field:node_0x48;
