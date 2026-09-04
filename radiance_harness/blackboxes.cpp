// Blackbox implementations + boot harness for running the radiance RadianceGsimConfig under GSIM.
//
// GSIM emits each Verilog blackbox as a plain C++ function call (once per cycle, scheduled as a
// registered source by the AST2Graph clocked-ext fix). We provide functional models:
//   SimDRAM  -> testchipip mm_magic_t (AXI4 magic memory), backing array preloaded with the ELF.
//   SimTSI   -> testchip_tsi_t (fesvr) : boot handshake + htif tohost/fromhost exit; loadmem writes
//               the ELF straight into the shared DRAM backing array.
//   SimUART  -> console: putchar on the serial.out fire.
//   IOcells / EICG / plusarg / SimJTAG / TraceSinkMonitor / ProfilerBlackBox -> passthrough/stubs.
//
// Note the GSIM longint->int param truncation: SimDRAM's mem_size arrives as 0, so we supply our own
// 4 GB backing (base 0x8000_0000) from the constants below.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/mman.h>

#include "mm.h"
#include <fesvr/elfloader.h>
#include <fesvr/memif.h>
#include "testchip_tsi.h"

typedef unsigned _BitInt(256) u256;
typedef unsigned _BitInt(512) u512;

// ---- shared DRAM backing ---------------------------------------------------------------------
static const uint64_t MEM_BASE = 0x80000000ULL;
static const uint64_t MEM_SIZE = 0x100000000ULL; // 4 GB
static backing_data_t g_dram = {nullptr, 0};

static void ensure_dram() {
  if (g_dram.data) return;
  void* p = mmap(nullptr, MEM_SIZE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) { perror("mmap dram"); abort(); }
  g_dram.data = (uint8_t*)p;
  g_dram.size = MEM_SIZE;
}

extern "C" bool dram_peek(uint64_t phys, void* buf, unsigned long n) {
  if (!g_dram.data || phys < MEM_BASE || phys + n > MEM_BASE + g_dram.size) return false;
  memcpy(buf, g_dram.data + (phys - MEM_BASE), n);
  return true;
}

// ---- args passed from main (for fesvr) -------------------------------------------------------
static int    g_argc = 0;
static char** g_argv = nullptr;
volatile bool g_tsi_done = false;
int           g_exit_code = 0;
extern "C" void harness_set_args(int argc, char** argv) { g_argc = argc; g_argv = argv; }

// ---- instrumentation counters ----------------------------------------------------------------
extern "C" {
unsigned long long g_dram_aw_fires = 0, g_dram_ar_fires = 0, g_dram_w_fires = 0;
unsigned long long g_dram_r_fires = 0, g_dram_b_fires = 0;
unsigned long long g_dram_writes_simt = 0;   // writes with addr >= 0x110000000
unsigned long long g_dram_writes_result = 0; // writes into T_matmul_0 page (0x110001000..0x110002000)
uint64_t g_dram_last_aw = 0, g_dram_last_ar = 0, g_dram_max_ar = 0;
unsigned long long g_cvfpu_accepts = 0;      // set from cvfpu_model.cpp
unsigned long long g_uart_chars = 0;
}
static void harness_print_stats() {
  fprintf(stderr,
    "[gsim-stats] dram_aw=%llu dram_w=%llu dram_ar=%llu writes>=0x110000000=%llu writes_resultpage=%llu\n"
    "[gsim-stats] last_aw=0x%llx last_ar=0x%llx max_ar=0x%llx r_fires=%llu b_fires=%llu cvfpu_accepts=%llu uart_chars=%llu\n",
    g_dram_aw_fires, g_dram_w_fires, g_dram_ar_fires, g_dram_writes_simt, g_dram_writes_result,
    (unsigned long long)g_dram_last_aw, (unsigned long long)g_dram_last_ar,
    (unsigned long long)g_dram_max_ar, g_dram_r_fires, g_dram_b_fires, g_cvfpu_accepts, g_uart_chars);
}

