// mvc2-executor scaffold — the thin SH4 game-tick executor (Zig, wasm-bound).
// Milestone 1: run a REAL MVC2 game routine (RNG, loc_8c11e730) from an actual
// RAM snapshot, byte-exact vs flycast. Establishes the pattern: SH4 ctx + flat
// little-endian guest RAM + hand-/lift-transpiled routines mirroring flycast's
// determinism-validated interpreter (the semantics we proved bit-exact in Zig).
const std = @import("std");

// Snapshot region (4KB around the RNG seed, from tools/render-replica-poc/_ram_f90.bin).
const seed_page = @embedFile("seed_page.bin");
const PAGE_BASE: u32 = 0x16B000;

var ram: [0x1000]u8 = undefined; // this milestone's working RAM (the seed page)

// guest addr (SH4 area-3 cached 0x8C.., mask to phys) -> page offset. DC SH4 = little-endian.
fn off(addr: u32) u32 {
    return (addr & 0x00FFFFFF) - PAGE_BASE;
}
fn rd32(addr: u32) u32 {
    const o = off(addr);
    return std.mem.readInt(u32, ram[o..][0..4], .little);
}
fn wr32(addr: u32, v: u32) void {
    const o = off(addr);
    std.mem.writeInt(u32, ram[o..][0..4], v, .little);
}

const Sh4 = struct {
    r: [16]u32 = [_]u32{0} ** 16,
    macl: u32 = 0,
};

// loc_8c11e730 Rng_function (bank11:35173) — hand-transpiled, flycast semantics.
// PC-relative literal-pool loads resolved to their #data constants (as lift.py does).
fn rng(c: *Sh4) u32 {
    c.r[4] = 0x8C16BC2C; // mov.l @(loc_8C11E7AC,pc),r4   -> &seed
    c.r[3] = 0x41C64E6D; // mov.l @(loc_8C11E7B0,pc),r3   -> LCG mult
    // sts.l macl,@-r15  (save macl; pure leaf, stack not modeled)
    const r2v = rd32(c.r[4]); // mov.l @r4,r2  -> seed
    c.r[1] = 0x3039; // mov.w @(loc_8C11E7A6,pc),r1  -> LCG add
    // mul.l r3,r2 -> macl = (u32)((s32)r3 * (s32)r2)   [sh4_opcodes.cpp:1414]
    c.macl = @bitCast(@as(i32, @bitCast(c.r[3])) *% @as(i32, @bitCast(r2v)));
    c.r[2] = 0x7FFF; // mov.w @(loc_8C11E7A8,pc),r2  -> mask
    c.r[3] = c.macl; // sts macl,r3
    c.r[3] +%= c.r[1]; // add r1,r3
    c.r[0] = c.r[3]; // mov r3,r0
    c.r[0] >>= 16; // shlr16 r0
    c.r[0] &= c.r[2]; // and r2,r0
    wr32(c.r[4], c.r[3]); // mov.l r3,@r4  -> store new seed
    return c.r[0]; // rts; return in r0
}

pub fn main() void {
    @memcpy(ram[0..], seed_page);
    const before = rd32(0x8C16BC2C);
    var c = Sh4{};
    const ret = rng(&c);
    const after = rd32(0x8C16BC2C);

    const REF_SEED: u32 = 0x92EA332A;
    const REF_RET: u32 = 0x12EA;
    std.debug.print("snapshot seed        = 0x{X:0>8}\n", .{before});
    std.debug.print("executor RNG -> seed = 0x{X:0>8}  ret = 0x{X:0>4}\n", .{ after, ret });
    std.debug.print("flycast reference    = 0x{X:0>8}  ret = 0x{X:0>4}\n", .{ REF_SEED, REF_RET });
    const ok = (after == REF_SEED and ret == REF_RET);
    if (ok) {
        std.debug.print("\nMILESTONE 1: real MVC2 routine (RNG) byte-exact from snapshot in the Zig executor\n", .{});
    } else {
        std.debug.print("\nMISMATCH\n", .{});
        std.process.exit(1);
    }
}
