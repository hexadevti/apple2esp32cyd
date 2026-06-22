// m0_bench.cpp - Apple IIGS feasibility gate (M0): PSRAM/SRAM memory-latency benchmark.
//
// Throwaway validation experiment. The ENTIRE file is wrapped in #ifdef IIGS_M0_BENCH so
// that, without the build flag, it adds nothing to the firmware (no 256KB static, no code).
//
// What it measures: a battery of access patterns (B1..B9) that bracket real-world behaviour,
// each reporting ns/access, host-cycles/access and M-accesses/sec. The MIXED pattern (B8)
// models the real 65C816 access mix (~50% SRAM hot banks + ~50% PSRAM with a working-set
// window, reads:writes ~= 4:1) and is the number the GO/NO-GO verdict is judged on. B9 runs
// the same mix THROUGH the actual read24/bankPtr dispatch so the table-lookup + I/O range
// check cost is folded into the end-to-end number.
//
// Verdict (judged on B8): GO if t_mix <= ~78 ns/access; see the plan file for the full model.
//
// Run from the top of setup() (before videoSetup/tasks/IRQs) so the render task, QSPI canvas
// flush and audio DMA don't perturb timing. Uses ESP.getCycleCount() (the project's timing
// idiom, see src/apple2/cpu.cpp) and min-of-7 runs to reject interrupt noise.

#ifdef IIGS_M0_BENCH

#include <Arduino.h>
#include "esp_heap_caps.h"
#include <string.h>

// ----------------------------------------------------------------------------- config
static const int N_RUNS = 7;          // runs per pattern; the MINIMUM delta is reported

// Region sizes are powers of two so indices mask with & (no modulo in the hot loop).
static const uint32_t SRAM_BYTES = 256u * 1024u;        // 4 hot banks ($00,$01,$E0,$E1)
static const uint32_t SRAM_MASK  = SRAM_BYTES - 1;
static const uint32_t PS_BYTES   = 4u * 1024u * 1024u;  // expansion RAM stand-in
static const uint32_t PS_MASK    = PS_BYTES - 1;

static const uint32_t WIN16      = 16u * 1024u;         // < 32KB D-cache -> cache-resident
static const uint32_t WIN16_MASK = WIN16 - 1;
static const uint32_t WIN64      = 64u * 1024u;         // 2x cache -> partially resident
static const uint32_t WIN64_MASK = WIN64 - 1;
static const uint32_t WIN_BASE   = 1u * 1024u * 1024u;  // window sits 1MB into the PSRAM block

// Iteration counts: enough for stable stats, small enough that each batch is well under the
// ESP.getCycleCount() 32-bit wrap (~17.9s @240MHz) even in the worst (slow PSRAM) case.
static const uint32_t N_FAST = 4u * 1000u * 1000u;      // SRAM / sequential / windowed (cheap)
static const uint32_t N_SLOW = 256u * 1024u;            // large random PSRAM: each access pays a
                                                        // full line-fill, so far fewer iters still
                                                        // give a rock-stable min over 7 runs

static float    NS_PER_CYC = 1000.0f / 240.0f;          // set from real CPU freq at startup
volatile uint32_t gSink = 0;                            // anti-DCE sink (printed at the end)

