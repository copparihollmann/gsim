// Chipyard simulation-boundary adapters, not accelerator/compiler models.
// Registered memory/TSI/UART calls delegate to the corresponding upstream
// C++ implementations; sizes and addresses come from FIRRTL parameters.
#include "TestHarness.h"
#include "mm.h"
#include "testchip_tsi.h"
#include "terminal_dump.h"
#include "uart.h"
#include <cmath>
#include <csignal>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <sys/mman.h>

uint64_t gsim_time_ps = 0;
bool gsim_done = false;
int gsim_exit_code = 0;
static int host_argc;
static char** host_argv;
static backing_data_t backing{nullptr, 0};
static uint64_t memory_base;
static terminal_dump::Request dump_request;
static bool dump_complete = false;
static volatile sig_atomic_t interrupted = 0;
static void (*previous_sigint)(int) = nullptr;
static void (*previous_sigterm)(int) = nullptr;

static void note_signal(int signal) {
  interrupted = 1;
  auto previous = signal == SIGINT ? previous_sigint : previous_sigterm;
  if (previous && previous != SIG_IGN && previous != SIG_DFL) previous(signal);
  else if (previous == SIG_DFL) {
    std::signal(signal, SIG_DFL);
    std::raise(signal);
  }
}

bool harness_dump_requested() { return dump_request.enabled; }
bool harness_dump_complete() { return dump_complete && !interrupted; }

void harness_args(int argc, char** argv) {
  dump_request = terminal_dump::request_from_args(argc, argv);
  host_argc = argc;
  host_argv = argv;
}

static uint64_t plusarg(const char* format, uint64_t fallback) {
  std::string key = "+" + std::string(format);
  key = key.substr(0, key.find('=')) + "=";
  for (int i = 1; i < host_argc; ++i) {
    std::string arg = host_argv[i];
    if (arg.rfind(key, 0) == 0) {
      char* end;
      uint64_t result = std::strtoull(arg.c_str() + key.size(), &end, 0);
      if (*end) throw std::runtime_error("invalid plusarg " + arg);
      return result;
    }
  }
  return fallback;
}

void plusarg_reader(int64_t value, const char* format, int64_t width, uint32_t& out) {
  if (width < 1 || width > 32) throw std::runtime_error("unsupported plusarg width");
  out = plusarg(format, value) & (width == 32 ? UINT32_MAX : (1u << width) - 1u);
}
void plusarg_reader(int64_t value, const char* format, int64_t width, uint8_t& out) {
  if (width < 1 || width > 8) throw std::runtime_error("unsupported plusarg width");
  out = plusarg(format, value) & ((1u << width) - 1u);
}

void GenericDigitalInIOCell(uint8_t pad, uint8_t& value, uint8_t enabled) {
  if (!enabled) throw std::runtime_error("disabled digital input is not a two-state value");
  value = pad;
}
void GenericDigitalOutIOCell(uint8_t& pad, uint8_t value, uint8_t enabled) {
  if (!enabled) throw std::runtime_error("tristate digital output is unsupported");
  pad = value;
}

struct ClockState { uint64_t next; uint64_t half; };
static std::unordered_map<uint8_t*, ClockState> clocks;

uint64_t harness_sample_step_ps() {
  uint64_t result = 0;
  for (const auto& clock : clocks) result = std::gcd(result, clock.second.half);
  if (!result) throw std::runtime_error("no declared free-running clock for the harness");
  return result;
}
uint64_t harness_reference_period_ps() {
  uint64_t result = UINT64_MAX;
  for (const auto& clock : clocks) result = std::min(result, clock.second.half);
  if (result > UINT64_MAX / 2) throw std::runtime_error("no representable reference clock");
  return 2 * result;
}

