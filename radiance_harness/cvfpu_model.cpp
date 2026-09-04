// Functional model of the OpenHW cvfpu (fpnew) blackbox as instantiated in the radiance Muon core.
//
// GSIM treats CVFPU as a clocked extmodule and (via the AST2Graph clocked-ext fix) schedules it as a
// registered source: this function is called once per step, reads the latched inputs, and drives the
// outputs. We reproduce fpnew's arithmetic bit-exactly by dispatching each SIMD lane to Berkeley
// SoftFloat (the same reference cvfpu itself is validated against). Rounding-mode and exception-flag
// encodings map 1:1 between fpnew and SoftFloat, so in the standard modes the result is bit-exact.
//
// Interface (from radiance vsrc/CVFPU.v + fpnew_pkg.sv):
//   op field is 5 bits: op[4:1] = operation_e, op[0] = op_mod.
//   operation_e: FMADD=0 FNMSUB=1 ADD=2 MUL=3 DIV=4 SQRT=5 SGNJ=6 MINMAX=7 CMP=8 CLASSIFY=9
//                F2F=10 F2I=11 I2F=12 CPKAB=13 CPKCD=14
//   fp_format_e: FP32=0 FP64=1 FP16=2 FP8=3 FP16ALT(bf16)=4   (this config enables FP32 + bf16)
//   roundmode_e: RNE=0 RTZ=1 RDN=2 RUP=3 RMM=4 ROD=5 (== SoftFloat softfloat_roundingMode)
//   operand mapping (fpnew_fma_multi.sv): a=operands[0] b=operands[1] c=operands[2];
//     op_mod inverts sign of C; FNMSUB inverts sign of A; ADD sets A=+1.0 (=> b+/-c); MUL sets C=+/-0.
//
// Two instances differ only in width/tag: fpAddMulPipe (WIDTH=256 => 8xFP32 lanes, tag 25b) and
// fpDivSqrtPipe (WIDTH=64 => 2xFP32 lanes, tag 13b). Emitted as two C++ overloads; both forward here.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>

extern "C" {
#include "softfloat.h"
}

extern "C" { extern unsigned long long g_cvfpu_accepts; }
unsigned long long g_sim_cycle = 0;  // set by main.cpp each step (C++ linkage; global scope)

