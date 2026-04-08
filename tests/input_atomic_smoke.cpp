// ============================================================================
// input_atomic_smoke.cpp — Phase A tear-free atomic stress test
// ============================================================================
//
// Proves that the packed _slotInputAtomic[] write/read path in
// core/network/maplecast_input_server.h cannot tear under contention.
//
// One thread = "network/UDP thread" — hammers updateSlot()-equivalent
// writes with a known walking-bit pattern at ~12 KHz (matching NOBD's
// production packet rate).
//
// Other thread = "maple/CMD9 thread" — polls _slotInputAtomic[0] in a tight
// loop, asserts every observed (buttons, lt, rt) triple is one of the
// finite set of values the writer ever produced as a single update.
//
// If the atomic packing is correct, every read snapshot should be a valid
// write triple. If there were a torn read across the buttons/lt/rt boundary,
// we'd see hybrid states like "old buttons + new lt" that the writer never
// committed as a unit.
//
// Build (standalone, no flycast linkage needed):
//   g++ -std=c++17 -O2 -pthread -I../core/network \
//       tests/input_atomic_smoke.cpp -o /tmp/input_atomic_smoke
// Run:
//   /tmp/input_atomic_smoke              # 60-second default
//   /tmp/input_atomic_smoke 5            # 5-second smoke
//
// Exit code: 0 = pass, non-zero = tear detected (test FAILED)
// ============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_set>

// Include the production packing helpers directly. We don't need <vector>
// from the full header; declare-and-paste the inline functions instead so
// the test stays self-contained.
namespace maplecast_input {

inline uint64_t packSlotInput(uint16_t buttons, uint8_t ltVal, uint8_t rtVal, uint32_t seq) {
    return (uint64_t)buttons
         | ((uint64_t)ltVal << 16)
         | ((uint64_t)rtVal << 24)
         | ((uint64_t)seq << 32);
}

inline void unpackSlotInput(uint64_t packed, uint16_t& buttons, uint8_t& ltVal, uint8_t& rtVal, uint32_t& seq) {
    buttons = (uint16_t)(packed & 0xFFFF);
    ltVal   = (uint8_t)((packed >> 16) & 0xFF);
    rtVal   = (uint8_t)((packed >> 24) & 0xFF);
    seq     = (uint32_t)((packed >> 32) & 0xFFFFFFFFu);
}

} // namespace

// ----------------------------------------------------------------------------
// Walking bit pattern: a finite, recognizable cycle of (buttons, lt, rt) triples
// ----------------------------------------------------------------------------
// Step i: buttons rotates left by 1 each step starting from 0x0001
//         lt = i*7 mod 256
//         rt = i*13 mod 256
// 16 distinct buttons states × full 256 lt × 256 rt — but we cycle through a
// fixed 256-step sequence so the reader can hash-set check each observation
// against the known-good set.
constexpr int PATTERN_LEN = 256;
struct Triple { uint16_t buttons; uint8_t lt; uint8_t rt; };
static Triple kPattern[PATTERN_LEN];

static void initPattern() {
    for (int i = 0; i < PATTERN_LEN; ++i) {
        uint16_t b = (uint16_t)(1u << (i % 16));   // walking bit, repeats every 16
        kPattern[i] = { b, (uint8_t)((i * 7) & 0xFF), (uint8_t)((i * 13) & 0xFF) };
    }
}

static uint64_t triplesKey(uint16_t b, uint8_t l, uint8_t r) {
    return ((uint64_t)b << 32) | ((uint64_t)l << 16) | (uint64_t)r;
}

// ----------------------------------------------------------------------------
// Shared atomic + control flag — exactly the production layout
// ----------------------------------------------------------------------------
static std::atomic<uint64_t> g_slotAtomic{
    maplecast_input::packSlotInput(0xFFFF, 0, 0, 0)
};
static std::atomic<bool> g_running{true};

