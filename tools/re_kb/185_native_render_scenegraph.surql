-- 185_native_render_scenegraph.surql -- Option A increment 2: the render is the MT Framework
-- SceneDraw GRAPH, driven by the host frame FUN_140055D40(app); the compose pass FUN_14006F5E0 is a
-- unit method inside it, not a callable leaf. 2026-09-06. Forward slashes only in ref/note.
USE NS re DB kb;

UPSERT source:native_render_scenegraph_20260906 SET kind='disasm', strength='code',
  ref='GhidraMCP :8080 over mvc_dump.bin: FUN_140055D40, FUN_140054860, FUN_1401DB1B0, FUN_1401DD130/DD1B0/DD310/DD970/DDA50/DCEB0, FUN_14006F5E0, FUN_14006D0E0, FUN_1402089E0, FUN_1402BD3F0, FUN_140297870, FUN_140047A20, FUN_1400388F0, FUN_140039DE0, FUN_14003A520, FUN_140118DD0. Captured heap d3dcap/ttd/runs/vs-01/pre/regions (filter 0x280FB620, nDraw 0x1760EF31000, texmgr 0x292994C0, pkg 0x29215B10, resmgr 0x27FC01E0, hostdev meta 0x2A1DC010). Install file nativeDX11x64/app_shader/AppShaderPackage.mfx.',
  note='Answers the increment-2 open items in finding:native_render_bringup_path / seed 183. Corrects the earlier framing that FUN_14006F5E0(pass,ctx) is a callable leaf.';

UPSERT finding:native_render_is_scenegraph SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='The render is the MT Framework SceneDraw graph driven by the host frame FUN_140055D40(app); FUN_14006F5E0 is a unit vtable method, not a standalone entry',
  statement='FUN_140055D40(app) is the render half of the game frame: it calls vt+0x30 on ~25 subsystem objects, FUN_1402B3A10(nDraw=app+0x3FF60) begin-frame (seed 183), the filter object DAT_142EBBDF0 phase methods (its vtable 0x140924C20: +0x30 FUN_1401DD130, +0x58 FUN_1401DD310, +0x68 FUN_1401DDA50), then nDraw vt+0x30 = FUN_1402B9100, then FUN_1402B6A50(nDraw) = submit FUN_1402BCC60 + IDXGISwapChain::Present + end. Each filter phase method loops views[0..*(filter+0xC38)) and calls the per-view driver (filter vt+0x48 FUN_1401DCEB0 update, vt+0x50 FUN_1401DD1B0, vt+0x60 FUN_1401DD970) which walks the view unit list at filter+0x50+v*0x30 (next at unit+0x18) and invokes each unit vt+0x28/0x40/0x50/0x58. MEASURED live (captured heap): filter+0xC38 = 0x20 views; view-1 head unit 0x2C037450 has class vtable 0x1408E3530 whose vt+0x58 = FUN_14006F5E0 (the compose pass, with RT textures at unit+0x50/+0x60 = nDraw::Texture objects vt 0x14092C8A8, dims-word 0x4000800 = 2048x1024 / 0x2000800 = 2048x512); view-2 head unit 0x2C441690 class vtable 0x1408DD4C0 vt+0x58 = FUN_14006D0E0 (companion draw). So driving the compose pass means running the whole host frame, not calling FUN_14006F5E0 directly.',
  consequence='Increment 2 = run FUN_140054860(app) ONCE (it IS the constructor of the app object FUN_140055D40 renders: it stores nDraw at app slot [0x7FEC]=byte 0x3FF60, filter at [0x7FEF]=0x3FF78, host device at [0x8020]=0x40100 -- all matching FUN_140055D40 reads), then FUN_140055D40(app) per frame after the sim tick. Same shape as increment 1 (one ctor + one per-frame call) on a bigger ctor. Deletes collect_world/decode_tcw/raster once gated.',
  caveat='FUN_140054860 builds ~30 objects incl the shader package and host device; it is the graphics subsystem bring-up, not three calls. Open: the app-object allocator/size (what upstream allocates the buffer FUN_140054860 fills), and whether the resource manager can load synchronously.';