void ClockSourceAtFreqMHz(double period_ns, uint8_t power, uint8_t gate, uint8_t& clk) {
  // Upstream: initially zero; at every PERIOD/2 event clk = ~clk & enable.
  // Per-output state keeps distinct instances independent, including equal periods.
  double half_ps = period_ns * 500.0;
  if (!std::isfinite(half_ps) || half_ps < 1 || half_ps >= static_cast<double>(UINT64_MAX / 2) || half_ps != std::floor(half_ps))
    throw std::runtime_error("clock period must have integral picosecond half-period");
  auto inserted = clocks.emplace(&clk, ClockState{static_cast<uint64_t>(half_ps), static_cast<uint64_t>(half_ps)});
  ClockState& state = inserted.first->second;
  if (state.half != static_cast<uint64_t>(half_ps)) throw std::runtime_error("clock period changed");
  if (gsim_time_ps > state.next) throw std::runtime_error("harness skipped a clock event");
  if (gsim_time_ps == state.next) {
    clk = (!clk) & power & (!gate);
    state.next += state.half;
  }
}

void EICG_wrapper(uint8_t clock, uint8_t test_enable, uint8_t enable, uint8_t& out) {
  // The vendor clock gate latches its enable while the input clock is low.
  static std::unordered_map<uint8_t*, uint8_t> latched;
  uint8_t& value = latched[&out];
  if (!clock) value = enable | test_enable;
  out = clock & value;
}

static uint8_t* checked_memory(uint64_t address, size_t size) {
  if (!backing.data || address < memory_base || address - memory_base > backing.size ||
      size > backing.size - (address - memory_base))
    throw std::runtime_error("host memory request outside declared DRAM");
  return backing.data + address - memory_base;
}

void SimDRAM(int64_t address_bits, int64_t chip_id, int64_t clock_hz, int64_t data_bits,
             int64_t id_bits, int64_t line_size, int64_t base, int64_t size,
             uint8_t reset, uint8_t& aw_ready, uint8_t aw_valid, uint8_t aw_id,
             uint32_t aw_address, uint8_t aw_len, uint8_t aw_size, uint8_t aw_burst,
             uint8_t aw_lock, uint8_t aw_cache, uint8_t aw_prot, uint8_t aw_qos,
             uint8_t& w_ready, uint8_t w_valid, uint64_t w_data, uint8_t w_strb, uint8_t w_last,
             uint8_t b_ready, uint8_t& b_valid, uint8_t& b_id, uint8_t& b_resp,
             uint8_t& ar_ready, uint8_t ar_valid, uint8_t ar_id, uint32_t ar_address,
             uint8_t ar_len, uint8_t ar_size, uint8_t ar_burst, uint8_t ar_lock,
             uint8_t ar_cache, uint8_t ar_prot, uint8_t ar_qos, uint8_t r_ready,
             uint8_t& r_valid, uint8_t& r_id, uint64_t& r_data, uint8_t& r_resp, uint8_t& r_last) {
  static std::unique_ptr<mm_magic_t> model;
  if (!model) {
    if (address_bits != 32 || data_bits != 64 || size <= 0 || base < 0 ||
        chip_id != 0 || clock_hz <= 0 || id_bits < 1 || id_bits > 8 || line_size <= 0)
      throw std::runtime_error("unsupported native memory adapter parameterization");
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (data == MAP_FAILED) throw std::runtime_error("DRAM allocation failed");
    backing = {static_cast<uint8_t*>(data), static_cast<size_t>(size)};
    memory_base = base;
    model = std::make_unique<mm_magic_t>(base, size, data_bits / 8, line_size, backing);
  }
  model->tick(reset, ar_valid, ar_address, ar_id, ar_size, ar_len,
              aw_valid, aw_address, aw_id, aw_size, aw_len,
              w_valid, w_strb, &w_data, w_last, r_ready, b_ready);
  aw_ready = reset ? 0 : model->aw_ready();
  ar_ready = reset ? 0 : model->ar_ready();
  w_ready = reset ? 0 : model->w_ready();
  b_valid = reset ? 0 : model->b_valid(); b_id = model->b_id(); b_resp = model->b_resp();
  r_valid = reset ? 0 : model->r_valid(); r_id = model->r_id(); r_resp = model->r_resp();
  r_last = model->r_last();
  r_data = r_valid ? *static_cast<uint64_t*>(model->r_data()) : 0;
  (void)aw_burst; (void)aw_lock; (void)aw_cache; (void)aw_prot; (void)aw_qos;
  (void)ar_burst; (void)ar_lock; (void)ar_cache; (void)ar_prot; (void)ar_qos;
}

