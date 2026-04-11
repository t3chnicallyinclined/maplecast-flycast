/*
 * SH4Recomp Verification Harness
 *
 * Runs inside flycast with the JIT executing normally (game renders).
 * When a game code block compiles for the first time, we:
 *   1. Snapshot Sh4cntx BEFORE
 *   2. Let the JIT/interpreter execute the block (correct result)
 *   3. Snapshot Sh4cntx AFTER (= expected)
 *   4. Restore BEFORE, run our static block
 *   5. Compare our result vs expected
 *   6. Restore expected state (game continues correctly)
 *
 * Output: verify_results.csv with per-block pass/fail
 *
 * Build with: cmake -DSH4RECOMP_VERIFY=ON
 */

#ifdef SH4RECOMP_VERIFY

#include "types.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_interpreter.h"
#include <cstdio>
#include <cstring>
#include <set>

// Block lookup from generated code
extern "C" {
    typedef u32 (*RecompBlockFunc)(u8* mem, Sh4Context* ctx);
    RecompBlockFunc recomp_find_block(u32 pc);
}

static FILE* g_verify_log = nullptr;
static u64 g_total_verified = 0;
static u64 g_total_passed = 0;
static u64 g_total_failed = 0;

static void verify_init() {
    if (g_verify_log) return;
    g_verify_log = fopen("/tmp/sh4recomp_verify.csv", "w");
    if (g_verify_log) {
        fprintf(g_verify_log, "block_addr,result,fail_field,expected,actual\n");
        printf("[sh4recomp-verify] Verification log → verify_results.csv\n");
        fflush(stdout);
    }
}

static void log_diff(u32 block_addr, const char* field, u32 expected, u32 actual) {
    if (g_verify_log) {
        fprintf(g_verify_log, "0x%08X,FAIL,%s,0x%08X,0x%08X\n",
            block_addr, field, expected, actual);
        fflush(g_verify_log);
    }
}

static void log_diff_float(u32 block_addr, const char* field, float expected, float actual) {
    if (g_verify_log) {
        u32 e, a;
        memcpy(&e, &expected, 4);
        memcpy(&a, &actual, 4);
        fprintf(g_verify_log, "0x%08X,FAIL,%s,0x%08X,0x%08X\n",
            block_addr, field, e, a);
        fflush(g_verify_log);
    }
}

static void log_pass(u32 block_addr) {
    if (g_verify_log) {
        fprintf(g_verify_log, "0x%08X,PASS,,,\n", block_addr);
    }
}