// 3-op xorshift32: a cheap deterministic PRNG (no Math.random, no call, no lock). Seed != 0.
static inline uint32_t xrnd(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

// ----------------------------------------------------------------------- batch inner loops
// Each returns a sink derived from every access so -O2 can't delete the loop. IRAM_ATTR keeps
// the loop body out of flash (no flash-cache stalls polluting the data-latency measurement).

static IRAM_ATTR uint32_t batchSeqRead(const uint8_t* p, uint32_t mask, uint32_t N) {
  uint32_t acc = 0, idx = 0;
  for (uint32_t i = 0; i < N; i++) { acc += p[idx & mask]; idx++; }
  return acc;
}
static IRAM_ATTR uint32_t batchRandRead(const uint8_t* p, uint32_t mask, uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed;
  for (uint32_t i = 0; i < N; i++) acc += p[xrnd(r) & mask];
  return acc;
}
static IRAM_ATTR uint32_t batchSeqWrite(uint8_t* p, uint32_t mask, uint32_t N) {
  uint32_t idx = 0;
  for (uint32_t i = 0; i < N; i++) { p[idx & mask] = (uint8_t)idx; idx++; }
  return p[N & mask];   // read one byte back so the writes are observable -> not elided
}
static IRAM_ATTR uint32_t batchRandWrite(uint8_t* p, uint32_t mask, uint32_t N, uint32_t seed) {
  uint32_t r = seed;
  for (uint32_t i = 0; i < N; i++) { uint32_t v = xrnd(r); p[v & mask] = (uint8_t)v; }
  return p[r & mask];
}
// MIXED (B8): the gate. ~50% SRAM / ~50% PSRAM-window, 1-in-5 writes (4:1 read:write).
static IRAM_ATTR uint32_t batchMixed(uint8_t* sram, uint32_t smask,
                                     uint8_t* psWin, uint32_t wmask, uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed, wc = 0;
  for (uint32_t i = 0; i < N; i++) {
    uint32_t v = xrnd(r);
    bool toSram  = v & 1;
    bool isWrite = (wc == 0); if (++wc == 5) wc = 0;
    uint32_t a = v >> 1;
    if (toSram) { if (isWrite) sram[a & smask]  = (uint8_t)v; else acc += sram[a & smask]; }
    else        { if (isWrite) psWin[a & wmask] = (uint8_t)v; else acc += psWin[a & wmask]; }
  }
  return acc;
}

// ------------------------------------------------ B9: the same mix through real read24/bankPtr
// Minimal, self-contained copy of the planned dispatch so B9 folds the bankPtr lookup + the
// $C000-$CFFF I/O range check into the measured cost. Banks 0..3 -> SRAM, banks 16..79 -> PSRAM.
static uint8_t* bm_bankPtr[256];
static inline bool bm_isIObank(uint8_t b) { return b == 0x00 || b == 0x01; } // I/O window banks
static IRAM_ATTR uint8_t bm_readIO(uint8_t b, uint16_t o)            { (void)b; (void)o; return 0; }
static IRAM_ATTR void    bm_writeIO(uint8_t b, uint16_t o, uint8_t v){ (void)b; (void)o; (void)v; }

static IRAM_ATTR uint8_t bm_read24(uint32_t a) {
  uint8_t bank = (uint8_t)(a >> 16); uint16_t off = (uint16_t)a;
  if ((off & 0xF000) == 0xC000 && bm_isIObank(bank)) return bm_readIO(bank, off);
  uint8_t* p = bm_bankPtr[bank];
  return p ? p[off] : 0;
}
static IRAM_ATTR void bm_write24(uint32_t a, uint8_t v) {
  uint8_t bank = (uint8_t)(a >> 16); uint16_t off = (uint16_t)a;
  if ((off & 0xF000) == 0xC000 && bm_isIObank(bank)) { bm_writeIO(bank, off, v); return; }
  uint8_t* p = bm_bankPtr[bank];
  if (p) p[off] = v;
}
static void bm_initBankPtr(uint8_t* sram, uint8_t* ps) {
  for (int b = 0; b < 256; b++) bm_bankPtr[b] = nullptr;
  for (int b = 0; b < 4;  b++) bm_bankPtr[b]      = sram + (size_t)b * 0x10000;  // 256KB, 4 banks
  for (int b = 0; b < 64; b++) bm_bankPtr[16 + b] = ps   + (size_t)b * 0x10000;  // 4MB, 64 banks
}
static IRAM_ATTR uint32_t batchMixed24(uint32_t N, uint32_t seed) {
  uint32_t acc = 0, r = seed, wc = 0;
  for (uint32_t i = 0; i < N; i++) {
    uint32_t v = xrnd(r);
    bool toSram  = v & 1;
    bool isWrite = (wc == 0); if (++wc == 5) wc = 0;
    uint32_t addr = toSram ? ((((v >> 1) & 3) << 16) | ((v >> 3) & 0xFFFF))   // banks 0..3 (SRAM)
                           : (((uint32_t)16 << 16)    | ((v >> 1) & 0xFFFF)); // bank 16 64K window
    if (isWrite) bm_write24(addr, (uint8_t)v); else acc += bm_read24(addr);
  }
  return acc;
}

// ------------------------------------------------------------------------------- reporting
static void report(const char* name, uint32_t* d, int n, uint32_t N) {
  for (int i = 1; i < n; i++) {                 // insertion sort (n=7)
    uint32_t k = d[i]; int j = i - 1;
    while (j >= 0 && d[j] > k) { d[j + 1] = d[j]; j--; }
    d[j + 1] = k;
  }
  uint32_t mn = d[0], md = d[n / 2], mx = d[n - 1];
  float ns     = (float)mn * NS_PER_CYC;        // total ns for the best (least-perturbed) run
  float nsPer  = ns / (float)N;
  float cycPer = (float)mn / (float)N;
  float macc   = (float)N / ns * 1000.0f;       // M-accesses/sec
  Serial.printf("M0 %-24s min=%-9lu med=%-9lu max=%-9lu cyc | %6.2f ns/acc | %5.2f cyc/acc | %7.2f Macc/s\n",
                name, (unsigned long)mn, (unsigned long)md, (unsigned long)mx, nsPer, cycPer, macc);
}

// One timed run + warm-up wrapper macros keep the per-pattern boilerplate small.
#define TIME_BATCH(EXPR) do {                                  \
    uint32_t s = ESP.getCycleCount();                          \
    uint32_t snk = (EXPR);                                     \
    uint32_t e = ESP.getCycleCount();                          \
    gSink ^= snk; d[r] = e - s;                                \
  } while (0)

