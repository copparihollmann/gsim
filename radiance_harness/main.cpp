// Radiance RadianceGsimConfig boot harness main loop for the GSIM-generated STestHarness.
//
// Usage: emu <soc.elf> [+plusargs...]
//   The ELF is loaded into the SimDRAM backing array by the fesvr TSI (loadmem). We drive the
//   top-level clock/reset (GSIM step() == one full clock), hold reset for a few cycles, then run
//   until fesvr reports htif done() (tohost exit) or the cycle cap is reached.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>
#include "TestHarness.h"

extern "C" void harness_set_args(int argc, char** argv);
extern "C" bool dram_peek(uint64_t phys, void* buf, unsigned long n);
extern volatile bool g_tsi_done;
extern int g_exit_code;

static volatile bool g_model_finished = false;
// Compatibility hook for an explicitly patched emitted stop site. The normal
// Radiance host-readback path observes the generated allFinished signal before
// stopSim invalidates the private caches.
extern "C" void gsim_model_finished(void) { g_model_finished = true; }

static bool enabled(const char* value) {
  return value && (!strcmp(value, "1") || !strcmp(value, "true") ||
                   !strcmp(value, "yes") || !strcmp(value, "on"));
}

static bool parse_u64(const char* text, uint64_t* value) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const unsigned long long parsed = strtoull(text, &end, 0);
  if (!end || *end != '\0') return false;
  *value = (uint64_t)parsed;
  return true;
}