// Run the interpreter for one block's worth of instructions
// Starting from ctx->pc, execute until we reach a branch/jump
static u32 interp_exec_block(u32 start_pc, u32 sh4_size) {
    // Ensure interpreter instance exists for delay slot handling
    static Sh4Interpreter* interp = nullptr;
    if (!interp) {
        interp = new Sh4Interpreter();
        interp->Init();
    }
    Sh4Interpreter* old_instance = Sh4Interpreter::Instance;
    Sh4Interpreter::Instance = interp;

    Sh4cntx.pc = start_pc;
    int insn_count = sh4_size / 2;

    // Execute exactly insn_count instructions — matches our block which
    // runs each instruction once (even in loops, the block exits after
    // one iteration and returns the branch target)
    for (int i = 0; i < insn_count; i++) {
        u32 addr = Sh4cntx.pc;
        u16 op = IReadMem16(addr);
        Sh4cntx.pc = addr + 2;

        if (Sh4cntx.sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
            continue;

        // For branch instructions with delay slots, the handler will
        // execute the delay slot internally and set PC to the target.
        // We need to stop after this — the delay slot counts as the
        // next instruction, so skip one from our count.
        u32 pc_before = Sh4cntx.pc;
        OpPtr[op](&Sh4cntx, op);

        // If the handler changed PC to outside the block (branch taken),
        // the delay slot was already executed internally. Stop here.
        if (Sh4cntx.pc < start_pc || Sh4cntx.pc >= start_pc + sh4_size) {
            // PC left the block — branch was taken (with delay slot)
            break;
        }
    }

    Sh4Interpreter::Instance = old_instance;
    return Sh4cntx.pc;
}

// Compare two contexts, report differences
static bool verify_context(const Sh4Context* expected, const Sh4Context* actual,
                           u32 block_addr, u32 expected_pc, u32 actual_pc) {
    bool pass = true;
    char field[32];

    // Next PC
    if (expected_pc != actual_pc) {
        log_diff(block_addr, "next_pc", expected_pc, actual_pc);
        pass = false;
    }

    // General registers
    for (int i = 0; i < 16; i++) {
        if (expected->r[i] != actual->r[i]) {
            snprintf(field, sizeof(field), "r%d", i);
            log_diff(block_addr, field, expected->r[i], actual->r[i]);
            pass = false;
        }
    }

    // SR flags
    if (expected->sr.T != actual->sr.T) { log_diff(block_addr, "sr.T", expected->sr.T, actual->sr.T); pass = false; }
    if (expected->sr.S != actual->sr.S) { log_diff(block_addr, "sr.S", expected->sr.S, actual->sr.S); pass = false; }
    if (expected->sr.Q != actual->sr.Q) { log_diff(block_addr, "sr.Q", expected->sr.Q, actual->sr.Q); pass = false; }
    if (expected->sr.M != actual->sr.M) { log_diff(block_addr, "sr.M", expected->sr.M, actual->sr.M); pass = false; }
    if (expected->sr.IMASK != actual->sr.IMASK) { log_diff(block_addr, "sr.IMASK", expected->sr.IMASK, actual->sr.IMASK); pass = false; }

    // Special registers
    if (expected->pr != actual->pr) { log_diff(block_addr, "pr", expected->pr, actual->pr); pass = false; }
    if (expected->mac.full != actual->mac.full) {
        log_diff(block_addr, "mac.h", (u32)(expected->mac.full >> 32), (u32)(actual->mac.full >> 32));
        log_diff(block_addr, "mac.l", (u32)expected->mac.full, (u32)actual->mac.full);
        pass = false;
    }
    if (expected->gbr != actual->gbr) { log_diff(block_addr, "gbr", expected->gbr, actual->gbr); pass = false; }
    if (expected->vbr != actual->vbr) { log_diff(block_addr, "vbr", expected->vbr, actual->vbr); pass = false; }
    if (expected->fpscr.full != actual->fpscr.full) { log_diff(block_addr, "fpscr", expected->fpscr.full, actual->fpscr.full); pass = false; }
    if (expected->fpul != actual->fpul) { log_diff(block_addr, "fpul", expected->fpul, actual->fpul); pass = false; }

    // Floating point registers
    for (int i = 0; i < 16; i++) {
        if (memcmp(&expected->fr[i], &actual->fr[i], 4) != 0) {
            snprintf(field, sizeof(field), "fr%d", i);
            log_diff_float(block_addr, field, expected->fr[i], actual->fr[i]);
            pass = false;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (memcmp(&expected->xf[i], &actual->xf[i], 4) != 0) {
            snprintf(field, sizeof(field), "xf%d", i);
            log_diff_float(block_addr, field, expected->xf[i], actual->xf[i]);
            pass = false;
        }
    }

    return pass;
}

// Called from compilePC after a block is decoded but before JIT compilation
// rbi contains the block info (vaddr, sh4_size, etc.)
void sh4recomp_verify_block(u32 vaddr, u32 sh4_size) {
    verify_init();

    // Normalize address
    u32 lookup_pc = vaddr;
    if ((lookup_pc >> 24) == 0x8C || (lookup_pc >> 24) == 0xAC)
        lookup_pc = (lookup_pc & 0x00FFFFFF) | 0x0C000000;

    // Only verify game code blocks
    if (lookup_pc < 0x0C020000) return;

    // Do we have a static block for this address?
    RecompBlockFunc block = recomp_find_block(lookup_pc);
    if (!block) return;

    g_total_verified++;

    // 1. Snapshot state BEFORE
    Sh4Context before;
    memcpy(&before, &Sh4cntx, sizeof(before));

    // 2. Run interpreter for this block (the correct result)
    u32 expected_next_pc = interp_exec_block(vaddr, sh4_size);
    Sh4Context expected;
    memcpy(&expected, &Sh4cntx, sizeof(expected));

    // 3. Restore BEFORE state
    memcpy(&Sh4cntx, &before, sizeof(Sh4cntx));

    // 4. Run our static block
    u32 actual_next_pc = block(nullptr, &Sh4cntx);
    Sh4Context actual;
    memcpy(&actual, &Sh4cntx, sizeof(actual));

    // 5. Compare
    bool pass = verify_context(&expected, &actual, lookup_pc, expected_next_pc, actual_next_pc);

    if (pass) {
        g_total_passed++;
        log_pass(lookup_pc);
    } else {
        g_total_failed++;
    }

    // 6. Restore correct state so JIT compilation uses correct values
    memcpy(&Sh4cntx, &before, sizeof(Sh4cntx));

    // Periodic summary
    if (g_total_verified <= 10 || (g_total_verified % 100) == 0) {
        printf("[sh4recomp-verify] %lu verified: %lu PASS, %lu FAIL (block 0x%08X %s)\n",
            g_total_verified, g_total_passed, g_total_failed,
            lookup_pc, pass ? "OK" : "FAIL");
        fflush(stdout);
    }
}

// Bulk verify: test ALL blocks using current memory state
void sh4recomp_verify_all() {
    verify_init();
    printf("[sh4recomp-verify] === BULK VERIFICATION START ===\n");
    fflush(stdout);

    // Iterate the block table and verify each one
    extern "C" {
        typedef struct { u32 addr; RecompBlockFunc func; } BlockEntry;
        extern const BlockEntry block_table[];
        extern const int NUM_BLOCKS_COUNT;  // we'll add this
    }

    // Since we can't easily get NUM_BLOCKS from the dispatch table,
    // iterate by calling recomp_find_block for addresses in the trace
    // Read the block trace CSV
    FILE* trace = fopen("/home/tris/projects/sh4recomp/extracted/block_trace.csv", "r");
    if (!trace) {
        printf("[sh4recomp-verify] Can't open block trace\n");
        return;
    }

    char line[256];
    fgets(line, sizeof(line), trace); // skip header

    std::set<u32> tested;
    int pass = 0, fail = 0, skip = 0;

    while (fgets(line, sizeof(line), trace)) {
        u32 vaddr, addr, sh4_size, guest_ops, block_type, bt, nt;
        if (sscanf(line, "0x%x,0x%x,%u,%u,%d,0x%x,0x%x",
                   &vaddr, &addr, &sh4_size, &guest_ops, &block_type, &bt, &nt) < 5)
            continue;

        u32 lookup_pc = vaddr;
        if ((lookup_pc >> 24) == 0x8C || (lookup_pc >> 24) == 0xAC)
            lookup_pc = (lookup_pc & 0x00FFFFFF) | 0x0C000000;

        if (lookup_pc < 0x0C020000) continue;
        if (tested.count(lookup_pc)) continue;
        tested.insert(lookup_pc);

        RecompBlockFunc block = recomp_find_block(lookup_pc);
        if (!block) { skip++; continue; }

        // Snapshot
        Sh4Context before;
        memcpy(&before, &Sh4cntx, sizeof(before));

        // Run interpreter
        Sh4cntx.pc = vaddr;
        u32 expected_pc = interp_exec_block(vaddr, sh4_size);
        Sh4Context expected;
        memcpy(&expected, &Sh4cntx, sizeof(expected));

        // Restore & run our block
        memcpy(&Sh4cntx, &before, sizeof(Sh4cntx));
        u32 actual_pc = block(nullptr, &Sh4cntx);
        Sh4Context actual;
        memcpy(&actual, &Sh4cntx, sizeof(actual));

        // Compare
        bool ok = verify_context(&expected, &actual, lookup_pc, expected_pc, actual_pc);
        if (ok) {
            pass++;
            log_pass(lookup_pc);
        } else {
            fail++;
        }

        // Restore original state
        memcpy(&Sh4cntx, &before, sizeof(Sh4cntx));

        if ((pass + fail) % 1000 == 0) {
            printf("[sh4recomp-verify] Progress: %d pass, %d fail, %d skip\n", pass, fail, skip);
            fflush(stdout);
        }
    }
    fclose(trace);

    printf("[sh4recomp-verify] === BULK RESULT: %d PASS, %d FAIL, %d SKIP ===\n", pass, fail, skip);
    fflush(stdout);
}

void sh4recomp_verify_shutdown() {
    if (g_verify_log) {
        printf("[sh4recomp-verify] FINAL: %lu verified, %lu passed, %lu failed\n",
            g_total_verified, g_total_passed, g_total_failed);
        fclose(g_verify_log);
        g_verify_log = nullptr;
    }
}

#endif // SH4RECOMP_VERIFY