namespace {

typedef unsigned _BitInt(256) u256;

// fpnew operation_e (op[4:1])
enum { OP_FMADD=0, OP_FNMSUB=1, OP_ADD=2, OP_MUL=3, OP_DIV=4, OP_SQRT=5,
       OP_SGNJ=6, OP_MINMAX=7, OP_CMP=8, OP_CLASSIFY=9, OP_F2F=10, OP_F2I=11, OP_I2F=12 };

static inline bool f32_isNaN(uint32_t v)  { return ((v & 0x7f800000u) == 0x7f800000u) && (v & 0x007fffffu); }
static inline bool f32_isSNaN(uint32_t v) { return f32_isNaN(v) && !(v & 0x00400000u); }
static const uint32_t F32_QNAN = 0x7fc00000u;

// Compute one FP32 lane. Returns the 32-bit result; accumulates flags into *flags.
static uint32_t compute_lane_fp32(int operation, int op_mod, uint8_t rnd, uint8_t intFmt,
                                  uint32_t a, uint32_t b, uint32_t c, uint8_t* flags) {
  softfloat_roundingMode = rnd;             // fpnew roundmode == SoftFloat roundingMode
  softfloat_exceptionFlags = 0;
  float32_t fa{a}, fb{b}, fc{c}, fr{0};
  switch (operation) {
    case OP_FMADD: { float32_t cc=fc; if (op_mod) cc.v ^= 0x80000000u; fr = f32_mulAdd(fa, fb, cc); break; }
    case OP_FNMSUB: { float32_t aa=fa; aa.v ^= 0x80000000u; float32_t cc=fc; if (op_mod) cc.v ^= 0x80000000u;
                      fr = f32_mulAdd(aa, fb, cc); break; }
    case OP_ADD: { float32_t cc=fc; if (op_mod) cc.v ^= 0x80000000u; fr = f32_add(fb, cc); break; }
    case OP_MUL: fr = f32_mul(fa, fb); break;
    case OP_DIV: fr = f32_div(fa, fb); break;
    case OP_SQRT: fr = f32_sqrt(fa); break;
    case OP_SGNJ: { // rnd: 0=sgnj 1=sgnjn 2=sgnjx ; sign source = operand b
                    uint32_t s; if (rnd==0) s = b & 0x80000000u; else if (rnd==1) s = (~b) & 0x80000000u;
                    else s = (a ^ b) & 0x80000000u; fr.v = (a & 0x7fffffffu) | s; break; }
    case OP_MINMAX: { // rnd: 0=min 1=max ; RISC-V minNum/maxNum semantics
                    bool an=f32_isNaN(a), bn=f32_isNaN(b);
                    if (f32_isSNaN(a) || f32_isSNaN(b)) softfloat_exceptionFlags |= softfloat_flag_invalid;
                    if (an && bn) fr.v = F32_QNAN; else if (an) fr.v = b; else if (bn) fr.v = a;
                    else { bool lt = f32_lt(fa, fb);
                           bool pickA = (rnd==0) ? lt : !lt;
                           // handle +0/-0: min picks -0, max picks +0
                           if ((a|b) == 0x80000000u || (a==0 && b==0x80000000u) || (a==0x80000000u && b==0)) {
                             bool aNeg = a==0x80000000u; pickA = (rnd==0) ? aNeg : !aNeg; }
                           fr.v = pickA ? a : b; }
                    break; }
    case OP_CMP: { // rnd: 0=LE 1=LT 2=EQ
                    bool res=false; bool an=f32_isNaN(a), bn=f32_isNaN(b);
                    if (rnd==2) { if (an||bn) { res=false; if (f32_isSNaN(a)||f32_isSNaN(b)) softfloat_exceptionFlags|=softfloat_flag_invalid; }
                                  else res = f32_eq(fa, fb); }
                    else { if (an||bn) { res=false; softfloat_exceptionFlags|=softfloat_flag_invalid; }
                           else res = (rnd==0) ? f32_le(fa, fb) : f32_lt(fa, fb); }
                    fr.v = res ? 1u : 0u; break; }
    case OP_CLASSIFY: { uint32_t m=0; bool neg=a>>31; uint32_t exp=(a>>23)&0xff, man=a&0x7fffff;
                    if (exp==0xff && man) m = f32_isSNaN(a)?(1u<<8):(1u<<9);
                    else if (exp==0xff) m = neg?(1u<<0):(1u<<7);
                    else if (exp==0 && man==0) m = neg?(1u<<3):(1u<<4);
                    else if (exp==0) m = neg?(1u<<2):(1u<<5);
                    else m = neg?(1u<<1):(1u<<6);
                    fr.v = m; break; }
    case OP_F2I: { // dst integer; intFmt: 0=int8 1=int16 2=int32 3=int64 ; op_mod=1 => unsigned
                    if (op_mod) fr.v = (uint32_t)f32_to_ui32(fa, rnd, true);
                    else fr.v = (uint32_t)f32_to_i32(fa, rnd, true); break; }
    case OP_I2F: { if (op_mod) fr = ui32_to_f32(a); else fr = i32_to_f32((int32_t)a); break; }
    case OP_F2F: fr.v = a; break; // FP32->FP32 identity (only FP32 enabled among 32b path)
    default: fr.v = a; break;
  }
  *flags |= (uint8_t)(softfloat_exceptionFlags & 0x1f);   // SoftFloat flag bits == RISC-V fflags
  return fr.v;
}

// Per-instance pipeline state (keyed by the address of an output reference, unique per instance).
template <typename WT, typename TAGT>
struct PipeEntry { WT result; uint8_t status; TAGT tag; int lat; };
template <typename WT, typename TAGT>
struct CvfpuState { std::deque<PipeEntry<WT,TAGT>> pipe; bool outValid=false; WT outResult{}; uint8_t outStatus=0; TAGT outTag{}; };

template <typename WT, typename TAGT>
void cvfpu_core(void* key, int widthBits, int latency,
                uint8_t reset, uint8_t& req_ready, uint8_t req_valid,
                uint8_t rnd, uint8_t op, uint8_t srcFmt, uint8_t /*dstFmt*/, uint8_t intFmt,
                TAGT tag, uint32_t simdMask, WT o0, WT o1, WT o2,
                uint8_t resp_ready, uint8_t& resp_valid, WT& result, uint8_t& status,
                TAGT& respTag, uint8_t flush, uint8_t& busy) {
  static std::map<void*, CvfpuState<WT,TAGT>> S;
  auto& st = S[key];
  const size_t CAP = 8;
  if (const char* e = getenv("CVFPU_LAT")) latency = atoi(e);

  if (reset) {
    st.pipe.clear(); st.outValid = false;
    req_ready = 1; resp_valid = 0; result = (WT)0; status = 0; respTag = (TAGT)0; busy = 0;
    return;
  }
  if (flush) { st.pipe.clear(); st.outValid = false; }

  // 1. retire a consumed output
  if (st.outValid && resp_ready) st.outValid = false;
  // 2. advance pipeline latencies
  for (auto& e : st.pipe) if (e.lat > 0) e.lat--;
  // 3. move a completed entry to the output register
  if (!st.outValid && !st.pipe.empty() && st.pipe.front().lat == 0) {
    auto e = st.pipe.front(); st.pipe.pop_front();
    st.outValid = true; st.outResult = e.result; st.outStatus = e.status; st.outTag = e.tag;
    if (getenv("CVFPU_SEQ")) fprintf(stderr, "[cvseq cyc=%llu key=%p] RETIRE tag=%u result=0x%08x\n",
        ::g_sim_cycle, key, (unsigned)e.tag, (uint32_t)(uint64_t)e.result);
  }
  if (getenv("CVFPU_TRACE2")) {
    uint32_t o2lo = (uint32_t)(uint64_t)o2, o0lo = (uint32_t)(uint64_t)o0, o1lo = (uint32_t)(uint64_t)o1;
    if (req_valid || o2lo) {
      static int n2 = 0;
      if (n2 < 120) { fprintf(stderr, "[cv2 key=%p] rv=%u rr=%u op=%u o0=0x%08x o1=0x%08x o2=0x%08x respv=%u\n",
                      key, req_valid, req_ready, op, o0lo, o1lo, o2lo, resp_valid); n2++; }
    }
  }
  // 4. accept a new request
  bool canAccept = st.pipe.size() < CAP;
  req_ready = canAccept ? 1 : 0;
  if (req_valid && canAccept) {
    g_cvfpu_accepts++;
    if (getenv("CVFPU_SEQ")) fprintf(stderr, "[cvseq cyc=%llu key=%p] ACCEPT tag=%u op=%u o0=0x%08x o1=0x%08x o2=0x%08x\n",
        ::g_sim_cycle, key, (unsigned)tag, op, (uint32_t)(uint64_t)o0, (uint32_t)(uint64_t)o1, (uint32_t)(uint64_t)o2);
    int operation = (op >> 1) & 0xf;
    int op_mod = op & 1;
    int nlanes = widthBits / 32;                 // FP32 lane count (only FP32 numeric path modeled)
    WT res = (WT)0; uint8_t flags = 0;
    bool anyMask = (simdMask != 0);
    for (int i = 0; i < nlanes; i++) {
      bool active = anyMask ? ((simdMask >> i) & 1) : (i == 0);
      if (!active) continue;
      uint32_t a = (uint32_t)(o0 >> (32 * i));
      uint32_t b = (uint32_t)(o1 >> (32 * i));
      uint32_t c = (uint32_t)(o2 >> (32 * i));
      uint32_t r = compute_lane_fp32(operation, op_mod, rnd, intFmt, a, b, c, &flags);
      if (getenv("CVFPU_TRACE")) {
        static int nlog = 0;
        if (nlog < 64 && (a || b || c)) {
          fprintf(stderr, "[cvfpu] op=%d mod=%d rnd=%d lane=%d a=0x%08x b=0x%08x c=0x%08x -> r=0x%08x\n",
                  operation, op_mod, rnd, i, a, b, c, r);
          nlog++;
        }
      }
      res |= ((WT)(uint64_t)r) << (32 * i);
    }
    (void)srcFmt;
    st.pipe.push_back(PipeEntry<WT,TAGT>{res, flags, tag, latency});
  }
  // 5. drive outputs
  resp_valid = st.outValid ? 1 : 0;
  result = st.outResult; status = st.outStatus; respTag = st.outTag;
  busy = (!st.pipe.empty() || st.outValid) ? 1 : 0;
}

} // namespace