UPSERT finding:native_render_shader_package SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='app/shader/AppShaderPackage resolves to nativeDX11x64/app_shader/AppShaderPackage.mfx in the install; load via the game resource manager (route a)',
  statement='FUN_1402BD3F0(pkg, "app/shader/AppShaderPackage", 1) sets DAT_142EBD9E0=pkg and loads resource type rShader2 (classinfo 0x14095C288, load stream FUN_140297870) through the resource manager DAT_142EBC118 (built by FUN_1402089E0(0x3A1D0,0x100000); its roots "resource"/"nativeDX11x64" come from GetCurrentDirectoryA DAT_1408DB320 + FUN_140140D00). FUN_140297870 validates the stream header *(u32)==0x58464D ("MFX"), +4==0x35, +6==0x24, then walks up to 0x12 record cases relocating offsets. The file exists on disk: nativeDX11x64/app_shader/AppShaderPackage.mfx (1,960,963 B, header 4D 46 58 00 35 00 24 00 = exactly that magic). Then FUN_1402C1590(pkg,res) builds the D3D shader objects from res+0x78[res+0x80] and FUN_1402C1160 sorts them. base.arc also holds common/rtt_hd (type rtt, 16 B header 52 54 58 00 A4 00 00 20... = "RTX", the scene RT descriptor) loaded by FUN_140038430 as common_rtt_hd.',
  consequence='Lightest route = construct the resource manager and call FUN_1402BD3F0 for real (it finds the .mfx by path); hand-building rShader2 means reimplementing the 0x12-case relocation walk. The resource manager uses worker threads (FUN_14020C1E0/FUN_14020B1D0 via CreateThread) to read files, so for a real load the loader CreateThread must be the real kernel32 one, not the increment-1 stub.',
  caveat='Whether the manager can load synchronously (avoiding real threads) is UNKNOWN -- fetch FUN_14020C6E0 / FUN_14020BD50 (the sync vs async load paths inside the mgr vt+0x60 FUN_140209E60) before deciding.';

UPSERT finding:native_render_host_device_ctor SET status='inferred', confidence='high', date='2026-09-06',
  contribution_candidate=true,
  title='FUN_140047A20 (host device ctor, 0xDBD48) allocates the config arena and the staging trio the runtime uses; constructing it fresh retires the --host-gfx fakes but clears the config/G region',
  statement='FUN_140047A20(obj) sets DAT_140acd3a8=obj, PTR_DAT_140acd3a0=&DAT_140ac6d40 then FUN_140800AA0(&DAT_140ac6d40,0,0x6660) (clears the config/G object), allocates via DAT_142E12CF8 vt+0x40: *config = alloc(0x10000000,0x1000); obj[0x1B7A1]=alloc(0x200000,0x10); obj[0x1B7A2]=alloc(0x200000,0x10); obj[0x1B7A3]=alloc(0x10000,0x10). These are the arenas the sim/ring use; the staging trio at obj+0xDBD08/10/18 that --host-gfx VirtualAllocs is this object own. It also derives the game language from *(DAT_142EBCD00+0x2980) and the mode session block lives at obj+0xD03xx (seed 176).',
  consequence='Constructing the host device the game way (FUN_140047A20 on a zeroed 0xDBD48 block, like increment 1 called the nDraw ctor) gives the game its own staging + config + ring, retiring the --host-gfx staging-trio VirtualAlloc, the rr_tex_map patch, the band ret-stubs and the synthetic IRender. But because it CLEARS DAT_140ac6d40 (config/G), it must run BEFORE the select anchor (blk/G/config) is placed -- reordering the runner setup -- or the anchor re-applied after.',
  caveat='The texture band FUN_140270930/272700/272EA0/396D00/397050 then runs against the real device (*(0x142EBD8F0)+0xB8): FUN_140396D00 and FUN_140397050 deref DAT_142EC01A0 (a critical section, --noop-import safe) and the real device. Gate unchanged: docs/RENDER-PIXEL-GATE.md.';

RELATE finding:native_render_is_scenegraph->cites->source:native_render_scenegraph_20260906 SET id='e185_sg_src';
RELATE finding:native_render_is_scenegraph->cites->finding:native_render_inc1_bringup_passed SET id='e185_sg_inc1';
RELATE finding:native_render_is_scenegraph->cites->finding:ndraw_frame_protocol SET id='e185_sg_proto';
RELATE finding:native_render_shader_package->cites->source:native_render_scenegraph_20260906 SET id='e185_shader_src';
RELATE finding:native_render_host_device_ctor->cites->source:native_render_scenegraph_20260906 SET id='e185_hd_src';
RELATE finding:native_render_host_device_ctor->cites->finding:mode_lives_in_host_device_session SET id='e185_hd_mode';

UPDATE finding:native_render_is_scenegraph SET status='confirmed';
UPDATE finding:native_render_shader_package SET status='confirmed';
UPDATE finding:native_render_host_device_ctor SET status='confirmed';