static void showFree(const char* tag) {
  Serial.printf("free: internal=%u spiram=%u total8=%u  (%s)\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), tag);
}

// ---------------------------------------------------------------------------------- driver
void runIIgsM0Bench() {
  NS_PER_CYC = 1000.0f / (float)ESP.getCpuFreqMHz();
  Serial.println();
  Serial.println("=== IIGS M0 PSRAM/SRAM latency benchmark ===");
  Serial.printf("CPU %u MHz (%.3f ns/cyc), %d runs/pattern (min reported)\n",
                (unsigned)ESP.getCpuFreqMHz(), NS_PER_CYC, N_RUNS);

  showFree("before alloc");
  uint8_t* sram = (uint8_t*)heap_caps_malloc(SRAM_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  uint8_t* ps   = (uint8_t*)ps_malloc(PS_BYTES);
  if (!sram || !ps) {
    Serial.printf("ALLOC FAIL: sram=%p ps=%p (need %u internal + %u spiram) -- abort\n",
                  sram, ps, (unsigned)SRAM_BYTES, (unsigned)PS_BYTES);
    return;
  }
  showFree("after alloc");
  memset(sram, 0, SRAM_BYTES);   // commit pages so first-touch faults don't inflate B1/B3
  memset(ps,   0, PS_BYTES);

  uint8_t* psWindow = ps + WIN_BASE;
  uint32_t d[N_RUNS];

  Serial.println("--- baselines (internal SRAM) ---");
  batchSeqRead(sram, SRAM_MASK, N_FAST);                                      // warm-up
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchSeqRead(sram, SRAM_MASK, N_FAST));
  report("B1 SRAM seq read", d, N_RUNS, N_FAST);

  batchRandRead(sram, SRAM_MASK, N_FAST, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandRead(sram, SRAM_MASK, N_FAST, 0xC0DECAFE + r));
  report("B2 SRAM rand read", d, N_RUNS, N_FAST);

  Serial.println("--- PSRAM reads ---");
  batchSeqRead(ps, PS_MASK, N_FAST);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchSeqRead(ps, PS_MASK, N_FAST));
  report("B3 PSRAM seq read", d, N_RUNS, N_FAST);

  batchRandRead(ps, PS_MASK, N_SLOW, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandRead(ps, PS_MASK, N_SLOW, 0xC0DECAFE + r));
  report("B4 PSRAM rand 4MB read", d, N_RUNS, N_SLOW);

  batchRandRead(psWindow, WIN16_MASK, N_FAST, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandRead(psWindow, WIN16_MASK, N_FAST, 0xC0DECAFE + r));
  report("B5 PSRAM rand 16K win", d, N_RUNS, N_FAST);

  Serial.println("--- PSRAM writes ---");
  batchSeqWrite(ps, PS_MASK, N_FAST);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchSeqWrite(ps, PS_MASK, N_FAST));
  report("B6 PSRAM seq write", d, N_RUNS, N_FAST);

  batchRandWrite(ps, PS_MASK, N_SLOW, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchRandWrite(ps, PS_MASK, N_SLOW, 0xC0DECAFE + r));
  report("B7 PSRAM rand 4MB write", d, N_RUNS, N_SLOW);

  Serial.println("--- the gate ---");
  batchMixed(sram, SRAM_MASK, psWindow, WIN64_MASK, N_FAST, 1);
  for (int r = 0; r < N_RUNS; r++)
    TIME_BATCH(batchMixed(sram, SRAM_MASK, psWindow, WIN64_MASK, N_FAST, 0xC0DECAFE + r));
  report("B8 MIXED (gate)", d, N_RUNS, N_FAST);

  bm_initBankPtr(sram, ps);
  batchMixed24(N_SLOW, 1);
  for (int r = 0; r < N_RUNS; r++) TIME_BATCH(batchMixed24(N_SLOW, 0xC0DECAFE + r));
  report("B9 MIXED via read24", d, N_RUNS, N_SLOW);

  Serial.printf("M0 sink=%08x (anti-DCE proof)\n", (unsigned)gSink);
  Serial.println("Verdict: plug B8 ns/acc into the model in the plan file (GO if <= ~78 ns/acc).");
  Serial.println("=== M0 end ===");
}

#endif // IIGS_M0_BENCH