static bool binary_dump(STestHarness* dut, const char* path, uint64_t address,
                        uint64_t local_address, uint64_t length) {
  if (!path || path[0] != '/' || length == 0) return false;
  std::vector<unsigned char> result((size_t)length);
  if (!dram_peek(address, result.data(), (unsigned long)length)) return false;

  // The inclusive L2 is 8-way, with 2048 32-byte sets. The emitted address
  // equations are set=phys[15:5], tag=phys[32:16]. Each way/set address is
  // banked by set[0]: banks 0..3 hold even sets and 4..7 hold odd sets, at
  // index=(way << 10) | set[10:1]. Directory entries are
  // {dirty[22], state[21:20], clients[19:17], tag[16:0]}.
  //
  // L0 writeback at allFinished is ordered into this cache, but the inclusive
  // cache is not itself flushed to DRAM. Overlay valid L2 lines before L0.
  const uint64_t first_l2_line = address & ~UINT64_C(31);
  const uint64_t last_l2_line = (address + length - 1) & ~UINT64_C(31);
  const size_t l2_line_count =
      (size_t)((last_l2_line - first_l2_line) / 32 + 1);
  size_t l2_overlaid = 0;
  for (size_t line_no = 0; line_no < l2_line_count; ++line_no) {
    const uint64_t line_addr = first_l2_line + line_no * 32;
    const size_t set = (size_t)((line_addr >> 5) & 0x7ff);
    const uint32_t wanted_tag = (uint32_t)((line_addr >> 16) & 0x1ffff);
    int matching_way = -1;
    for (int way = 0; way < 8; ++way) {
      const uint32_t entry =
          dut->chiptop0$system$coh_wrapper$l2$inclusive_cache_bank_sched$directory$cc_dir[set][way];
      const uint32_t state = (entry >> 20) & 3;
      const uint32_t clients = (entry >> 17) & 7;
      const uint32_t tag = entry & 0x1ffff;
      if (l2_line_count <= 4) {
        fprintf(stderr,
                "[gsim-emu] L2_PROBE phys_line=0x%llx set=%zu way=%d entry=0x%x state=%u clients=0x%x tag=0x%x want=0x%x\n",
                (unsigned long long)line_addr, set, way, entry, state,
                clients, tag, wanted_tag);
      }
      // state==0 means the L2 data way is invalid. A clients-only directory
      // entry names a private L0 owner and is intentionally handled below.
      if (state == 0 || tag != wanted_tag) continue;
      if (matching_way >= 0) {
        fprintf(stderr,
                "[gsim-emu] L2_OVERLAY conflict phys_line=0x%llx set=%zu ways=%d,%d\n",
                (unsigned long long)line_addr, set, matching_way, way);
        return false;
      }
      matching_way = way;
    }
    if (matching_way < 0) continue;
    const size_t data_index = ((size_t)matching_way << 10) | (set >> 1);
    const int first_bank = (set & 1) ? 4 : 0;
    uint64_t words[4];
#define L2_BANK(n) \
    dut->chiptop0$system$coh_wrapper$l2$inclusive_cache_bank_sched$bankedStore$cc_banks_##n[data_index]
    if (first_bank == 0) {
      words[0] = L2_BANK(0); words[1] = L2_BANK(1);
      words[2] = L2_BANK(2); words[3] = L2_BANK(3);
    } else {
      words[0] = L2_BANK(4); words[1] = L2_BANK(5);
      words[2] = L2_BANK(6); words[3] = L2_BANK(7);
    }
#undef L2_BANK
    const uint64_t begin = address > line_addr ? address - line_addr : 0;
    const uint64_t end_addr =
        address + length < line_addr + 32 ? address + length : line_addr + 32;
    const uint64_t end = end_addr - line_addr;
    memcpy(&result[(line_addr + begin) - address],
           reinterpret_cast<unsigned char*>(words) + begin,
           (size_t)(end - begin));
    l2_overlaid++;
  }
  fprintf(stderr,
          "[gsim-emu] L2_OVERLAY complete lines=%zu/%zu phys=0x%llx\n",
          l2_overlaid, l2_line_count, (unsigned long long)address);

  // This engine is bound to the emitted RadianceGsimConfig L0D shape: one
  // direct-mapped 64-set x 64-byte data array per Muon core, with metadata
  // {state[1:0], tag[18:0]}, index=address[11:6], tag=address>>12.  DRAM alone
  // is insufficient because a successful allFinished/stopSim may leave the
  // final dirty lines resident. Overlay every matching valid line and refuse
  // incoherent duplicates instead of guessing which core owns it.
  const uint64_t first_line = local_address & ~UINT64_C(63);
  const uint64_t last_line = (local_address + length - 1) & ~UINT64_C(63);
  const size_t line_count = (size_t)((last_line - first_line) / 64 + 1);
  std::vector<unsigned char> seen(line_count, 0);
  std::vector<unsigned char> cache_bytes(line_count * 64);
  size_t overlaid = 0;
  bool conflict = false;
  auto visit = [&](const char* label, auto& meta, auto& data) {
    for (size_t line_no = 0; line_no < line_count; ++line_no) {
      const uint64_t line_addr = first_line + line_no * 64;
      const size_t index = (size_t)((line_addr >> 6) & 63);
      const uint32_t entry = meta[index];
      const uint32_t state = (entry >> 19) & 3;
      const uint32_t tag = entry & 0x7ffff;
      const uint64_t phys_line = address + (line_addr - local_address);
      if (line_count <= 4) {
        uint32_t probe_words[16];
        memcpy(probe_words, &data[index], sizeof(probe_words));
        fprintf(stderr,
                "[gsim-emu] CACHE_PROBE cache=%s local_line=0x%llx index=%zu entry=0x%x state=%u tag=0x%x want=0x%llx data=%08x,%08x,%08x,%08x\n",
                label, (unsigned long long)line_addr, index, entry, state, tag,
                (unsigned long long)((phys_line >> 12) & 0x7ffff),
                probe_words[0], probe_words[1], probe_words[2], probe_words[3]);
      }
      if (state == 0 || tag != ((phys_line >> 12) & 0x7ffff)) continue;
      unsigned char line[64];
      memcpy(line, &data[index], sizeof(line));
      if (seen[line_no] && memcmp(&cache_bytes[line_no * 64], line, 64) != 0) {
        fprintf(stderr,
                "[gsim-emu] CACHE_OVERLAY conflict cache=%s local_line=0x%llx index=%zu\n",
                label, (unsigned long long)line_addr, index);
        conflict = true;
        continue;
      }
      if (!seen[line_no]) {
        memcpy(&cache_bytes[line_no * 64], line, 64);
        seen[line_no] = 1;
        overlaid++;
      }
    }
  };
#define VISIT_L0D(label, prefix) visit(label, \
    dut->prefix##$l0d$tlnbdCache$nbdCache$meta$tag_array, \
    dut->prefix##$l0d$tlnbdCache$nbdCache$data$array_0_0)
  VISIT_L0D("c0t0", chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile);
  VISIT_L0D("c0t1", chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile);
  VISIT_L0D("c1t0", chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile);
  VISIT_L0D("c1t1", chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile);
#undef VISIT_L0D
  if (conflict) return false;
  for (size_t line_no = 0; line_no < line_count; ++line_no) {
    if (!seen[line_no]) continue;
    const uint64_t line_addr = first_line + line_no * 64;
    const uint64_t begin = local_address > line_addr ? local_address - line_addr : 0;
    const uint64_t end_addr = local_address + length < line_addr + 64
        ? local_address + length : line_addr + 64;
    const uint64_t end = end_addr - line_addr;
    memcpy(&result[(line_addr + begin) - local_address],
           &cache_bytes[line_no * 64 + begin], (size_t)(end - begin));
  }
  fprintf(stderr, "[gsim-emu] CACHE_OVERLAY complete lines=%zu/%zu local=0x%llx\n",
          overlaid, line_count, (unsigned long long)local_address);

  FILE* output = fopen(path, "wb");
  if (!output) return false;
  const size_t written = fwrite(result.data(), 1, result.size(), output);
  const bool ok = fclose(output) == 0 && written == result.size();
  if (!ok) {
    fprintf(stderr, "[gsim-emu] BINARY_DUMP failed address=0x%llx requested=%llu written=%zu\n",
            (unsigned long long)address, (unsigned long long)length, written);
    return false;
  }
  fprintf(stderr, "[gsim-emu] BINARY_DUMP complete bytes=%llu\n",
          (unsigned long long)length);
  return true;
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <soc.elf> [+plusargs]\n", argv[0]); return 2; }
  harness_set_args(argc, argv);

  uint64_t max_cycles = 20000000ULL;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("+max-cycles=", 0) == 0) max_cycles = strtoull(a.c_str() + 12, nullptr, 0);
  }
  const bool stop_on_finish = enabled(getenv("GSIM_STOP_ON_FINISH"));
  const char* dump_file = getenv("GSIM_DUMP_FILE");
  uint64_t dump_address = 0, dump_local_address = 0, dump_length = 0;
  const bool dump_requested = dump_file != nullptr;
  if (dump_requested &&
      (!stop_on_finish || !parse_u64(getenv("GSIM_DUMP_ADDR"), &dump_address) ||
       !parse_u64(getenv("GSIM_DUMP_LOCAL_ADDR"), &dump_local_address) ||
       !parse_u64(getenv("GSIM_DUMP_LEN"), &dump_length) || dump_length == 0)) {
    fprintf(stderr, "[gsim-emu] invalid binary-dump ABI; require STOP_ON_FINISH plus FILE/ADDR/LOCAL_ADDR/LEN\n");
    return 2;
  }

  STestHarness* dut = new STestHarness();
  // Power-on zero-init: GSIM's `new` does not value-initialize members and RANDOMIZE_INIT is off,
  // so non-reset registers (e.g. icacheInFlightsReg) start as heap garbage → the Muon icache never
  // completes a fetch. Zero the whole register-storage block [_var_start,_var_end) to a clean 0 state.
  {
    uint32_t* p0 = &dut->_var_start;
    uint32_t* p1 = &dut->_var_end;
    for (uint32_t* p = p0; p < p1; ++p) *p = 0;
    fprintf(stderr, "[gsim-emu] zero-init %ld register words\n", (long)(p1 - p0));
  }

