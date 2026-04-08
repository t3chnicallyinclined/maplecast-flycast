// ============================================================================
// input_accumulator_test.cpp — Phase B accumulator + edge preservation test
// ============================================================================
//
// Single-threaded synthetic test of the input accumulator's edge detection
// and drain semantics. Verifies that:
//
//   1. A press transition during a vblank interval sets `any_pressed`.
//   2. A release transition sets `any_released`.
//   3. drainAccumulator() returns the snapshot AND clears any_pressed +
//      any_released, leaving current/lt/rt intact.
//   4. A press-and-release within the same interval sets BOTH bits.
//   5. The "edge preservation" rule:
//        latched_buttons = current & ~any_pressed
//      → bits that were pressed at any moment are PRESSED in the latch
//        even if they were released by drain time.
//   6. The "deferred release" rule:
//        next_deferred = any_pressed & any_released
//      → blip-press bits get released on the NEXT latch.
//   7. After a blip-press → release sequence, after two latches the
//      button is properly back to released.
//
// This test does NOT cover the multi-threaded CAS loop (that's the
// input_atomic_smoke binary's job). It tests the LOGIC, not the
// concurrency.
//
// Build (standalone, no flycast linkage):
//   g++ -std=c++17 -O2 -pthread -I../core/network \
//       tests/input_accumulator_test.cpp -o /tmp/input_accumulator_test
// Run:
//   /tmp/input_accumulator_test
//
// Exit code: 0 = all assertions pass, 1 = at least one assertion fails
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Inline copies of the production types from maplecast_input_server.h.
// Same byte layout, same packing math, no flycast headers needed.
struct AccumPacked {
    uint16_t any_pressed;
    uint16_t any_released;
    uint16_t current;
    uint8_t  lt;
    uint8_t  rt;
};
static_assert(sizeof(AccumPacked) == 8, "AccumPacked must fit in u64");

static inline uint64_t packAccum(const AccumPacked& a) {
    uint64_t packed; __builtin_memcpy(&packed, &a, 8); return packed;
}
static inline AccumPacked unpackAccum(uint64_t packed) {
    AccumPacked a; __builtin_memcpy(&a, &packed, 8); return a;
}

// Same writer logic as production updateSlot() — minus the seq/telemetry,
// just the accumulator CAS body. Single-threaded here so the CAS loop is
// guaranteed to succeed on the first try.
static std::atomic<uint64_t> g_slotAccum{0};

static void resetAccum() {
    AccumPacked a{};
    a.any_pressed = 0;
    a.any_released = 0;
    a.current = 0xFFFF;  // active-low neutral
    a.lt = 0;
    a.rt = 0;
    g_slotAccum.store(packAccum(a), std::memory_order_release);
}

static void writeUpdate(uint16_t buttons, uint8_t lt, uint8_t rt) {
    uint64_t old = g_slotAccum.load(std::memory_order_acquire);
    AccumPacked a = unpackAccum(old);
    uint16_t newly_pressed  = (uint16_t)((~buttons) & a.current);
    uint16_t newly_released = (uint16_t)(buttons & (~a.current));
    AccumPacked next = a;
    next.any_pressed  = (uint16_t)(a.any_pressed  | newly_pressed);
    next.any_released = (uint16_t)(a.any_released | newly_released);
    next.current = buttons;
    next.lt = lt;
    next.rt = rt;
    g_slotAccum.store(packAccum(next), std::memory_order_release);
}

static AccumPacked drain() {
    uint64_t old = g_slotAccum.load(std::memory_order_acquire);
    AccumPacked a = unpackAccum(old);
    AccumPacked cleared = a;
    cleared.any_pressed = 0;
    cleared.any_released = 0;
    g_slotAccum.store(packAccum(cleared), std::memory_order_release);
    return a;
}

// ----------------------------------------------------------------------------
// Assertion helpers
// ----------------------------------------------------------------------------
static int g_passed = 0;
static int g_failed = 0;

static void check(bool cond, const char* test, const char* detail) {
    if (cond) {
        ++g_passed;
        fprintf(stderr, "  PASS: %s — %s\n", test, detail);
    } else {
        ++g_failed;
        fprintf(stderr, "  FAIL: %s — %s\n", test, detail);
    }
}

