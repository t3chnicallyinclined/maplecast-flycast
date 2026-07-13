// ta.rs — PowerVR2 TA display-list parser (faithful port of web/webgpu/ta-parser.mjs).
//
// Turns a reconstructed MVC2 Tile-Accelerator command stream into draw-quads.
// Vertex layout: 28 bytes = x(f32) y(f32) z=1/w(f32) col(u8 R,G,B,A) spc(u8 R,G,B,A) u(f32) v(f32)

/// A run of vertices sharing render state (ISP/TSP/TCW/PCW + tile-clip).
#[derive(Clone, Copy, Debug, Default)]
pub struct PolyParam {
    pub first: u32,
    pub count: u32,
    pub isp: u32,
    pub tsp: u32,
    pub tcw: u32,
    pub pcw: u32,
    pub tileclip: u32,
}

/// Parsed draw lists plus the interleaved vertex buffer.
#[derive(Clone, Debug, Default)]
pub struct Parsed {
    pub vertex_data: Vec<u8>, // 28 bytes per vertex
    pub vertex_count: usize,
    pub opaque: Vec<PolyParam>,
    pub punch_through: Vec<PolyParam>,
    pub translucent: Vec<PolyParam>,
}

// --- little-endian readers (bounds-checked; guards in parse() keep these in range) ---

#[inline]
fn ru32(ta: &[u8], o: usize) -> u32 {
    if o + 4 <= ta.len() {
        u32::from_le_bytes([ta[o], ta[o + 1], ta[o + 2], ta[o + 3]])
    } else {
        0
    }
}

#[inline]
fn ru16(ta: &[u8], o: usize) -> u16 {
    if o + 2 <= ta.len() {
        u16::from_le_bytes([ta[o], ta[o + 1]])
    } else {
        0
    }
}

#[inline]
fn rf32(ta: &[u8], o: usize) -> f32 {
    f32::from_bits(ru32(ta, o))
}

// The stream's "f16" is a truncated float32, not IEEE half: high 16 bits of an f32.
#[inline]
fn f16(v: u16) -> f32 {
    f32::from_bits((v as u32) << 16)
}

// JS ToInt32 semantics (truncate toward zero, wrap mod 2^32).
#[inline]
fn to_i32(f: f64) -> i32 {
    if !f.is_finite() {
        return 0;
    }
    let m = f.trunc().rem_euclid(4294967296.0);
    m as u32 as i32
}

// JS: Math.min(255, Math.max(0, (f * 255) | 0))
#[inline]
fn clamp255(f: f64) -> i32 {
    let i = to_i32(f * 255.0);
    if i < 0 {
        0
    } else if i > 255 {
        255
    } else {
        i
    }
}

#[derive(Clone, Copy, PartialEq)]
enum Which {
    None,
    Op,
    Pt,
    Tr,
}

struct Builder {
    vertex_data: Vec<u8>,
    n: usize,
    op: Vec<PolyParam>,
    pt: Vec<PolyParam>,
    tr: Vec<PolyParam>,

    cur_list: i32,
    cur_which: Which,
    cur_pp: Option<PolyParam>,

    tileclip: u32,
    c_isp: u32,
    c_tsp: u32,
    c_tcw: u32,
    c_pcw: u32,
    c_obj: u32,

    fbc: [i32; 4],
    foc: [i32; 4],
}

impl Builder {
    fn new() -> Self {
        Builder {
            vertex_data: Vec::new(),
            n: 0,
            op: Vec::new(),
            pt: Vec::new(),
            tr: Vec::new(),
            cur_list: -1,
            cur_which: Which::None,
            cur_pp: None,
            tileclip: 0,
            c_isp: 0,
            c_tsp: 0,
            c_tcw: 0,
            c_pcw: 0,
            c_obj: 0,
            fbc: [0xFF, 0xFF, 0xFF, 0xFF],
            foc: [0, 0, 0, 0],
        }
    }

