#!/usr/bin/env python3
"""Phase 0 gate for docs/TA-DICT-WIRE-PLAN.md (TDW1 content-addressed TA wire).

Spec-exact encoder + decoder pair (plan section 2a, INCLUDING the u8 length
prefix on new-dictionary blocks — the deliberate deviation from
ta_dictwire_sim.py) run over a .mirror.zcst capture:

  G0-A  mask OFF: encode -> decode == ORIGINAL TA buffer, byte-exact, every frame
  G0-B  mask ON : encode -> decode == taCanonicalize(original), byte-exact, every
        frame, where the canon reference is an INDEPENDENT implementation
        (killRange-style in-place zeroing, mirroring maplecast_mirror.cpp:514-572)
        rather than the walker's block masking — a genuine cross-check.
  G0-C  numbers: masked + unmasked wire Mbps under the spec encoding
        (20B inner header + u32 refs + {u8 len + block} news), streaming
        zstd-3, flush per frame — directly comparable to ta_dictwire_sim.py.

Read-only on the capture. Output stdout. Usage:
  python ta_dictwire_decode.py [--gate] <capture.mirror.zcst>
"""
import struct, sys, statistics
import zstandard as zstd

Z4=b"\0"*4; Z8=b"\0"*8; Z16=b"\0"*16; Z28=b"\0"*28

def read_framed(path):
    data = open(path, "rb").read()
    off, msgs = 0, []
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        msgs.append(data[off:off+n]); off += n
    return msgs

def reconstruct(msgs):
    """Rebuild full per-frame TA buffers from the legacy ZCST delta chain
    (mirror of the client delta-apply, maplecast_mirror.cpp:1605-1634)."""
    dctx = zstd.ZstdDecompressor()
    prev = None; frames = []
    for m in msgs:
        if len(m) < 8 or m[:4] != b"ZCST": continue
        usize = struct.unpack_from("<I", m, 4)[0]
        raw = dctx.decompress(m[8:], max_output_size=usize)
        if raw[:4] == b"SYNC": continue
        taSize, dps = struct.unpack_from("<II", raw, 72)
        p = 80
        if dps == taSize:
            buf = bytearray(raw[p:p+taSize])
        else:
            if prev is None: continue
            buf = bytearray(taSize)
            n = min(taSize, len(prev)); buf[:n] = prev[:n]
            q, end = p, p + dps
            while q + 4 <= end:
                o = struct.unpack_from("<I", raw, q)[0]; q += 4
                if o == 0xFFFFFFFF: break
                rl = struct.unpack_from("<H", raw, q)[0]; q += 2
                if o + rl <= taSize: buf[o:o+rl] = raw[q:q+rl]
                q += rl
        prev = buf
        frames.append(bytes(buf))
    return frames

def walk_blocks(ta, mask):
    """Yield TA parameter blocks in emit order (same FSM as taCanonicalize,
    maplecast_mirror.cpp:514-572). mask=True applies the TACANON dead-byte map
    at block granularity; mask=False copies raw bytes (concat == original)."""
    taSize=len(ta); off=0; curList=-1; inPolyList=False; isSpr=False; haveParam=False; cObj=0
    out=[]
    while off + 32 <= taSize:
        pcw = struct.unpack_from("<I", ta, off)[0]
        pt = (pcw >> 29) & 7
        if pt in (0,1,2,3,6):
            haveParam=False
            if pt==0:
                curList=-1; inPolyList=False
                out.append((ta[off:off+4]+Z28) if mask else ta[off:off+32])
            else:
                out.append(ta[off:off+32])
            off+=32; continue
        if pt==4:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            if curList in (1,3): haveParam=False; out.append(ta[off:off+32]); off+=32; continue
            cObj=pcw&0xFF; isSpr=False; haveParam=True
            colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if colType==2 and not vol and ((cObj>>2)&1): sz=64 if off+64<=taSize else 32
            elif colType>=1 and vol: sz=64 if off+64<=taSize else 32
            else: sz=32
            b=ta[off:off+sz]
            if mask and sz==32 and colType in (0,1) and not vol: b=b[:16]+Z16
            out.append(b); off+=sz; continue
        if pt==5:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            cObj=pcw&0xFF; isSpr=True; haveParam=True
            out.append((ta[off:off+24]+Z8) if mask else ta[off:off+32]); off+=32; continue
        if pt==7:
            if not inPolyList or not haveParam: out.append(ta[off:off+32]); off+=32; continue
            tex=(cObj>>3)&1; colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if isSpr and off+64<=taSize:
                out.append((ta[off:off+48]+Z4+ta[off+52:off+64]) if mask else ta[off:off+64]); off+=64; continue
            if not tex: sz=32
            elif not vol: sz=64 if (colType==1 and off+64<=taSize) else 32
            else: sz=32
            b=ta[off:off+sz]
            if mask:
                if sz==32 and (not tex) and colType==0: b=b[:16]+Z8+b[24:28]+Z4
                elif sz==64: b=b[:24]+Z8+b[32:]
            out.append(b); off+=sz; continue
        out.append(ta[off:off+32]); off+=32
    if off != taSize:
        raise AssertionError(f"walker coverage hole: off={off} taSize={taSize}")
    return out

