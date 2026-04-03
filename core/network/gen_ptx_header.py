#!/usr/bin/env python3
"""Generate C header with embedded PTX kernel string."""
import os

ptx_file = os.path.join(os.path.dirname(__file__), "rgba_to_nv12.ptx")
out_file = os.path.join(os.path.dirname(__file__), "rgba_to_nv12_ptx.h")

with open(ptx_file, "r") as f:
    lines = f.readlines()

with open(out_file, "w") as out:
    out.write("// Auto-generated from rgba_to_nv12.cu via nvcc — do not edit\n")
    out.write("static const char _ptxRGBAtoNV12[] =\n")
    for line in lines:
        line = line.rstrip("\n").replace("\\", "\\\\").replace('"', '\\"')
        out.write(f'    "{line}\\n"\n')
    out.write(";\n")

print(f"Generated {out_file} ({len(lines)} lines)")