// ---- Exported overloads matching the two GSIM-emitted CVFPU signatures ------------------------

// fpDivSqrtPipe: IS_DIVSQRT=1 LANES=4 TAG=13 WIDTH=64  (operands uint64, tag uint16)
void CVFPU(int, int, int, int width,
           uint8_t reset, uint8_t& req_ready, uint8_t req_valid,
           uint8_t rnd, uint8_t op, uint8_t srcFmt, uint8_t dstFmt, uint8_t intFmt,
           uint16_t tag, uint8_t simdMask, uint64_t o0, uint64_t o1, uint64_t o2,
           uint8_t resp_ready, uint8_t& resp_valid, uint64_t& result, uint8_t& status,
           uint16_t& respTag, uint8_t flush, uint8_t& busy) {
  cvfpu_core<uint64_t, uint16_t>((void*)&req_ready, width, /*latency*/ 3,
      reset, req_ready, req_valid, rnd, op, srcFmt, dstFmt, intFmt,
      tag, (uint32_t)simdMask, o0, o1, o2, resp_ready, resp_valid, result, status, respTag, flush, busy);
}

// fpAddMulPipe: IS_DIVSQRT=0 LANES=16 TAG=25 WIDTH=256 (operands _BitInt(256), tag uint32)
void CVFPU(int, int, int, int width,
           uint8_t reset, uint8_t& req_ready, uint8_t req_valid,
           uint8_t rnd, uint8_t op, uint8_t srcFmt, uint8_t dstFmt, uint8_t intFmt,
           uint32_t tag, uint16_t simdMask, u256 o0, u256 o1, u256 o2,
           uint8_t resp_ready, uint8_t& resp_valid, u256& result, uint8_t& status,
           uint32_t& respTag, uint8_t flush, uint8_t& busy) {
  cvfpu_core<u256, uint32_t>((void*)&req_ready, width, /*latency*/ 2,
      reset, req_ready, req_valid, rnd, op, srcFmt, dstFmt, intFmt,
      tag, (uint32_t)simdMask, o0, o1, o2, resp_ready, resp_valid, result, status, respTag, flush, busy);
}