def canon_reference(ta):
    """INDEPENDENT canon implementation: in-place killRange zeroing on a copy,
    structured like the C++ (maplecast_mirror.cpp killRange), NOT like the
    walker's masked-block concatenation. Cross-checks G0-B."""
    buf = bytearray(ta); taSize = len(buf)
    def kill(lo, hi):
        hi = min(hi, taSize)
        if lo < hi: buf[lo:hi] = b"\0" * (hi - lo)
    off=0; curList=-1; inPolyList=False; isSpr=False; haveParam=False; cObj=0
    while off + 32 <= taSize:
        pcw = struct.unpack_from("<I", buf, off)[0]
        pt = (pcw >> 29) & 7
        if pt in (0,1,2,3,6):
            haveParam=False
            if pt==0: curList=-1; inPolyList=False; kill(off+4, off+32)
            off+=32; continue
        if pt==4:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            if curList in (1,3): haveParam=False; off+=32; continue
            cObj=pcw&0xFF; isSpr=False; haveParam=True
            colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if colType==2 and not vol and ((cObj>>2)&1): sz=64 if off+64<=taSize else 32
            elif colType>=1 and vol: sz=64 if off+64<=taSize else 32
            else: sz=32
            if sz==32 and colType in (0,1) and not vol: kill(off+16, off+32)
            off+=sz; continue
        if pt==5:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            cObj=pcw&0xFF; isSpr=True; haveParam=True
            kill(off+24, off+32); off+=32; continue
        if pt==7:
            if not inPolyList or not haveParam: off+=32; continue
            tex=(cObj>>3)&1; colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if isSpr and off+64<=taSize: kill(off+48, off+52); off+=64; continue
            if not tex:
                sz=32
                if colType==0: kill(off+16, off+24); kill(off+28, off+32)
            elif not vol:
                sz=64 if (colType==1 and off+64<=taSize) else 32
                if sz==64: kill(off+24, off+32)
            else: sz=32
            off+=sz; continue
        off+=32
    return bytes(buf)

def encode_frame(blocks, dictionary, frameNum, vframe, taSize):
    """Spec 2a inner payload: header(20) + refs(u32 x n) + news{u8 len + bytes}."""
    refs = bytearray(); news = bytearray()
    for b in blocks:
        bid = dictionary.get(b)
        if bid is None:
            bid = len(dictionary); dictionary[b] = bid
            news.append(len(b)); news += b
        refs += struct.pack("<I", bid)
    return struct.pack("<IIIII", frameNum, vframe, taSize, len(blocks), len(news)) + bytes(refs) + bytes(news)

def decode_frame(inner, dict_list):
    """Normative decoder (plan section 2a). dict_list is the client dict (list of bytes)."""
    frameNum, vframe, taSize, nBlocks, newSection = struct.unpack_from("<IIIII", inner, 0)
    p = 20
    refs_end = p + 4 * nBlocks
    np_, news_end = refs_end, refs_end + newSection
    out = bytearray()
    for i in range(nBlocks):
        bid = struct.unpack_from("<I", inner, p + 4*i)[0]
        if bid == len(dict_list):
            ln = inner[np_]; np_ += 1
            blk = inner[np_:np_+ln]; np_ += ln
            dict_list.append(blk)
        elif bid > len(dict_list):
            raise AssertionError(f"desync: ref {bid} > dict {len(dict_list)}")
        out += dict_list[bid]
    if np_ != news_end: raise AssertionError("newBlocks section length mismatch")
    if len(out) != taSize: raise AssertionError(f"taSize {taSize} != rebuilt {len(out)}")
    return bytes(out)

def run_variant(frames, mask, gate):
    dictionary = {}; dict_list = []
    cctx = zstd.ZstdCompressor(level=3)
    comp = cctx.compressobj()
    total = 0; per_frame = []; mismatches = 0
    for fi, ta in enumerate(frames):
        blocks = walk_blocks(ta, mask)
        inner = encode_frame(blocks, dictionary, fi, 0, len(ta))
        c = comp.compress(inner) + comp.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        total += len(c); per_frame.append(len(c))
        if gate:
            dec = decode_frame(inner, dict_list)
            ref = ta if not mask else canon_reference(ta)
            if dec != ref:
                mismatches += 1
                if mismatches <= 3:
                    d = next(i for i in range(min(len(dec), len(ref))) if dec[i] != ref[i])
                    print(f"  MISMATCH frame {fi} first-diff byte {d}")
    n = len(frames); dur = n / 60.0
    mbps = total / dur * 8 / 1e6
    label = "mask-ON " if mask else "mask-OFF"
    g = ""
    if gate:
        g = f"  GATE {'PASS' if mismatches == 0 else 'FAIL'} ({n - mismatches}/{n} frames byte-exact)"
    print(f"[{label}] dict={len(dictionary):,} blocks/{sum(len(b) for b in dictionary):,}B  "
          f"wire={total:,}B = {mbps:.3f} Mbps  median={statistics.median(per_frame):.0f}B "
          f"p95={sorted(per_frame)[int(0.95*n)]}B max={max(per_frame)}B{g}")
    return mismatches

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    gate = "--gate" in sys.argv
    cap = args[0] if args else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst"
    frames = reconstruct(read_framed(cap))
    print(f"{cap}: {len(frames)} frames = {len(frames)/60.0:.1f}s  (gate={'ON' if gate else 'OFF'})")
    bad  = run_variant(frames, mask=False, gate=gate)   # G0-A + unmasked G0-C
    bad += run_variant(frames, mask=True,  gate=gate)   # G0-B + masked  G0-C
    if gate:
        print("G0 RESULT:", "ALL GATES PASS" if bad == 0 else f"{bad} FRAME MISMATCHES — DO NOT PROCEED")
        sys.exit(0 if bad == 0 else 1)

if __name__ == "__main__":
    main()