    // Emit one 28-byte vertex. bc/oc are packed ARGB8888 (0xAARRGGBB); stored as R,G,B,A bytes.
    #[inline]
    fn push_vtx(&mut self, x: f32, y: f32, z: f32, bc: u32, oc: u32, u: f32, v: f32) {
        let d = &mut self.vertex_data;
        d.extend_from_slice(&x.to_le_bytes());
        d.extend_from_slice(&y.to_le_bytes());
        d.extend_from_slice(&z.to_le_bytes());
        d.push(((bc >> 16) & 0xFF) as u8);
        d.push(((bc >> 8) & 0xFF) as u8);
        d.push((bc & 0xFF) as u8);
        d.push(((bc >> 24) & 0xFF) as u8);
        d.push(((oc >> 16) & 0xFF) as u8);
        d.push(((oc >> 8) & 0xFF) as u8);
        d.push((oc & 0xFF) as u8);
        d.push(((oc >> 24) & 0xFF) as u8);
        d.extend_from_slice(&u.to_le_bytes());
        d.extend_from_slice(&v.to_le_bytes());
        self.n += 1;
    }

    #[inline]
    fn push_current_list(&mut self, pp: PolyParam) {
        match self.cur_which {
            Which::Op => self.op.push(pp),
            Which::Pt => self.pt.push(pp),
            Which::Tr => self.tr.push(pp),
            Which::None => {}
        }
    }

    // Finalize the pending PolyParam: fill its count and commit it if it drew anything.
    fn finalize_pending(&mut self) {
        if let Some(mut pp) = self.cur_pp.take() {
            pp.count = self.n as u32 - pp.first;
            if pp.count > 0 {
                self.push_current_list(pp);
            }
        }
    }

    fn start_list(&mut self, lt: i32) {
        if self.cur_list != -1 {
            return;
        }
        self.cur_list = lt;
        self.cur_which = match lt {
            0 => Which::Op,
            4 => Which::Pt,
            2 => Which::Tr,
            _ => Which::None, // modvol lists 1 & 3 (and any other) are dropped
        };
        self.cur_pp = None;
    }

    fn end_list(&mut self) {
        self.finalize_pending();
        self.cur_pp = None;
        self.cur_which = Which::None;
        self.cur_list = -1;
    }

    // Begin a new PolyParam for the current header (lazy count).
    fn new_pp(&mut self) {
        self.finalize_pending();
        self.cur_pp = Some(PolyParam {
            first: self.n as u32,
            count: 0,
            isp: self.c_isp,
            tsp: self.c_tsp,
            tcw: self.c_tcw,
            pcw: self.c_pcw,
            tileclip: self.tileclip,
        });
    }

    // End of a triangle strip: commit the current run, open a fresh run with the same state.
    fn end_strip(&mut self) {
        if let Some(mut pp) = self.cur_pp.take() {
            let count = self.n as u32 - pp.first;
            if count > 0 {
                pp.count = count;
                let (isp, tsp, tcw, pcw, tileclip) =
                    (pp.isp, pp.tsp, pp.tcw, pp.pcw, pp.tileclip);
                self.push_current_list(pp);
                self.cur_pp = Some(PolyParam {
                    first: self.n as u32,
                    count: 0,
                    isp,
                    tsp,
                    tcw,
                    pcw,
                    tileclip,
                });
            } else {
                self.cur_pp = Some(pp);
            }
        }
    }
}