#define RPC   dut->chiptop0$system$tile_prci_domain$element_reset_domain$rockettile$core$wb_reg_pc
#define MI00  dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$minstretReg
#define MI01  dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$execute$minstretReg
#define MI10  dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$minstretReg
#define MI11  dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$execute$minstretReg
#define MRST  dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$_be_reset_T_1
#define MPC   dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$fe$warpScheduler$currPC
#define PROBE(tag) fprintf(stderr, "[gsim-probe %s] rocket_pc=0x%llx muon_c0t0_currPC=0x%llx muon_minstret[c0t0=%llu c0t1=%llu c1t0=%llu c1t1=%llu] muon_be_reset=%u\n", \
    tag, (unsigned long long)RPC, (unsigned long long)MPC, (unsigned long long)MI00, (unsigned long long)MI01, \
    (unsigned long long)MI10, (unsigned long long)MI11, (unsigned)MRST)

// --- fetch-path localization probe (cluster0 core0) ---
#define FIC_V dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$icacheWordNodeOut$$a$$valid
#define FIF   dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$fe$warpScheduler$icacheInFlightsReg
#define FPROBE(c) fprintf(stderr, "[gsim-fetch cyc=%llu] currPC=0x%llx icacheWordNode.a.valid=%u icacheInFlights=0x%llx be_reset=%u\n", \
    (unsigned long long)(c), (unsigned long long)MPC, (unsigned)FIC_V, (unsigned long long)FIF, (unsigned)MRST)

