/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PPC_PPC_INTERPRETER_H_
#define XENIA_CPU_PPC_PPC_INTERPRETER_H_

#include <cstdint>

namespace xe::cpu {
class GuestFunction;
class ThreadState;
namespace ppc {

enum class PPCInterpreterExitReason : uint32_t {
  kCompleted,
  kUnsupportedInstruction,
  kMemoryFault,
  kInstructionBudgetExceeded,
  kInvalidEntry,
};

struct PPCInterpreterResult {
  PPCInterpreterExitReason exit_reason =
      PPCInterpreterExitReason::kInvalidEntry;
  uint32_t guest_pc = 0;
  uint32_t instruction = 0;
  uint64_t instructions_executed = 0;
};

[[nodiscard]] PPCInterpreterResult ExecutePPCInterpreter(
    GuestFunction& function, ThreadState& thread_state,
    uint64_t max_instructions);

}  // namespace ppc
}  // namespace xe::cpu

#endif  // XENIA_CPU_PPC_PPC_INTERPRETER_H_