/// Parse the reconstructed TA buffer into draw-quads. `ta` is prev_ta[..ta_size].
pub fn parse(ta: &[u8]) -> Parsed {
    let mut b = Builder::new();
    let mut off = 0usize;

    while off + 32 <= ta.len() {
        let pcw = ru32(ta, off);
        let para_type = (pcw >> 29) & 7;

        match para_type {
            0 => {
                // End of list
                b.end_list();
                off += 32;
            }
            1 => {
                // User tile clip — sets the clip RECTANGLE, preserving the mode nibble.
                let xmin = ru32(ta, off + 12) & 63;
                let ymin = ru32(ta, off + 16) & 31;
                let xmax = ru32(ta, off + 20) & 63;
                let ymax = ru32(ta, off + 24) & 31;
                b.tileclip = (b.tileclip & 0xF000_0000)
                    | xmin
                    | (xmax << 6)
                    | (ymin << 12)
                    | (ymax << 17);
                off += 32;
            }
            2 => {
                // Object list set
                off += 32;
            }
            3 | 6 => {
                // Reserved
                off += 32;
            }
            4 => {
                // Polygon param
                let lt = ((pcw >> 24) & 7) as i32;
                if b.cur_list == -1 {
                    b.start_list(lt);
                }
                if b.cur_list == 1 || b.cur_list == 3 {
                    // modvol list — drop
                    off += 32;
                } else {
                    // Clip MODE from PCW bits 16-17
                    b.tileclip = (b.tileclip & 0x0FFF_FFFF) | (((pcw >> 16) & 3) << 28);
                    b.c_pcw = pcw;
                    b.c_obj = pcw & 0xFF;
                    b.c_isp = ru32(ta, off + 4);
                    b.c_tsp = ru32(ta, off + 8);
                    b.c_tcw = ru32(ta, off + 12);
                    let col_type = (b.c_obj >> 4) & 3;
                    let vol = (b.c_obj >> 6) & 1;

                    if col_type == 2 && vol == 0 && ((b.c_obj >> 2) & 1) == 1 {
                        // PolyParam2: 64B — face base + offset colors in second 32B
                        if off + 64 <= ta.len() {
                            b.fbc[3] = clamp255(rf32(ta, off + 32) as f64);
                            b.fbc[0] = clamp255(rf32(ta, off + 36) as f64);
                            b.fbc[1] = clamp255(rf32(ta, off + 40) as f64);
                            b.fbc[2] = clamp255(rf32(ta, off + 44) as f64);
                            b.foc[3] = clamp255(rf32(ta, off + 48) as f64);
                            b.foc[0] = clamp255(rf32(ta, off + 52) as f64);
                            b.foc[1] = clamp255(rf32(ta, off + 56) as f64);
                            b.foc[2] = clamp255(rf32(ta, off + 60) as f64);
                            off += 64;
                        } else {
                            off += 32;
                        }
                    } else if col_type >= 1 && vol == 1 {
                        off += if off + 64 <= ta.len() { 64 } else { 32 };
                    } else {
                        if col_type == 1 && vol == 0 {
                            // intensity type 1 — face base color only
                            b.fbc[3] = clamp255(rf32(ta, off + 16) as f64);
                            b.fbc[0] = clamp255(rf32(ta, off + 20) as f64);
                            b.fbc[1] = clamp255(rf32(ta, off + 24) as f64);
                            b.fbc[2] = clamp255(rf32(ta, off + 28) as f64);
                        } else if col_type == 2 && vol == 0 {
                            b.fbc[3] = clamp255(rf32(ta, off + 16) as f64);
                            b.fbc[0] = clamp255(rf32(ta, off + 20) as f64);
                            b.fbc[1] = clamp255(rf32(ta, off + 24) as f64);
                            b.fbc[2] = clamp255(rf32(ta, off + 28) as f64);
                        }
                        off += 32;
                    }

                    if b.cur_which != Which::None {
                        b.new_pp();
                    }
                }
            }
            5 => {
                // Sprite param
                let lt = ((pcw >> 24) & 7) as i32;
                if b.cur_list == -1 {
                    b.start_list(lt);
                }
                b.tileclip = (b.tileclip & 0x0FFF_FFFF) | (((pcw >> 16) & 3) << 28);
                b.c_pcw = pcw;
                b.c_obj = pcw & 0xFF;
                // Sprites flip cull mode (ta_vtx.cpp)
                b.c_isp = ru32(ta, off + 4) ^ (1 << 27);
                b.c_tsp = ru32(ta, off + 8);
                b.c_tcw = ru32(ta, off + 12);
                let sbc = ru32(ta, off + 16);
                b.fbc[0] = ((sbc >> 16) & 0xFF) as i32;
                b.fbc[1] = ((sbc >> 8) & 0xFF) as i32;
                b.fbc[2] = (sbc & 0xFF) as i32;
                b.fbc[3] = ((sbc >> 24) & 0xFF) as i32;
                if b.cur_which != Which::None {
                    b.new_pp();
                }
                off += 32;
            }
            7 => {
                // Vertex
                if b.cur_which == Which::None || b.cur_pp.is_none() {
                    off += 32;
                } else {
                    let eos = (pcw >> 28) & 1;
                    let tex = (b.c_obj >> 3) & 1;
                    let col_type = (b.c_obj >> 4) & 3;
                    let uv16 = b.c_obj & 1;
                    let vol = (b.c_obj >> 6) & 1;
                    let is_spr = ((b.c_pcw >> 29) & 7) == 5;

                    if is_spr && off + 64 <= ta.len() {
                        // Sprite geometry: cv[0]=D cv[1]=C cv[2]=A cv[3]=B; strip order D,C,A,B.
                        let ax = rf32(ta, off + 4);
                        let ay = rf32(ta, off + 8);
                        let az = rf32(ta, off + 12);
                        let bx = rf32(ta, off + 16);
                        let by = rf32(ta, off + 20);
                        let bz = rf32(ta, off + 24);
                        let cx = rf32(ta, off + 28);
                        let cy = rf32(ta, off + 32);
                        let cz = rf32(ta, off + 36);
                        let dx = rf32(ta, off + 40);
                        let dy = rf32(ta, off + 44);

                        let (mut au, mut av) = (0f32, 0f32);
                        let (mut bu, mut bv) = (0f32, 0f32);
                        let (mut cu, mut cv) = (0f32, 0f32);
                        if tex != 0 {
                            av = f16(ru16(ta, off + 52));
                            au = f16(ru16(ta, off + 54));
                            bv = f16(ru16(ta, off + 56));
                            bu = f16(ru16(ta, off + 58));
                            cv = f16(ru16(ta, off + 60));
                            cu = f16(ru16(ta, off + 62));
                        }

                        // Solve D's Z and UV via the sprite plane (CaclulateSpritePlane), in f64.
                        let (axf, ayf, azf) = (ax as f64, ay as f64, az as f64);
                        let (bxf, byf, bzf) = (bx as f64, by as f64, bz as f64);
                        let (cxf, cyf, czf) = (cx as f64, cy as f64, cz as f64);
                        let (dxf, dyf) = (dx as f64, dy as f64);
                        let (auf, avf) = (au as f64, av as f64);
                        let (buf, bvf) = (bu as f64, bv as f64);
                        let (cuf, cvf) = (cu as f64, cv as f64);

                        let acx = cxf - axf;
                        let acy = cyf - ayf;
                        let acz = czf - azf;
                        let abx = bxf - axf;
                        let aby = byf - ayf;
                        let abz = bzf - azf;
                        let apx = dxf - axf;
                        let apy = dyf - ayf;
                        let abu = buf - auf;
                        let abv = bvf - avf;
                        let acu = cuf - auf;
                        let acv = cvf - avf;

                        let k3 = acx * aby - acy * abx;
                        let k2 = if k3 != 0.0 {
                            (apx * aby - apy * abx) / k3
                        } else {
                            0.0
                        };
                        let k1 = if abx != 0.0 {
                            (dxf - axf - k2 * acx) / abx
                        } else if aby != 0.0 {
                            (dyf - ayf - k2 * acy) / aby
                        } else {
                            0.0
                        };
                        let dz = azf + k1 * abz + k2 * acz;
                        let du = auf + k1 * abu + k2 * acu;
                        let dv = avf + k1 * abv + k2 * acv;

                        let bc = ((b.fbc[3] as u32) << 24)
                            | ((b.fbc[0] as u32) << 16)
                            | ((b.fbc[1] as u32) << 8)
                            | (b.fbc[2] as u32);
                        let oc = ((b.foc[3] as u32) << 24)
                            | ((b.foc[0] as u32) << 16)
                            | ((b.foc[1] as u32) << 8)
                            | (b.foc[2] as u32);

                        // Emit in strip order: D, C, A, B
                        b.push_vtx(dx, dy, dz as f32, bc, oc, du as f32, dv as f32);
                        b.push_vtx(cx, cy, cz, bc, oc, cu, cv);
                        b.push_vtx(ax, ay, az, bc, oc, au, av);
                        b.push_vtx(bx, by, bz, bc, oc, bu, bv);
                        off += 64;
                        b.end_strip();
                    } else {
                        if tex == 0 {
                            let x = rf32(ta, off + 4);
                            let y = rf32(ta, off + 8);
                            let z = rf32(ta, off + 12);
                            let mut bc: u32 = 0xFFFF_FFFF;
                            if col_type == 0 {
                                bc = ru32(ta, off + 24);
                            } else if col_type == 1 {
                                bc = ((clamp255(rf32(ta, off + 16) as f64) as u32) << 24)
                                    | ((clamp255(rf32(ta, off + 20) as f64) as u32) << 16)
                                    | ((clamp255(rf32(ta, off + 24) as f64) as u32) << 8)
                                    | (clamp255(rf32(ta, off + 28) as f64) as u32);
                            }
                            b.push_vtx(x, y, z, bc, 0, 0.0, 0.0);
                            off += 32;
                        } else if vol == 0 {
                            if col_type == 0 {
                                let x = rf32(ta, off + 4);
                                let y = rf32(ta, off + 8);
                                let z = rf32(ta, off + 12);
                                let (u, v);
                                if uv16 == 0 {
                                    u = rf32(ta, off + 16);
                                    v = rf32(ta, off + 20);
                                } else {
                                    v = f16(ru16(ta, off + 16));
                                    u = f16(ru16(ta, off + 18));
                                }
                                b.push_vtx(
                                    x,
                                    y,
                                    z,
                                    ru32(ta, off + 24),
                                    ru32(ta, off + 28),
                                    u,
                                    v,
                                );
                                off += 32;
                            } else if col_type == 1 && off + 64 <= ta.len() {
                                let x = rf32(ta, off + 4);
                                let y = rf32(ta, off + 8);
                                let z = rf32(ta, off + 12);
                                let (u, v);
                                if uv16 == 0 {
                                    u = rf32(ta, off + 16);
                                    v = rf32(ta, off + 20);
                                } else {
                                    v = f16(ru16(ta, off + 16));
                                    u = f16(ru16(ta, off + 18));
                                }
                                let bc = ((clamp255(rf32(ta, off + 32) as f64) as u32) << 24)
                                    | ((clamp255(rf32(ta, off + 36) as f64) as u32) << 16)
                                    | ((clamp255(rf32(ta, off + 40) as f64) as u32) << 8)
                                    | (clamp255(rf32(ta, off + 44) as f64) as u32);
                                let oc = ((clamp255(rf32(ta, off + 48) as f64) as u32) << 24)
                                    | ((clamp255(rf32(ta, off + 52) as f64) as u32) << 16)
                                    | ((clamp255(rf32(ta, off + 56) as f64) as u32) << 8)
                                    | (clamp255(rf32(ta, off + 60) as f64) as u32);
                                b.push_vtx(x, y, z, bc, oc, u, v);
                                off += 64;
                            } else {
                                let x = rf32(ta, off + 4);
                                let y = rf32(ta, off + 8);
                                let z = rf32(ta, off + 12);
                                let (u, v);
                                if uv16 == 0 {
                                    u = rf32(ta, off + 16);
                                    v = rf32(ta, off + 20);
                                } else {
                                    v = f16(ru16(ta, off + 16));
                                    u = f16(ru16(ta, off + 18));
                                }
                                let bi = rf32(ta, off + 24) as f64;
                                let oi = rf32(ta, off + 28) as f64;
                                let bc = ((b.fbc[3] as u32) << 24)
                                    | (((to_i32(b.fbc[0] as f64 * bi) as u32) & 0xFF) << 16)
                                    | (((to_i32(b.fbc[1] as f64 * bi) as u32) & 0xFF) << 8)
                                    | ((to_i32(b.fbc[2] as f64 * bi) as u32) & 0xFF);
                                let oc = ((b.foc[3] as u32) << 24)
                                    | (((to_i32(b.foc[0] as f64 * oi) as u32) & 0xFF) << 16)
                                    | (((to_i32(b.foc[1] as f64 * oi) as u32) & 0xFF) << 8)
                                    | ((to_i32(b.foc[2] as f64 * oi) as u32) & 0xFF);
                                b.push_vtx(x, y, z, bc, oc, u, v);
                                off += 32;
                            }
                        } else {
                            // two-volume — skip
                            off += 32;
                        }

                        if eos != 0 {
                            b.end_strip();
                        }
                    }
                }
            }
            _ => {
                off += 32;
            }
        }
    }

    // Finalize any trailing pending run (parse may end without an explicit end-of-list).
    b.finalize_pending();

    // TODO fillBGP — background quad from PVR regs + VRAM is inserted later.

    let vertex_count = b.n;
    Parsed {
        vertex_data: b.vertex_data,
        vertex_count,
        opaque: b.op,
        punch_through: b.pt,
        translucent: b.tr,
    }
}
