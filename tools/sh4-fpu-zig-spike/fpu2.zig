// Phase 0.4 — Zig scalar FP ops vs flycast. Same file builds native (main) and
// wasm (run/count exports). ftrc replicates flycast's clamp explicitly so wasm
// never traps on out-of-range float->int.
const std = @import("std");
const data = @embedFile("corpus2.bin");

fn ftrc(x: f32) u32 {
    if (std.math.isNan(x)) return 0x80000000;
    if (x >= 2147483648.0) return 0x7fffffff; // >= 2^31 -> INT_MAX (flycast's fpul-- / clamp path)
    if (x < -2147483648.0) return 0x80000000; // < -2^31 -> INT_MIN
    const i: i32 = @intFromFloat(x); // in range: truncate toward zero
    if (i > 0x7fffff80) return 0x7fffffff;
    return @bitCast(i);
}

fn bits(x: f32) u32 {
    return @bitCast(x);
}

const rb: usize = 9 * 4;

export fn count() u32 {
    return @intCast(data.len / rb);
}

export fn run() u32 {
    const n = data.len / rb;
    var mism: u32 = 0;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        var r: [9]f32 = undefined;
        @memcpy(std.mem.asBytes(&r), data[i * rb .. i * rb + rb]);
        const a = r[0];
        const b = r[1];
        const c = r[2];
        if (bits(a + b) != bits(r[3])) mism += 1;
        if (bits(a - b) != bits(r[4])) mism += 1;
        if (bits(a * b) != bits(r[5])) mism += 1;
        if (bits(a / b) != bits(r[6])) mism += 1;
        if (bits(@mulAdd(f32, a, b, c)) != bits(r[7])) mism += 1;
        const exp_ftrc: u32 = @bitCast(r[8]);
        if (ftrc(a) != exp_ftrc) mism += 1;
    }
    return mism;
}

pub fn main() void {
    const m = run();
    std.debug.print("zig fp-ops: {d} records ({d} outputs), {d} mismatches\n", .{ count(), count() * 6, m });
    if (m != 0) std.process.exit(1);
}