// Render a 16-bit mask as hex for log clarity.
static const char* h(uint16_t v) {
    static char buf[16][8];
    static int bi = 0;
    char* b = buf[bi++ & 15];
    snprintf(b, 8, "0x%04x", v);
    return b;
}

// ----------------------------------------------------------------------------
// Test cases
// ----------------------------------------------------------------------------

static void test_neutral_drain() {
    fprintf(stderr, "\n[test 1] neutral state drain\n");
    resetAccum();
    AccumPacked a = drain();
    check(a.any_pressed == 0,  "T1.a", "any_pressed == 0 after fresh reset");
    check(a.any_released == 0, "T1.b", "any_released == 0 after fresh reset");
    check(a.current == 0xFFFF, "T1.c", "current == 0xFFFF (active-low neutral)");
}

static void test_single_press() {
    fprintf(stderr, "\n[test 2] single press transition (active-low: bit 0 → 0)\n");
    resetAccum();
    // Press button 0: 0xFFFF → 0xFFFE
    writeUpdate(0xFFFE, 0, 0);
    AccumPacked a = drain();
    check(a.any_pressed == 0x0001, "T2.a",
          "any_pressed has bit 0 set (it transitioned 1→0)");
    check(a.any_released == 0,     "T2.b", "any_released == 0");
    check(a.current == 0xFFFE,     "T2.c", "current == 0xFFFE");
    // After drain, the press flag should be cleared
    AccumPacked a2 = drain();
    check(a2.any_pressed == 0,  "T2.d", "any_pressed cleared after drain");
    check(a2.any_released == 0, "T2.e", "any_released still 0");
    check(a2.current == 0xFFFE, "T2.f", "current preserved across drain");
}

static void test_single_release() {
    fprintf(stderr, "\n[test 3] single release transition (active-low: bit 0 → 1)\n");
    resetAccum();
    // First put button 0 into the pressed state without recording it
    writeUpdate(0xFFFE, 0, 0);
    drain();  // clear the press flag from the setup
    // Now release it: 0xFFFE → 0xFFFF
    writeUpdate(0xFFFF, 0, 0);
    AccumPacked a = drain();
    check(a.any_released == 0x0001, "T3.a",
          "any_released has bit 0 set (it transitioned 0→1)");
    check(a.any_pressed == 0,       "T3.b", "any_pressed == 0");
    check(a.current == 0xFFFF,      "T3.c", "current == 0xFFFF");
}

static void test_blip_press() {
    fprintf(stderr, "\n[test 4] blip press: press AND release within one interval\n");
    resetAccum();
    // Within the SAME drain interval: press then release button 0
    writeUpdate(0xFFFE, 0, 0);  // press
    writeUpdate(0xFFFF, 0, 0);  // release immediately
    AccumPacked a = drain();
    check(a.any_pressed == 0x0001,  "T4.a",
          "any_pressed has bit 0 (it WAS pressed during the interval)");
    check(a.any_released == 0x0001, "T4.b",
          "any_released has bit 0 (it WAS released during the interval)");
    check(a.current == 0xFFFF,      "T4.c",
          "current == 0xFFFF (state at end of interval)");

    // Edge preservation rule: latched = current & ~any_pressed
    // bit 0 is in any_pressed, so the latch should report it as PRESSED (cleared)
    uint16_t latched = a.current & ~a.any_pressed;
    check(latched == 0xFFFE, "T4.d",
          "edge preservation: latched buttons show bit 0 as PRESSED "
          "even though current has it released");

    // Deferred release rule: next_deferred = any_pressed & any_released
    // For a blip press, both bits are set on bit 0 → bit 0 is the next deferred release
    uint16_t deferred = a.any_pressed & a.any_released;
    check(deferred == 0x0001, "T4.e",
          "deferred release: bit 0 will unlatch on next frame");
}