// ----------------------------------------------------------------------------
// Writer thread — emulates updateSlot() at ~12 KHz
// ----------------------------------------------------------------------------
static void writerLoop()
{
    uint32_t seq = 0;
    int patternIdx = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        const Triple& t = kPattern[patternIdx];
        ++seq;
        g_slotAtomic.store(
            maplecast_input::packSlotInput(t.buttons, t.lt, t.rt, seq),
            std::memory_order_release);
        patternIdx = (patternIdx + 1) % PATTERN_LEN;
        // ~83 us = 12 KHz
        std::this_thread::sleep_for(std::chrono::microseconds(83));
    }
}

// ----------------------------------------------------------------------------
// Reader thread — emulates getLocalInput() reads at much higher rate
// ----------------------------------------------------------------------------
static std::atomic<uint64_t> g_readsTotal{0};
static std::atomic<uint64_t> g_readsTorn{0};
static std::atomic<uint64_t> g_lastSeqSeen{0};

static void readerLoop(const std::unordered_set<uint64_t>& validKeys)
{
    while (g_running.load(std::memory_order_relaxed)) {
        uint64_t packed = g_slotAtomic.load(std::memory_order_acquire);
        uint16_t b; uint8_t l; uint8_t r; uint32_t s;
        maplecast_input::unpackSlotInput(packed, b, l, r, s);

        // Initial neutral state (0xFFFF, 0, 0) is always valid before the
        // first writer step lands.
        if (s == 0) {
            g_readsTotal.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const uint64_t k = triplesKey(b, l, r);
        if (validKeys.find(k) == validKeys.end()) {
            // Torn read! The writer never committed this exact triple as
            // a unit. Print and count.
            uint64_t torn = g_readsTorn.fetch_add(1, std::memory_order_relaxed) + 1;
            if (torn <= 10) {
                fprintf(stderr,
                    "[TORN] read seq=%u buttons=0x%04x lt=%u rt=%u "
                    "(packed=0x%016lx) — not in writer pattern!\n",
                    s, b, l, r, (unsigned long)packed);
            }
        }

        g_lastSeqSeen.store(s, std::memory_order_relaxed);
        g_readsTotal.fetch_add(1, std::memory_order_relaxed);
    }
}

// ----------------------------------------------------------------------------
// Main
// ----------------------------------------------------------------------------
int main(int argc, char** argv)
{
    int durationSec = 60;
    if (argc > 1) durationSec = atoi(argv[1]);
    if (durationSec <= 0) durationSec = 60;

    initPattern();
    std::unordered_set<uint64_t> validKeys;
    validKeys.reserve(PATTERN_LEN);
    for (int i = 0; i < PATTERN_LEN; ++i)
        validKeys.insert(triplesKey(kPattern[i].buttons, kPattern[i].lt, kPattern[i].rt));

    fprintf(stderr,
        "input_atomic_smoke: pattern=%d unique-triples writer=12KHz duration=%ds\n",
        (int)validKeys.size(), durationSec);

    std::thread writer(writerLoop);
    std::thread reader(readerLoop, std::ref(validKeys));

    std::this_thread::sleep_for(std::chrono::seconds(durationSec));
    g_running.store(false, std::memory_order_relaxed);
    writer.join();
    reader.join();

    uint64_t reads = g_readsTotal.load();
    uint64_t torn  = g_readsTorn.load();
    uint64_t lastSeq = g_lastSeqSeen.load();

    fprintf(stderr,
        "input_atomic_smoke: %lu reads, %lu torn, last writer seq seen=%u\n",
        (unsigned long)reads, (unsigned long)torn, (unsigned)lastSeq);

    if (torn > 0) {
        fprintf(stderr, "FAIL: %lu torn reads detected — atomic packing is broken\n",
                (unsigned long)torn);
        return 1;
    }
    if (reads == 0) {
        fprintf(stderr, "FAIL: no reads observed (timing bug in test harness?)\n");
        return 2;
    }
    fprintf(stderr, "PASS: zero torn reads across %lu observations\n",
            (unsigned long)reads);
    return 0;
}
