#!/usr/bin/env python3
"""Simulate the 'TA command dictionary' wire on a capture:
per frame: [u32 ref ids for each canon-masked block, in emit order] + [new dictionary blocks].
Streaming zstd-3 (shared window) over the concatenation, flush per frame == the
same compressor discipline as REPORT.md experiment (g). Textures assumed LOCAL
(VRAM pages NOT shipped; PVR palette pages reported separately from REPORT.md).
Read-only. Output stdout.
"""
import struct, sys
import zstandard as zstd

CAP = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\trist\projects\maplecast-flycast\_bwlab\cap_prod_play.mirror.zcst"

def read_framed(path):
    data = open(path, "rb").read()
    off, msgs = 0, []
    while off + 4 <= len(data):
        n = struct.unpack_from("<I", data, off)[0]; off += 4
        msgs.append(data[off:off+n]); off += n
    return msgs

def reconstruct(msgs):
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

Z4=b"\0"*4; Z8=b"\0"*8; Z16=b"\0"*16; Z28=b"\0"*28

def walk_canon_blocks(ta):
    """Yield canon-masked blocks in emit order (same FSM as taCanonicalize)."""
    taSize=len(ta); off=0; curList=-1; inPolyList=False; isSpr=False; haveParam=False; cObj=0
    out=[]
    while off + 32 <= taSize:
        pcw = struct.unpack_from("<I", ta, off)[0]
        pt = (pcw >> 29) & 7
        if pt in (0,1,2,3,6):
            haveParam=False
            if pt==0:
                curList=-1; inPolyList=False
                out.append(ta[off:off+4]+Z28)
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
            if sz==32 and colType in (0,1) and not vol: b=b[:16]+Z16
            out.append(b); off+=sz; continue
        if pt==5:
            lt=(pcw>>24)&7
            if curList==-1: curList=lt; inPolyList=lt in (0,2,4)
            cObj=pcw&0xFF; isSpr=True; haveParam=True
            out.append(ta[off:off+24]+Z8); off+=32; continue
        if pt==7:
            if not inPolyList or not haveParam: out.append(ta[off:off+32]); off+=32; continue
            tex=(cObj>>3)&1; colType=(cObj>>4)&3; vol=(cObj>>6)&1
            if isSpr and off+64<=taSize:
                out.append(ta[off:off+48]+Z4+ta[off+52:off+64]); off+=64; continue
            if not tex: sz=32
            elif not vol: sz=64 if (colType==1 and off+64<=taSize) else 32
            else: sz=32
            b=ta[off:off+sz]
            if sz==32 and (not tex) and colType==0: b=b[:16]+Z8+b[24:28]+Z4
            elif sz==64: b=b[:24]+Z8+b[32:]
            out.append(b); off+=sz; continue
        out.append(ta[off:off+32]); off+=32
    return out

def main():
    frames = reconstruct(read_framed(CAP))
    n = len(frames)
    dur_s = n / 60.0
    print(f"{n} frames = {dur_s:.1f}s")

    dictionary = {}         # block bytes -> id
    cctx = zstd.ZstdCompressor(level=3)
    comp = cctx.compressobj()  # streaming, shared window
    total_comp = 0
    total_raw_refs = 0
    total_new_bytes = 0
    per_frame_comp = []
    for fi, ta in enumerate(frames):
        blocks = walk_canon_blocks(ta)
        refs = bytearray()
        newb = bytearray()
        for b in blocks:
            bid = dictionary.get(b)
            if bid is None:
                bid = len(dictionary)
                dictionary[b] = bid
                newb += b
            refs += struct.pack("<I", bid)
        payload = struct.pack("<II", len(blocks), len(newb)) + bytes(refs) + bytes(newb)
        c = comp.compress(payload) + comp.flush(zstd.COMPRESSOBJ_FLUSH_BLOCK)
        total_comp += len(c)
        per_frame_comp.append(len(c))
        total_raw_refs += len(refs)
        total_new_bytes += len(newb)

    mbps = total_comp / dur_s * 8 / 1e6
    print(f"dictionary size at end: {len(dictionary):,} blocks, {sum(len(b) for b in dictionary):,} B content")
    print(f"raw refs {total_raw_refs:,} B, new-block bytes {total_new_bytes:,} B")
    print(f"DICT-WIRE compressed (streaming zstd-3, flush/frame): {total_comp:,} B = {mbps:.3f} Mbps  avg {total_comp/n:.0f} B/frame")
    import statistics
    print(f"per-frame compressed: median {statistics.median(per_frame_comp):.0f} B, p95 {sorted(per_frame_comp)[int(0.95*n)]} B, max {max(per_frame_comp)} B")
    print("NOTE: textures assumed LOCAL (no VRAM pages). Add ~0.31 Mbps if PVR/palette pages ship as today (REPORT.md).")

if __name__ == "__main__":
    main()
