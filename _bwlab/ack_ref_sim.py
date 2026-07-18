#!/usr/bin/env python3
# ACK-reference codec simulator. Reads raw inners ([u32 len][inner]...) and, for each
# reference distance K, encodes frame N as zstd(inner_N, dict=inner_{N-K}) -- i.e. the
# client ACKed frame N-K and the server references it. Measures encoded size (=> Mbps)
# and VERIFIES a byte-exact round-trip. K=1..3 models a clean low-RTT link (ACK ~1-2
# frames back); large K models a lagging/lossy client. Streaming (persistent window)
# is the thin baseline we must approach.
import sys, struct
try:
    import zstandard as zstd
except ImportError:
    sys.exit("pip install zstandard")

path = sys.argv[1] if len(sys.argv) > 1 else None
if not path:
    sys.exit("usage: ack_ref_sim.py <inners.bin>")

inners = []
with open(path, 'rb') as f:
    data = f.read()
off = 0
while off + 4 <= len(data):
    (l,) = struct.unpack_from('<I', data, off); off += 4
    if off + l > len(data): break
    inners.append(data[off:off+l]); off += l
WARM = 60  # skip the dict-fill warmup (early frames ship the whole dict as 'news')
inners = inners[WARM:]
n = len(inners)
print(f"frames={n} (after {WARM}-frame warmup)  avg_raw={sum(len(x) for x in inners)//max(n,1)}B")

def raw_dict(b):
    return zstd.ZstdCompressionDict(b, dict_type=zstd.DICT_TYPE_RAWCONTENT)

def mbps(total_bytes, frames):
    return total_bytes * 8 / (frames / 60) / 1e6

# --- SINGLE-frame ref: encode N against N-K (one frame back), verify round-trip ---
print("single-frame dict (ref = ONE frame, K back):")
for K in (1, 2, 3, 5, 60):
    total = 0; frames = 0; bad = 0; big = 0
    for i in range(K, n):
        ref = inners[i - K]
        c = zstd.ZstdCompressor(level=3, dict_data=raw_dict(ref)).compress(inners[i])
        total += len(c); frames += 1
        if len(c) > 1200: big += 1
        if zstd.ZstdDecompressor(dict_data=raw_dict(ref)).decompress(c) != inners[i]: bad += 1
    print(f"  K={K:<3} avg={total//max(frames,1):>6}B  {mbps(total,frames):.2f} Mbps  >1200B={big}/{frames}  bad={bad}")

# --- MULTI-frame ref: dict = the last M frames (all ACKed => present on client) ---
print("multi-frame dict (ref = last M frames, K=1 lag):")
for M in (2, 4, 8, 16):
    total = 0; frames = 0; bad = 0; big = 0
    for i in range(M, n):
        ref = b''.join(inners[i - M:i])          # concat last M frames
        c = zstd.ZstdCompressor(level=3, dict_data=raw_dict(ref)).compress(inners[i])
        total += len(c); frames += 1
        if len(c) > 1200: big += 1
        if zstd.ZstdDecompressor(dict_data=raw_dict(ref)).decompress(c) != inners[i]: bad += 1
    print(f"  M={M:<3} avg={total//max(frames,1):>6}B  {mbps(total,frames):.2f} Mbps  >1200B={big}/{frames}  bad={bad}")

# --- streaming baseline (persistent window over the whole run) ---
total = len(zstd.ZstdCompressor(level=3).compress(b''.join(inners)))
print(f"streaming (persistent window)  avg={total//n:>6}B  {mbps(total,n):.2f} Mbps  (thin baseline)")