// ==============================================================================================
// SimDRAM  (params _0.._7 alphabetical: ADDR_BITS,CHIP_ID,CLOCK_HZ,DATA_BITS,ID_BITS,LINE_SIZE,
//           MEM_BASE,MEM_SIZE ; MEM_SIZE truncated to 0 by GSIM -> we use our MEM_SIZE constant)
// ==============================================================================================
void SimDRAM(int, int, int, int /*data_bits*/, int, int, int, int,
             uint8_t reset,
             uint8_t& aw_ready, uint8_t aw_valid, uint8_t aw_id, uint64_t aw_addr,
             uint8_t aw_len, uint8_t aw_size, uint8_t aw_burst, uint8_t aw_lock,
             uint8_t aw_cache, uint8_t aw_prot, uint8_t aw_qos,
             uint8_t& w_ready, uint8_t w_valid, uint64_t w_data, uint8_t w_strb, uint8_t w_last,
             uint8_t b_ready, uint8_t& b_valid, uint8_t& b_id, uint8_t& b_resp,
             uint8_t& ar_ready, uint8_t ar_valid, uint8_t ar_id, uint64_t ar_addr,
             uint8_t ar_len, uint8_t ar_size, uint8_t ar_burst, uint8_t ar_lock,
             uint8_t ar_cache, uint8_t ar_prot, uint8_t ar_qos,
             uint8_t r_ready, uint8_t& r_valid, uint8_t& r_id, uint64_t& r_data,
             uint8_t& r_resp, uint8_t& r_last) {
  static mm_magic_t* mm = nullptr;
  ensure_dram();
  if (!mm) { mm = new mm_magic_t(MEM_BASE, MEM_SIZE, /*word*/ 8, /*line*/ 64, g_dram);
             atexit(harness_print_stats); }

  // instrumentation: count channel fires (valid && ready) using the *previous* cycle's ready
  if (aw_valid && aw_ready) { g_dram_aw_fires++; g_dram_last_aw = aw_addr;
    if (aw_addr >= 0x110000000ULL) g_dram_writes_simt++;
    if (aw_addr >= 0x110001000ULL && aw_addr < 0x110002000ULL) g_dram_writes_result++; }
  if (w_valid && w_ready) g_dram_w_fires++;
  if (ar_valid && ar_ready) { g_dram_ar_fires++; g_dram_last_ar = ar_addr;
    if (ar_addr > g_dram_max_ar) g_dram_max_ar = ar_addr;
    static int nlog = 0; if (nlog < 40) { fprintf(stderr, "[gsim-ar] #%d addr=0x%llx\n", nlog, (unsigned long long)ar_addr); nlog++; } }
  static unsigned long long r_fires = 0, b_fires = 0;
  extern unsigned long long g_dram_r_fires, g_dram_b_fires;
  if (r_valid && r_ready) g_dram_r_fires++;
  if (b_valid && b_ready) g_dram_b_fires++;
  (void)r_fires;(void)b_fires;

  // --- DRAM channel trace (first N cycles): shows request/response handshake at the memory ---
  if (getenv("DRAM_TRACE")) {
    static unsigned long long tcyc = 0;
    unsigned long long lim = strtoull(getenv("DRAM_TRACE"), nullptr, 0);
    if (tcyc < lim) {
      if (ar_valid || aw_valid || w_valid || r_valid || b_valid || ar_ready || r_ready)
        fprintf(stderr, "[dram t=%llu] AR(v%u r%u a=0x%llx id%u len%u) W(v%u r%u last%u) R(v%u r%u last%u id%u) AW(v%u r%u a=0x%llx) B(v%u r%u) rst%u\n",
          tcyc, ar_valid, ar_ready, (unsigned long long)ar_addr, ar_id, ar_len,
          w_valid, w_ready, w_last, r_valid, r_ready, r_last, r_id,
          aw_valid, aw_ready, (unsigned long long)aw_addr, b_valid, b_ready, reset);
    }
    tcyc++;
  }

  uint64_t wd = w_data;
  mm->tick(reset,
           ar_valid, ar_addr, ar_id, ar_size, ar_len,
           aw_valid, aw_addr, aw_id, aw_size, aw_len,
           w_valid, w_strb, &wd, w_last,
           r_ready, b_ready);
  (void)aw_burst;(void)aw_lock;(void)aw_cache;(void)aw_prot;(void)aw_qos;
  (void)ar_burst;(void)ar_lock;(void)ar_cache;(void)ar_prot;(void)ar_qos;

  aw_ready = mm->aw_ready();
  w_ready  = mm->w_ready();
  ar_ready = mm->ar_ready();
  b_valid  = mm->b_valid(); b_id = mm->b_id(); b_resp = mm->b_resp();
  r_valid  = mm->r_valid(); r_id = mm->r_id(); r_resp = mm->r_resp(); r_last = mm->r_last();
  r_data   = r_valid ? *(uint64_t*)mm->r_data() : 0;
}