// --- FP writeback / regfile localization probe (writeback valid, dest reg, data, regfile slot). ---
#define WBPROBE(cyc, tag, VALID, RD, DATA, TMASK, RF) do { \
  if (VALID) { unsigned rd = (unsigned)(RD); \
    fprintf(stderr, "[wb cyc=%llu %s] rd=%u data[0]=0x%08x data[1]=0x%08x tmask=0x%llx rf[rd][0]=0x%08x\n", \
      (unsigned long long)(cyc), tag, rd, (unsigned)(DATA)[0], (unsigned)(DATA)[1], \
      (unsigned long long)(TMASK), (unsigned)(RF)[rd][0]); } } while(0)
// cluster1 core0 (clid=1 cid=0, where the matmul warp issues)
#define A10V dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$valid
#define A10R dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$bits$$rd
#define A10D dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$bits$$data
#define A10T dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$bits$$tmask
#define F10  dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$collector$rfBanks_mem
// cluster1 core1
#define A11V dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$valid
#define A11R dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$bits$$rd
#define A11D dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$bits$$data
#define A11T dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$execute$respArbiter$io$$out$$bits$$reg$$bits$$tmask
#define F11  dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$be$collector$rfBanks_mem
// c1t0 fpAddMulPipe CVFPU resp (as the core sees it): valid, tag, result lane0
#define R10V dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$fpAddMulPipe$CVFPU$resp$$valid
#define R10T dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$fpAddMulPipe$CVFPU$resp$$bits$$tag
#define R10D dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$be$execute$fpAddMulPipe$CVFPU$resp$$bits$$result
#define WB_ALL(cyc) do { \
  if (R10V) fprintf(stderr, "[fpresp cyc=%llu c1t0] valid=1 tag=%u result[0]=0x%08x\n", \
    (unsigned long long)(cyc), (unsigned)R10T, (unsigned)(uint32_t)(uint64_t)R10D); \
  WBPROBE(cyc, "c1t0", A10V, A10R, A10D, A10T, F10); \
  WBPROBE(cyc, "c1t1", A11V, A11R, A11D, A11T, F11); } while(0)

// c1t0 flush units (icache l0i + dcache l0d): flushing, counter, inFlights, dirty, and dcache wb release
#define FI_FL dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0i$tlnbdCache$nbdCache$flush_unit$flushing
#define FI_CT dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0i$tlnbdCache$nbdCache$flush_unit$flushCounter_value
#define FI_IF dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0i$tlnbdCache$nbdCache$flush_unit$inFlights
#define FD_FL dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0d$tlnbdCache$nbdCache$flush_unit$flushing
#define FD_CT dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0d$tlnbdCache$nbdCache$flush_unit$flushCounter_value
#define FD_IF dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0d$tlnbdCache$nbdCache$flush_unit$inFlights
#define FD_DR dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0d$tlnbdCache$nbdCache$flush_unit$isDirty
#define FD_WBV dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$l0d$tlnbdCache$nbdCache$wb$io$$release$$valid
#define FLUSH_PROBE(cyc) do { if (FI_FL || FD_FL) \
  fprintf(stderr, "[flush cyc=%llu c1t0] i{fl=%u ct=%u if=%u} d{fl=%u ct=%u if=%u dirty=%u wbrel=%u}\n", \
    (unsigned long long)(cyc), (unsigned)FI_FL, (unsigned)FI_CT, (unsigned)FI_IF, \
    (unsigned)FD_FL, (unsigned)FD_CT, (unsigned)FD_IF, (unsigned)FD_DR, (unsigned)FD_WBV); } while(0)

