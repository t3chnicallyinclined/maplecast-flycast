-- 186_world_texture_bank_decode.surql -- the runner's world texture decode: per-bank POL-header
-- resolution (was single-bank) + proven VQ/detwiddle port. Stage pixel-perfect; decode byte-exact.
-- 2026-09-06. Forward slashes only in ref/note.
USE NS re DB kb;

UPSERT source:world_texture_decode_20260906 SET kind='reproduction', strength='reproduction',
  ref='GhidraMCP FUN_14060D470 (stage bind), FUN_1408458A0 (base setter -> ctx+0x1E0098), exe_image.bin PTR globals 0x142edf588/590/598/5a0; d3dcap/replay/rip_texbank.py (proven byte-exact decode) BANKS + decode_host/detwiddle16/expand_vq; sg_runner src/main.rs decode_tcw/resolve_tcw_desc; live run sg_play TEXPAGE log + VQ byte-diff.',
  note='DIFF PROOF: live arc-bank VQ page 0x0C50 (128x128 fmt2 typ3) decoded by the runner == rip_texbank.decode_host on the same DC-RAM bytes, 0 of 65536 bytes differ; without detwiddle 55.84% differ.';

UPSERT finding:world_tex_per_bank_resolution SET status='confirmed', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='World TCW -> texture header is PER-BANK: each bank has its own POL-header object at *(ptr global), array at bankobj+0xB08; resolve by TCW base, not one global array',
  statement='FUN_1408458A0(base) just sets ctx+0x1E0098 = the currently-bound bank base; the game rebinds it per bank per frame, so reading that global once gives a stale/zero value. FUN_14060D470 (stage bind) reads the header array as *(PTR_DAT_142edf588 + 0xB08) with count +0xB00. The four world banks and their pointer globals (bankobj = *(global)): 0x810 common/select *(0x142edf5a0)=0x142ee1760; 0xC10 stage/background *(0x142edf588)=0x142edf630; 0xC50 arc characters/effects *(0x142edf590)=0x142ee0140; 0xC90 hud *(0x142edf598)=0x142ee0c50. Header record 16 bytes {u16 w, u16 h, u8 fmt, u8 typ, u16 pad, u32 loc@+8}; texIndex = tcw - base; pixels at DC-RAM loc & 0x1FFFFFF. The runner previously resolved every TCW against only 0x142ee1760 (common) with guessed bases, so stage/arc/hud all dropped at no-base-matched. sg_runner resolve_tcw_desc now walks the four banks and accepts the one whose header dims match the slot table ctx+0x1E00A0+tcw*0x18 (+6 w, +8 h).',
  consequence='Stage/background render pixel-perfect; HUD present; failed-to-decode pages fell from 39 to 1 (a stray 0x0817). Cross-check against the slot table means a wrong bank simply does not match rather than decoding garbage.';

UPSERT finding:world_tex_decode_byte_exact SET status='confirmed', confidence='high', date='2026-09-06',
  title='The runner world texture decode is byte-exact with the proven reference; type 3 is VQ (not palette) and detwiddle is rectangular-aware',
  statement='decode_tcw was ported verbatim from rip_texbank.py decode_host: type 1 = twiddled 16bpp, type 3 = VQ (0x800-byte codebook of 256 x 4 u16 + (w*h)/4 index bytes in 2x2-block twiddle order, each index expanded to its 4 codebook words then plain detwiddle), fmt 0 ARGB1555 / 1 RGB565 / 2 ARGB4444, detwiddle16 handles square and non-square (min(w,h) squares along the long axis). VERIFIED: a live 0xC50 VQ page decoded by the runner equals rip_texbank on the same bytes, 0/65536 differ. The previous inline code decoded type 3 as a 4-bit palette and used a square-only twiddle -- both wrong for battle pages.',
  consequence='Texture decode is DONE and correct. The remaining in-match character garble is NOT decode -- it is sprite geometry/UV in collect_world (atlas sub-rect UVs vs full-page stage UVs). Separate open item; also the character-select preview sprite + banner (per-character on-highlight load, FUN_14060C370(6)) are still absent.',
  caveat='One page (0x0817) still fails: slot says 128x128 but the common bank header at that index is 256x256 -- a stale/rebound select page, low priority.';

RELATE finding:world_tex_per_bank_resolution->cites->source:world_texture_decode_20260906 SET id='e186_bank_src';
RELATE finding:world_tex_decode_byte_exact->cites->source:world_texture_decode_20260906 SET id='e186_dec_src';
RELATE finding:world_tex_decode_byte_exact->cites->finding:world_tex_per_bank_resolution SET id='e186_dec_bank';

UPDATE finding:world_tex_per_bank_resolution SET status='confirmed';
UPDATE finding:world_tex_decode_byte_exact SET status='confirmed';