// ==============================================================================================
// SimTSI  (fesvr testchip_tsi ; loadmem writes ELF into g_dram)
// ==============================================================================================
namespace {
class GsimTSI : public testchip_tsi_t {
 public:
  GsimTSI(int argc, char** argv) : testchip_tsi_t(argc, argv, /*has_loadmem*/ true) {}
 protected:
  void load_mem_write(addr_t addr, size_t nbytes, const void* src) override {
    if (addr >= MEM_BASE && addr + nbytes <= MEM_BASE + g_dram.size)
      memcpy(g_dram.data + (addr - MEM_BASE), src, nbytes);
  }
  void load_mem_read(addr_t addr, size_t nbytes, void* dst) override {
    if (addr >= MEM_BASE && addr + nbytes <= MEM_BASE + g_dram.size)
      memcpy(dst, g_dram.data + (addr - MEM_BASE), nbytes);
    else memset(dst, 0, nbytes);
  }
};
} // namespace

void SimTSI(int /*chip_id*/, uint8_t reset,
            uint8_t in_ready, uint8_t& in_valid, uint32_t& in_bits,
            uint8_t& out_ready, uint8_t out_valid, uint32_t out_bits, uint32_t& exit_code) {
  static GsimTSI* tsi = nullptr;
  if (reset) { in_valid = 0; in_bits = 0; out_ready = 0; exit_code = 0; return; }
  ensure_dram();
  if (!tsi) tsi = new GsimTSI(g_argc, g_argv);

  tsi->tick(out_valid, out_bits, in_ready);
  tsi->switch_to_host();
  in_valid  = tsi->in_valid();
  in_bits   = tsi->in_bits();
  out_ready = tsi->out_ready();
  if (tsi->done()) { g_tsi_done = true; g_exit_code = tsi->exit_code();
                     exit_code = (uint32_t)((tsi->exit_code() << 1) | 1); }
  else exit_code = 0;
}

// ==============================================================================================
// SimUART -> console
// ==============================================================================================
void SimUART(int, int, uint8_t /*reset*/,
             uint8_t /*in_ready*/, uint8_t& in_valid, uint8_t& in_bits,
             uint8_t& out_ready, uint8_t out_valid, uint8_t out_bits) {
  out_ready = 1; in_valid = 0; in_bits = 0;
  if (out_valid) { putchar((char)out_bits); fflush(stdout); g_uart_chars++; }
}

// ==============================================================================================
// Trivial cells / stubs
// ==============================================================================================
void GenericDigitalInIOCell (uint8_t pad, uint8_t& o, uint8_t /*ie*/) { o = pad; }
void GenericDigitalOutIOCell(uint8_t& pad, uint8_t i, uint8_t /*oe*/) { pad = i; }
void EICG_wrapper(uint8_t, uint8_t) { }
void TraceSinkMonitor(const char*, uint8_t, uint8_t, uint8_t) { }
void ProfilerBlackBox(int,int,int,int,uint8_t,uint8_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,
                      uint64_t,u512,u512,u512,u512,u512,u512,u512,u512,u512,u512) { }

void SimJTAG(int, uint8_t, uint8_t& trstn, uint8_t& tms, uint8_t& tdi,
             uint8_t, uint8_t, uint8_t, uint8_t, uint32_t& ex) { trstn = 1; tms = 0; tdi = 0; ex = 0; }

// plusarg_reader: return the plusarg value if present on the command line, else the default.
// The RTL passes the plusarg NAME as a printf-style format ("max_core_cycles=%d"); the real plusarg
// key is the part before the '='/'%' ("max_core_cycles"), which is what appears on the command line
// as "+max_core_cycles=<value>". Strip the format tail so the match works (without it a "+name=0"
// like the PlusArgTimeout disable never matched and the default fired).
static uint32_t plusarg_value(const char* name, int def) {
  uint32_t out = (uint32_t)def;
  if (!name || !g_argv) return out;
  std::string base = name;
  size_t cut = base.find_first_of("=%");
  if (cut != std::string::npos) base = base.substr(0, cut);
  std::string key = std::string("+") + base + "=";
  for (int i = 1; i < g_argc; i++) {
    std::string a = g_argv[i];
    if (a.rfind(key, 0) == 0) return (uint32_t)strtoul(a.c_str() + key.size(), nullptr, 0);
  }
  return out;
}
void plusarg_reader(int, const char* name, int def, uint32_t& out) { out = plusarg_value(name, def); }
void plusarg_reader(int, const char* name, int def, uint8_t&  out) { out = (uint8_t)plusarg_value(name, def); }

// Blocker-B (dangling gemmini tile_3/6 im2col temps) is fixed by prepending zero definitions to the
// single failing TestHarness1.cpp (see build script) — im2col is inactive for GEMM so 0 is safe.