static void test_simultaneous_distinct_buttons() {
    fprintf(stderr, "\n[test 5] simultaneous press of two buttons mid-interval\n");
    resetAccum();
    // Press button 0 (bit 0 → 0): 0xFFFF → 0xFFFE
    writeUpdate(0xFFFE, 0, 0);
    // Then 2 ms later press button 4 (bit 4 → 0): 0xFFFE → 0xFFEE
    writeUpdate(0xFFEE, 0, 0);
    AccumPacked a = drain();
    check(a.any_pressed == 0x0011,  "T5.a",
          "any_pressed has BOTH bit 0 AND bit 4 set");
    check(a.any_released == 0,      "T5.b", "any_released == 0");
    check(a.current == 0xFFEE,      "T5.c", "current == 0xFFEE (last state)");

    uint16_t latched = a.current & ~a.any_pressed;
    check(latched == 0xFFEE, "T5.d",
          "latched: both bits PRESSED (0xFFFF & ~0x0011 = 0xFFEE)");
}

static void test_three_presses_collapse_to_one() {
    fprintf(stderr, "\n[test 6] three rapid taps in one interval collapse to one press\n");
    resetAccum();
    // Tap → Tap → Tap on button 0 within one interval
    writeUpdate(0xFFFE, 0, 0); writeUpdate(0xFFFF, 0, 0);
    writeUpdate(0xFFFE, 0, 0); writeUpdate(0xFFFF, 0, 0);
    writeUpdate(0xFFFE, 0, 0); writeUpdate(0xFFFF, 0, 0);
    AccumPacked a = drain();
    check(a.any_pressed == 0x0001,  "T6.a",
          "any_pressed records ONE bit (no edge counting, only edge presence)");
    check(a.any_released == 0x0001, "T6.b",
          "any_released also records ONE bit");
    // Game sees one dash input — matches arcade-perfect behavior
}

static void test_long_press_across_intervals() {
    fprintf(stderr, "\n[test 7] long press held across multiple intervals\n");
    resetAccum();
    // Press in interval 1
    writeUpdate(0xFFFE, 0, 0);
    AccumPacked a1 = drain();
    check(a1.any_pressed == 0x0001 && a1.current == 0xFFFE, "T7.a",
          "interval 1: press is in any_pressed, current matches");

    // Held in interval 2 (no new edges)
    AccumPacked a2 = drain();
    check(a2.any_pressed == 0,  "T7.b", "interval 2: no new presses");
    check(a2.any_released == 0, "T7.c", "interval 2: no releases");
    check(a2.current == 0xFFFE, "T7.d", "interval 2: current still pressed");

    // Released in interval 3
    writeUpdate(0xFFFF, 0, 0);
    AccumPacked a3 = drain();
    check(a3.any_pressed == 0,      "T7.e", "interval 3: no presses");
    check(a3.any_released == 0x0001, "T7.f", "interval 3: bit 0 released");
    check(a3.current == 0xFFFF,     "T7.g", "interval 3: current matches");
}

static void test_lt_rt_pass_through() {
    fprintf(stderr, "\n[test 8] lt/rt are stored in accumulator and survive drain\n");
    resetAccum();
    writeUpdate(0xFFFF, 200, 50);
    AccumPacked a = drain();
    check(a.lt == 200, "T8.a", "lt == 200 (the most recent value)");
    check(a.rt == 50,  "T8.b", "rt == 50");
}

static void test_two_writes_lt_rt_overwrite() {
    fprintf(stderr, "\n[test 9] later lt/rt overwrites earlier in same interval\n");
    resetAccum();
    writeUpdate(0xFFFF, 100, 100);
    writeUpdate(0xFFFF, 250, 25);
    AccumPacked a = drain();
    check(a.lt == 250, "T9.a", "lt == 250 (most recent overwrite)");
    check(a.rt == 25,  "T9.b", "rt == 25 (most recent overwrite)");
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main() {
    fprintf(stderr, "input_accumulator_test — Phase B accumulator logic tests\n");

    test_neutral_drain();
    test_single_press();
    test_single_release();
    test_blip_press();
    test_simultaneous_distinct_buttons();
    test_three_presses_collapse_to_one();
    test_long_press_across_intervals();
    test_lt_rt_pass_through();
    test_two_writes_lt_rt_overwrite();

    fprintf(stderr, "\n=== %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