// per-core warpScheduler "finished" signal (allFinished = AND of all 4 => stopSim)
#define FIN00 dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$fe$warpScheduler$_io_finished_T_9
#define FIN01 dut->chiptop0$system$cluster_prci_domain$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$fe$warpScheduler$_io_finished_T_9
#define FIN10 dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain$element_reset_domain$muon_tile$core$fe$warpScheduler$_io_finished_T_9
#define FIN11 dut->chiptop0$system$cluster_prci_domain_1$element_reset_domain$element$tile_prci_domain_1$element_reset_domain$muon_tile$core$fe$warpScheduler$_io_finished_T_9
#define FIN_ALL dut->chiptop0$system$domain_1$resetAggregator$allFinished
#define FIN_PROBE(cyc) fprintf(stderr, "[fin cyc=%llu] c0t0=%u c0t1=%u c1t0=%u c1t1=%u allFinished=%u\n", \
    (unsigned long long)(cyc), (unsigned)FIN00, (unsigned)FIN01, (unsigned)FIN10, (unsigned)FIN11, (unsigned)FIN_ALL)

  // reset sequence
  dut->set_reset(1);
  for (int i = 0; i < 12; i++) dut->step();
  dut->set_reset(0);
  PROBE("post-reset");

  fprintf(stderr, "[gsim-emu] reset done, running (max_cycles=%llu)\n",
          (unsigned long long)max_cycles);

  auto start = std::chrono::steady_clock::now();
  uint64_t cycles = 0;
  bool saw_not_finished = !FIN_ALL;
  extern unsigned long long g_sim_cycle;
  while (!g_tsi_done && !(stop_on_finish && g_model_finished) && cycles < max_cycles) {
    dut->step();
    cycles++;
    g_sim_cycle = cycles;
    if (!FIN_ALL) saw_not_finished = true;
    if (stop_on_finish && saw_not_finished && FIN_ALL) {
      g_model_finished = true;
      fprintf(stderr, "[gsim-emu] RTL_COMPLETION allFinished=1 after_observed_low=1\n");
      break;
    }
    if (cycles >= 1 && cycles <= 160) FPROBE(cycles);
    if (getenv("WB_TRACE")) WB_ALL(cycles);
    if (getenv("FLUSH_TRACE")) FLUSH_PROBE(cycles);
    if (getenv("FIN_TRACE") && (cycles % 1000ULL) == 0) FIN_PROBE(cycles);
    if ((cycles % 2000ULL) == 0) PROBE("tick");
    if ((cycles % 1000000ULL) == 0) {
      auto now = std::chrono::steady_clock::now();
      double s = std::chrono::duration<double>(now - start).count();
      fprintf(stderr, "[gsim-emu] %llu cycles, %.1fs, %.0f cyc/s\n",
              (unsigned long long)cycles, s, cycles / s);
      PROBE("progress");
    }
  }
  PROBE("final");
  auto end = std::chrono::steady_clock::now();
  double secs = std::chrono::duration<double>(end - start).count();

  fflush(stdout);
  fprintf(stderr, "\n[gsim-emu] FINISHED: cycles=%llu wall=%.2fs (%.0f cyc/s) done=%d model_finished=%d exit_code=%d\n",
          (unsigned long long)cycles, secs, cycles / (secs > 0 ? secs : 1),
          (int)g_tsi_done, (int)g_model_finished, g_exit_code);

  if (dump_requested) {
    if (!g_model_finished) {
      fprintf(stderr, "[gsim-emu] refusing binary dump without RTL model completion\n");
      return 3;
    }
    if (!binary_dump(dut, dump_file, dump_address, dump_local_address, dump_length)) return 4;
  }

  // Optional DRAM dumps: GSIM_DUMP=addr:len[,addr:len...] (hex addr, decimal len) -> hex to stderr
  if (const char* env = getenv("GSIM_DUMP")) {
    std::string s = env; size_t pos = 0;
    while (pos < s.size()) {
      size_t comma = s.find(',', pos); if (comma == std::string::npos) comma = s.size();
      std::string tok = s.substr(pos, comma - pos); pos = comma + 1;
      size_t colon = tok.find(':'); if (colon == std::string::npos) continue;
      uint64_t addr = strtoull(tok.substr(0, colon).c_str(), nullptr, 0);
      unsigned long len = strtoul(tok.substr(colon + 1).c_str(), nullptr, 0);
      if (len == 0 || len > 4096) len = 32;
      unsigned char buf[4096];
      if (dram_peek(addr, buf, len)) {
        fprintf(stderr, "[gsim-emu] DRAM 0x%llx:", (unsigned long long)addr);
        for (unsigned long i = 0; i < len; i++) fprintf(stderr, " %02x", buf[i]);
        fprintf(stderr, "\n");
      } else fprintf(stderr, "[gsim-emu] DRAM 0x%llx: <out of range>\n", (unsigned long long)addr);
    }
  }
  return g_exit_code;
}
