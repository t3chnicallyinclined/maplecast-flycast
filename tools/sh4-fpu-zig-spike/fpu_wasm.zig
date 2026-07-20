// Same flycast ftrv/fipr parity check, compiled to wasm32. Exports run()->mismatches
// and count()->records so Node can verify the browser target is ALSO bit-exact.
const std = @import("std");
const data = @embedFile("corpus.bin");

fn fipr(fn_: *const [4]f32, fm: *const [4]f32) f32 {
    const f: f64 = @as(f64, fn_[0]) * @as(f64, fm[0]) + @as(f64, fn_[1]) * @as(f64, fm[1]) + @as(f64, fn_[2]) * @as(f64, fm[2]) + @as(f64, fn_[3]) * @as(f64, fm[3]);
    return @floatCast(f);
}
fn ftrv(fd: *[4]f32, fn_: *const [4]f32, fm: *const [16]f32) void {
    var k: usize = 0;
    while (k < 4) : (k += 1) {
        const f: f64 = @as(f64, fn_[0]) * @as(f64, fm[k + 0]) + @as(f64, fn_[1]) * @as(f64, fm[k + 4]) + @as(f64, fn_[2]) * @as(f64, fm[k + 8]) + @as(f64, fn_[3]) * @as(f64, fm[k + 12]);
        fd[k] = @floatCast(f);
    }
}
fn bits(x: f32) u32 {
    return @bitCast(x);
}

const rec_bytes: usize = 25 * 4;

export fn count() u32 {
    return @intCast(data.len / rec_bytes);
}

export fn run() u32 {
    const n = data.len / rec_bytes;
    var mism: u32 = 0;
    var i: usize = 0;
    while (i < n) : (i += 1) {
        var rec: [25]f32 = undefined;
        @memcpy(std.mem.asBytes(&rec), data[i * rec_bytes .. i * rec_bytes + rec_bytes]);
        const m: *const [16]f32 = rec[0..16];
        const v: *const [4]f32 = rec[16..20];
        const exp_ftrv: *const [4]f32 = rec[20..24];
        const exp_fipr: f32 = rec[24];
        var got: [4]f32 = undefined;
        ftrv(&got, v, m);
        const gfip = fipr(v, m[0..4]);
        var k: usize = 0;
        while (k < 4) : (k += 1) {
            if (bits(got[k]) != bits(exp_ftrv[k])) mism += 1;
        }
        if (bits(gfip) != bits(exp_fipr)) mism += 1;
    }
    return mism;
}
