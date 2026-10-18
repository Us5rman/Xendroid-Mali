/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
#define XENIA_CPU_BACKEND_A64_A64_EMITTER_H_

#include <functional>
#include <unordered_map>
#include <vector>

#include "xenia/base/arena.h"
#include "xenia/base/vec128.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache_base.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/function_trace_data.h"
#include "xenia/cpu/hir/hir_builder.h"
#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/value.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/memory.h"

#include "xbyak_aarch64.h"

namespace xe {
namespace cpu {
class Processor;
}  // namespace cpu
}  // namespace xe

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {
using namespace arm64;
class A64Backend;
class A64CodeCache;

enum class FPCRMode : uint32_t { Unknown, Fpu, Vmx };

// Unfortunately due to the design of xbyak we have to pass this to the ctor.
class XbyakA64Allocator : public Xbyak_aarch64::Allocator {
 public:
  virtual bool useProtect() const { return false; }
};

class A64Emitter;
using TailEmitCallback =
    std::function<void(A64Emitter& e, Xbyak_aarch64::Label& lbl)>;
struct TailEmitter {
  Xbyak_aarch64::Label label;
  uint32_t alignment;
  TailEmitCallback func;
};

class A64Emitter : public Xbyak_aarch64::CodeGenerator {
 public:
  A64Emitter(A64Backend* backend, XbyakA64Allocator* allocator);
  virtual ~A64Emitter();

  Processor* processor() const { return processor_; }
  A64Backend* backend() const { return backend_; }

  bool Emit(GuestFunction* function, hir::HIRBuilder* builder,
            uint32_t debug_info_flags, FunctionDebugInfo* debug_info,
            void** out_code_address, size_t* out_code_size,
            std::vector<SourceMapEntry>* out_source_map);

 public:
  // Reserved: sp, x19 (backend context), x20 (context), x21 (membase)
  // Scratch: x0-x18 (caller-saved), v0-v3
  // Available GPRs for register allocator: x22-x28
  static constexpr int GPR_COUNT = 7;
  // Available VEC regs: v4-v15, v16-v31
  static constexpr int VEC_COUNT = 28;
  static constexpr size_t kStashOffset = 32;

  // A value that reached codegen without a register assignment indexes the maps
  // out of bounds, which otherwise surfaces far away as an unencodable-register
  // exception. Report the culprit here, where it is still in hand.
  static uint32_t MapReg(const hir::Value* v, const uint32_t* map, int count,
                         const char* set_name);

  static void SetupReg(const hir::Value* v, Xbyak_aarch64::WReg& r) {
    r = Xbyak_aarch64::WReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::XReg& r) {
    r = Xbyak_aarch64::XReg(MapReg(v, gpr_reg_map_, GPR_COUNT, "gpr"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::SReg& r) {
    r = Xbyak_aarch64::SReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::DReg& r) {
    r = Xbyak_aarch64::DReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::QReg& r) {
    r = Xbyak_aarch64::QReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }
  static void SetupReg(const hir::Value* v, Xbyak_aarch64::VReg& r) {
    r = Xbyak_aarch64::VReg(MapReg(v, vec_reg_map_, VEC_COUNT, "vec"));
  }

  Xbyak_aarch64::Label& epilog_label() { return *epilog_label_; }

  FunctionDebugInfo* debug_info() const { return debug_info_; }
  size_t stack_size() const { return stack_size_; }
  // Guest start address of the function being emitted.
  uint32_t current_guest_function() const { return current_guest_function_; }

  void MarkSourceOffset(const hir::Instr* i);

  void DebugBreak();
  void Trap(uint16_t trap_type = 0);
  void UnimplementedInstr(const hir::Instr* i);

  void Call(const hir::Instr* instr, GuestFunction* function);
  void CallIndirect(const hir::Instr* instr, int reg_index);
  void CallExtern(const hir::Instr* instr, const Function* function);
  void CallNative(void* fn);
  void CallNativeSafe(void* fn);
  void SetReturnAddress(uint64_t value);

  // Backend context register = x19.
  // Points to A64BackendContext (immediately before PPCContext in memory).
  const Xbyak_aarch64::XReg& GetBackendCtxReg() const { return x19; }
  // Context register = x20.
  const Xbyak_aarch64::XReg& GetContextReg() const { return x20; }
  // Memory base register = x21.
  const Xbyak_aarch64::XReg& GetMembaseReg() const { return x21; }

  void ReloadMembase();

  void PushStackpoint();
  void PopStackpoint();
  void EnsureSynchronizedGuestAndHostStack();

  static void HandleStackpointOverflowError(ppc::PPCContext* context);

  void ForgetFpcrMode() {
    if (fpcr_mode_ == FPCRMode::Vmx) {
      ChangeFpcrMode(FPCRMode::Fpu);
    }
    fpcr_mode_ = FPCRMode::Unknown;
  }
  bool ChangeFpcrMode(FPCRMode new_mode, bool already_set = false);
  bool IsFeatureEnabled(uint64_t feature_flag) const {
    return (feature_flags_ & feature_flag) == feature_flag;
  }

  Xbyak_aarch64::Label& AddToTail(TailEmitCallback callback,
                                  uint32_t alignment = 0);
  Xbyak_aarch64::Label& NewCachedLabel();

  // Emits a cooperative-scheduler preemption safepoint: yields the fiber once
  // the context's preempt_requested flag is raised. Only valid at a block head.
  // guest_address is stamped into the context for wedge diagnosis when
  // log_safepoint_pc is on; 0 means unknown.
  void EmitPreemptCheck(uint32_t guest_address = 0);

  // ARM64 conditional branches (cbz/cbnz: ±1 MiB, tbz/tbnz: ±32 KiB,
  // b.cond: ±1 MiB) can fall short of their target in large guest functions.
  // b.cond, cbz and cbnz are emitted direct while near_branches() - a guest
  // function they don't reach across throws ERR_LABEL_IS_TOO_FAR and is
  // emitted again without; otherwise these shadows emit the safe pattern
  // `<inverse> skip; b target; skip:`, routing the long branch through
  // unconditional b (±128 MiB), at an extra instruction and a taken branch on
  // the path that doesn't branch. tbz/tbnz always take the safe pattern. A
  // rarely taken branch to tail code is cheapest written as `b(cond, tail)`:
  // direct, it falls through. The int64_t-immediate overloads remain
  // available via the using-declarations for hand-tuned thunks that pass
  // literal byte offsets.
  using Xbyak_aarch64::CodeGenerator::b;
  using Xbyak_aarch64::CodeGenerator::cbnz;
  using Xbyak_aarch64::CodeGenerator::cbz;
  using Xbyak_aarch64::CodeGenerator::tbnz;
  using Xbyak_aarch64::CodeGenerator::tbz;
  void b(const Xbyak_aarch64::Cond cond, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::WReg& rt, const Xbyak_aarch64::Label& label);
  void cbnz(const Xbyak_aarch64::XReg& rt, const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
           const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::WReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);
  void tbnz(const Xbyak_aarch64::XReg& rt, uint32_t imm,
            const Xbyak_aarch64::Label& label);

  bool near_branches() const { return near_branches_; }

  // Single-instruction conditional branches for forward targets that are
  // PROVABLY within range because the label is bound a bounded number of
  // instructions later within the same sequence/helper emission (e.g.
  // intra-sequence fast-path skips). Callers must guarantee the bound:
  // ±1 MiB for b_near/cbz_near/cbnz_near. (In near_branches() mode the
  // shadows above are direct already.)
  void b_near(const Xbyak_aarch64::Cond cond,
              const Xbyak_aarch64::Label& label) {
    CodeGenerator::b(cond, label);
  }
  void cbz_near(const Xbyak_aarch64::WReg& rt,
                const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbz(rt, label);
  }
  void cbz_near(const Xbyak_aarch64::XReg& rt,
                const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbz(rt, label);
  }
  void cbnz_near(const Xbyak_aarch64::WReg& rt,
                 const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbnz(rt, label);
  }
  void cbnz_near(const Xbyak_aarch64::XReg& rt,
                 const Xbyak_aarch64::Label& label) {
    CodeGenerator::cbnz(rt, label);
  }

  // A compare whose only reader is the branch right after it makes no
  // boolean: the branch reads the flags the compare set. Keyed on the
  // register the cset would have written; cleared on read. Only instructions
  // that PassesHandoffs may sit between the two, and the branch must take it,
  // or Emit fails the function.
  void MarkFusedCompareBranch(int dest_reg, Xbyak_aarch64::Cond cond) {
    fused_cmp_branch_reg_ = dest_reg;
    fused_cmp_branch_cond_ = cond;
  }
  bool ConsumeFusedCompareBranch(int dest_reg, Xbyak_aarch64::Cond* out_cond) {
    if (dest_reg < 0 || fused_cmp_branch_reg_ != dest_reg) {
      return false;
    }
    *out_cond = fused_cmp_branch_cond_;
    fused_cmp_branch_reg_ = -1;
    return true;
  }

  // SOURCE_OFFSET, CONTEXT_BARRIER and NOP emit no code that touches NZCV
  // (here SOURCE_OFFSET only records a source map entry). COMMENT does not
  // pass: it calls TraceString when instruction tracing is on.
  static bool PassesHandoffs(const hir::Instr* instr) {
    switch (instr->GetOpcodeNum()) {
      case hir::OPCODE_SOURCE_OFFSET:
      case hir::OPCODE_CONTEXT_BARRIER:
      case hir::OPCODE_NOP:
        return true;
      default:
        return false;
    }
  }

  // Get or create a xbyak_aarch64 label for a HIR label ID.
  Xbyak_aarch64::Label& GetLabel(uint32_t label_id);

  // Get or create a pool slot for a v128 constant with no immediate form.
  Xbyak_aarch64::Label& GetV128ConstLabel(const vec128_t& value);

  XexModule* GuestModule() { return guest_module_; }

 protected:
  void* Emplace(const EmitFunctionInfo& func_info,
                GuestFunction* function = nullptr);
  // Drops the code buffer, tail entries and both label pools. Both the success
  // path and a failed compile must run it, or stale labels carry over.
  void ResetPerFunctionState();
  // Gives every local of the function its stack offset (turning it into a
  // constant) and returns the frame size.
  size_t LayOutLocals(hir::HIRBuilder* builder);
  bool Emit(hir::HIRBuilder* builder, size_t stack_size,
            EmitFunctionInfo& func_info);
  // Whether a guest function has to be emitted again with long-range branches.
  bool IsNearBranchOutOfRange(const Xbyak_aarch64::Error& e) const;

  // Emit the pending v128 literals, branching over them when code follows.
  bool FlushV128ConstPool(bool branch_over);
  // Plant an island, only safe where a branch and data cannot split a sequence.
  bool MaybeFlushV128ConstPool();

 protected:
  Processor* processor_ = nullptr;
  A64Backend* backend_ = nullptr;
  A64CodeCache* code_cache_ = nullptr;
  XbyakA64Allocator* allocator_ = nullptr;
  XexModule* guest_module_ = nullptr;
  uint64_t feature_flags_ = 0;
  uint32_t current_guest_function_ = 0;
  bool log_guest_call_ = false;

  Xbyak_aarch64::Label* epilog_label_ = nullptr;

  hir::Instr* current_instr_ = nullptr;

  FunctionDebugInfo* debug_info_ = nullptr;
  uint32_t debug_info_flags_ = 0;
  FunctionTraceData* trace_data_ = nullptr;
  Arena source_map_arena_;

  size_t stack_size_ = 0;

  static const uint32_t gpr_reg_map_[GPR_COUNT];
  static const uint32_t vec_reg_map_[VEC_COUNT];

  std::vector<TailEmitter> tail_code_;
  std::vector<Xbyak_aarch64::Label*> label_cache_;

  // v128 constants needing a literal, with labels owned by label_cache_.
  std::vector<std::pair<vec128_t, Xbyak_aarch64::Label*>> v128_consts_;
  // Code offset of the ldr that opened the pending pool.
  size_t v128_consts_first_use_ = 0;

  // Map from HIR label IDs to xbyak_aarch64 Labels.
  std::unordered_map<uint32_t, Xbyak_aarch64::Label*> label_map_;

  // The compare-branch handoff between adjacent sequences; see
  // MarkFusedCompareBranch.
  void ResetSequenceHandoffs() { fused_cmp_branch_reg_ = -1; }
  bool sequence_handoff_pending() const { return fused_cmp_branch_reg_ >= 0; }
  int fused_cmp_branch_reg_ = -1;
  Xbyak_aarch64::Cond fused_cmp_branch_cond_ = Xbyak_aarch64::EQ;

  FPCRMode fpcr_mode_ = FPCRMode::Unknown;
  bool synchronize_stack_on_next_instruction_ = false;
  // Set per attempt while a guest function is emitted (the helper thunks keep
  // the long-range form).
  bool near_branches_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_EMITTER_H_
