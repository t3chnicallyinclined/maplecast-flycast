-- 172_sprite_tile_pipeline.surql -- the chain from the engine's own draw list to a tile's pixels,
-- closed 2026-09-05. Everything here was measured in a running sg_runner or read out of
-- mvc_dump.bin; nothing is inferred from the offline atlas.
--
-- What was ALREADY known and is NOT repeated here:
--   168_ctx_display_list          the ctx+0x30 display list layout
--   171_sprite_quad_texture_identity  the 0xC quad record + the slot formula
--   TEXTURE-BANKS-GHIDRA.md:14,44 the 16-byte TEX descriptor and the slot-entry layout
-- What is NEW: the DESCRIPTOR -> SLOT BINDING (that doc's line 158 records the SH4 side as "not
-- located"), the per-tile decomposition, and the quad anchor rule.
USE NS re DB kb;

-- ─────────────────────────────────────────────────────────────────────────────
-- SOURCES
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT source:sg_runner_dlist_walk SET kind='reproduction', strength='reproduction',
  ref='supergun/supersoaker/runner/sg_runner/src/main.rs --dlist / --dlist-shot / --dlist-window, tag supergun-dlist-working',
  note='Walks the engine display list and the draw-list handle table in a live sg_runner over the stage-9 anchor, with no hooks and no debugger. Prints per-quad decode, the slot->part prediction and its gate.';

UPSERT source:mvc_dump_disasm_20260905 SET kind='code', strength='code',
  ref='C:/Users/trist/ghidra_projects/mvc_dump.bin, base 0x140000000, capstone',
  note='Ghidra MCP was down; functions were disassembled from the raw dump. File offset == RVA, no ASLR.';

-- ─────────────────────────────────────────────────────────────────────────────
-- FINDINGS
-- ─────────────────────────────────────────────────────────────────────────────

UPSERT finding:sprite_tile_decomposition SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='A sprite record expands into tiles by a per-tile ALLOCATION LIST, not by part geometry',
  statement='FUN_140612A10 (tiled body). The cell is reached as cell = node+0x1B0 + u32[node+0x1B0 + (sid & 0x7FFF)*4]; u16 cell[0] is the RECORD COUNT and records follow at cell+2 with STRIDE 8 (add r15,8 @0x140612ED5). Per record the PART SELECTOR is u16 [rec+6] (@0x140612BE7, which then indexes gfx1 at node+0x1A8). The TILE COUNT is u8[allocList+1] + 1 (@0x140612BF0/BF5) where allocList = 0x142EED950 + node[0x120]*4, stride 4 (add rbx,4 @0x140612EA3 -- INSIDE the tile loop, so entries are PER TILE). Each entry is {b0 = tile size, b1, b2 = column, b3 = row} and the loop places the tile at (b2*b0, b3*b0) (@0x140612DA1-0x140612DBB), which is the sub-rect of the part that tile covers. The slot cursor is node+0x120, advancing by the tile count, bounded by 0x180 (@0x140612C1F) = the 0x10..0x190 ping-pong window.',
  gate='ROUTE B GATE, live over the stage-9 anchor: predicted slot set vs the set the engine actually emitted = 86 emitted, 86 predicted, 0 missing, 0 extra. Four nodes at cursors 0, 24, 56, 64 whose tile counts 24+32+8+22 = 86 tile contiguously with no gap or overlap. ONE wrong tile count shifts every later cursor and the set stops matching, so this cannot pass by coincidence. The tile grids independently reconstruct the offline atlas part dimensions (part 266 -> 8x8, 274 -> 8x32, 271 -> 64x128, 229 -> 32x8), and neither side was fitted to the other.',
  corrects='TWO PLAUSIBLE READINGS THAT ARE BOTH WRONG. (1) node+0x120 is a RUNNING CURSOR into the allocation list, NOT a per-node slot base -- read as a base it makes a partition test pass arithmetically and mean nothing (two drawn fighters at cursor 0 and 24 do NOT imply the first consumed 24 slots). (2) Tiles per record are DATA, not ceil(w/32)*ceil(h/32) -- that guess gave 14 where the game used 24.';

UPSERT finding:tex_descriptor_slot_binding SET status='confirmed', confidence='high', date='2026-09-05',
  contribution_candidate=true,
  title='The descriptor -> slot binding, and the DC-RAM address of a tile s pixels',
  statement='FUN_140844DC0(rcx = record list, rdx = descriptor array). ctx = *(0x142EF0AB0). It walks records from rcx+0x18 while (i32)[rbx] < 0, advancing rbx += 0x50 + (i32)[rbx+0x4C] (@0x140844EC9-ED1). Per record: texIndex = (i32)[rec+0x20]; SLOT = texIndex + ctx[0x1E0098] (@0x140844DE1/E0C); the DESCRIPTOR is descArray + texIndex*16 (@0x140844E13, shl 4); the resolved slot is written back to [rec+0xC]. Bounded by slot < 0xFFF. The slot entry is ctx + (slot*3 + 0x3C014)*8 == ctx + 0x1E00A0 + slot*0x18. If u16[entry] != 0 the slot is OCCUPIED and the record is skipped. Otherwise it writes u16[entry+0] = 1, u16[entry+2] = desc.fmt (u16 [desc+4]), u16[entry+4] = 0 when paletted else -1 (the (fmt & 0xFF00) - 0x500 test @0x140844E72-E8A), u16[entry+6] = desc.w (u16 [desc+0]), u16[entry+8] = desc.h (u16 [desc+2]), then calls FUN_140048460 to create the host texture and stores it at [entry+0x10]. THE PIXELS: r9 = (u32[desc+8] & 0x1FFFFFF) + ctx[0x1E0088] (@0x140844E4B-E59) -- a 25-bit offset into the DC-RAM image, whose base ctx[0x1E0088] was measured equal to the runner s dcram (0x103c1000).',
  significance='This is what a client needs to draw the game s OWN tiles with no offline art at all: the descriptor gives w/h/fmt and a DC-RAM address, and the DC-RAM image is reproducible from the user s own game_50.arc (Gate 3). TEXTURE-BANKS-GHIDRA.md already documented the 16-byte TEX record and the slot-entry layout; what was open at its line 158 is exactly this binding.',
  caveat='⚠ IN A RUNNER THAT NEVER CREATES TEXTURES THE SLOT TABLE IS STALE. sg_runner reads (w,h) = 32x32 for every slot because those entries are leftovers from the capture session that produced the anchor, while the allocation list gives real tile sizes of 8 AND 32 in the same frame. Trusting slot (w,h) there draws every 8px tile at 4x size -- a picture that is almost right. Until FUN_140844DC0 is ported, take the tile size from the allocation list.';

UPSERT finding:sprite_quad_anchor_rule SET status='confirmed', confidence='high', date='2026-09-05',
  title='Where a 2D quad actually lands: the anchor bits in the quad flags',
  statement='FUN_1408458B0 @0x140845980-0x140845A29, the consumer that expands a 0xC quad into its four vertices and is therefore the authority on placement. W = (float)u16[slot+0x1E00A6] * f32[quad+0x10]; H = (float)u16[slot+0x1E00A8] * f32[quad+0x14]. Then xOff = (flags&3)==1 ? W : (flags&3)==2 ? round(W*0.5) : 0, and yOff = (flags&0xC)==0xC ? H : (flags&0xC)==8 ? round(H*0.5) : 0, and the quad is placed at left = f32[quad+0x04] - xOff, top = f32[quad+0x08] - yOff (@0x140845A17-A23). The function also drops the quad early when u16[slot+0x1E00A0] == 0 (free slot, @0x1408458F9) or when f32[quad+0x0C] <= 0 (@0x140845910), and applies rotation when (flags & 0x20800) == 0x800 (@0x140845946-5D).',
  significance='Observed flags on live character quads are 0x41F, so (flags & 0xC) == 0xC and top = y - H: a FULL-HEIGHT shift on every tile. Drawing at the raw (x,y) scatters the parts off the body and looks like a broken decode rather than a missing offset.',
  caveat='⚠ METHOD NOTE WORTH KEEPING. This was found only after two guesses were drawn and looked at: vflip taken as false (every character upside down) and the quad position taken as a top-left corner (every tile scattered). FUN_1408458B0 was the authority the whole time. Guess-and-look is not a method on this project; port the function.';

-- ─────────────────────────────────────────────────────────────────────────────
-- EDGES
-- ─────────────────────────────────────────────────────────────────────────────

RELATE finding:sprite_tile_decomposition -> cites -> source:mvc_dump_disasm_20260905 SET id='e_172_tile_code';
RELATE finding:sprite_tile_decomposition -> cites -> source:sg_runner_dlist_walk SET id='e_172_tile_repro';
RELATE finding:tex_descriptor_slot_binding -> cites -> source:mvc_dump_disasm_20260905 SET id='e_172_desc_code';
RELATE finding:tex_descriptor_slot_binding -> cites -> source:sg_runner_dlist_walk SET id='e_172_desc_repro';
RELATE finding:sprite_quad_anchor_rule -> cites -> source:mvc_dump_disasm_20260905 SET id='e_172_anchor_code';
RELATE finding:sprite_quad_anchor_rule -> cites -> source:sg_runner_dlist_walk SET id='e_172_anchor_repro';
