// wasm variant of the scalar FP-op parity check — exports only (no main/std.debug/
// std.process, which don't link on wasm32-freestanding). Node calls run()/count().
const std = @import("std");
const data = @embedFile("corpus2.bin");

fn ftrc(x: f32) u32 {
    if (std.math.isNan(x)) return 0x80000000;
    if (x >= 2147483648.0) return 0x7fffffff;
    if (x < -2147483648.0) return 0x80000000;
    const i: i32 = @intFromFloat(x);
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