class HostTSI : public testchip_tsi_t {
public:
  HostTSI() : testchip_tsi_t(host_argc, host_argv, true) {
    if (dump_request.enabled) {
      previous_sigint = std::signal(SIGINT, note_signal);
      previous_sigterm = std::signal(SIGTERM, note_signal);
    }
  }
protected:
  void load_mem_write(addr_t address, size_t size, const void* source) override {
    memcpy(checked_memory(address, size), source, size);
  }
  void load_mem_read(addr_t address, size_t size, void* destination) override {
    memcpy(destination, checked_memory(address, size), size);
  }
  void stop() override {
    if (dump_request.enabled) {
      if (interrupted || exit_code() != 0 || !backing.data) {
        fprintf(stderr, "[gsim-dump] refused: signal, nonzero guest exit or absent DRAM\n");
      } else {
        try {
          auto regions = terminal_dump::read_regions(dump_request.regions, memory_base, backing.size);
          auto coherent_read = [this](uint64_t address, size_t size, void* destination) {
            // After load_program(), testchip_tsi_t clears is_loadmem. This
            // read traverses the SoC's live TSI/TileLink host port.
            memif().read(address, size, destination);
          };
          if (dump_request.mode == terminal_dump::Request::Mode::coherent_packet)
            terminal_dump::publish_packet(dump_request.output, regions,
                                          coherent_read, [] { return interrupted != 0; });
          else
            terminal_dump::publish(dump_request.output, regions,
                                   coherent_read, [] { return interrupted != 0; });
          dump_complete = true;
          if (dump_request.mode == terminal_dump::Request::Mode::coherent_packet)
            fprintf(stderr, "[gsim-dump] complete regions=%zu source=coherent mode=packet\n", regions.size());
          else
            fprintf(stderr, "[gsim-dump] complete regions=%zu source=coherent\n", regions.size());
        } catch (const std::exception& error) {
          fprintf(stderr, "[gsim-dump] incomplete: %s\n", error.what());
        }
      }
    }
    testchip_tsi_t::stop();
  }
};

void SimTSI(int64_t chip_id, uint8_t reset, uint8_t in_ready,
            uint8_t& in_valid, uint32_t& in_bits, uint8_t& out_ready,
            uint8_t out_valid, uint32_t out_bits, uint32_t& exit) {
  static std::unique_ptr<HostTSI> model;
  if (chip_id != 0) throw std::runtime_error("multiple TSI instances unsupported");
  if (reset) { in_valid = 0; in_bits = 0; out_ready = 0; exit = 0; return; }
  if (!model) model = std::make_unique<HostTSI>();
  model->tick(out_valid, out_bits, in_ready);
  model->switch_to_host();
  in_valid = model->in_valid(); in_bits = model->in_bits(); out_ready = model->out_ready();
  exit = model->done() ? ((model->exit_code() << 1) | 1) : 0;
  if (model->done()) { gsim_done = true; gsim_exit_code = model->exit_code(); }
}

void SimUART(int64_t force_pty, int64_t uart_number, uint8_t reset,
             uint8_t in_ready, uint8_t& in_valid, uint8_t& in_bits,
             uint8_t& out_ready, uint8_t out_valid, uint8_t out_bits) {
  static std::unique_ptr<uart_t> model;
  if (!model) model = std::make_unique<uart_t>(nullptr, uart_number, force_pty);
  if (reset) { in_valid = 0; in_bits = 0; out_ready = 0; return; }
  char bits = in_bits;
  model->tick(out_valid, &out_ready, out_bits, &in_valid, in_ready, &bits);
  in_bits = bits;
}

void SimJTAG(int64_t delay, uint8_t reset, uint8_t& trstn, uint8_t& tck,
             uint8_t& tms, uint8_t& tdi, uint8_t tdo, uint8_t driven,
             uint8_t enable, uint8_t init_done, uint32_t& exit) {
  static bool previous_reset = false;
  if (enable) throw std::runtime_error("interactive JTAG is not enabled in this bounded harness");
  if (reset || previous_reset) { exit = 0; tck = !tck; }
  previous_reset = reset;
  (void)delay; (void)trstn; (void)tms; (void)tdi; (void)tdo; (void)driven; (void)init_done;
}
