/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_interpreter.h"

#include <cstdint>

#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe::cpu::ppc {
namespace {

[[nodiscard]] bool CanRead(Memory& memory, uint32_t address, uint32_t size) {
  if (size == 0) {
    return false;
  }
  const uint64_t last_address = static_cast<uint64_t>(address) + size - 1;
  if (last_address > UINT32_MAX) {
    return false;
  }
  auto* first_heap = memory.LookupHeap(address);
  auto* last_heap = memory.LookupHeap(static_cast<uint32_t>(last_address));
  if (first_heap == nullptr || first_heap != last_heap) {
    return false;
  }
  if (first_heap->QueryRangeAccess(address,
                                   static_cast<uint32_t>(last_address)) ==
      memory::PageAccess::kNoAccess) {
    return false;
  }
  for (uint32_t offset = 0; offset < size; ++offset) {
    if (memory.LookupVirtualMappedRange(address + offset) != nullptr) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] uint32_t LoadInstruction(Memory& memory, uint32_t address) {
  const auto* bytes = memory.TranslateVirtual<const uint8_t*>(address);
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

}  // namespace

PPCInterpreterResult ExecutePPCInterpreter(GuestFunction& function,
                                           ThreadState& thread_state,
                                           uint64_t max_instructions) {
  PPCInterpreterResult result;
  result.guest_pc = function.address();
  if (max_instructions == 0 || !function.has_end_address()) {
    result.exit_reason =
        max_instructions == 0
            ? PPCInterpreterExitReason::kInstructionBudgetExceeded
            : PPCInterpreterExitReason::kInvalidEntry;
    return result;
  }

  Memory& memory = *thread_state.memory();
  auto* context = thread_state.context();
  uint32_t pc = function.address();
  while (result.instructions_executed < max_instructions) {
    if (pc < function.address() || pc > function.end_address() ||
        !CanRead(memory, pc, sizeof(uint32_t))) {
      result.exit_reason = PPCInterpreterExitReason::kMemoryFault;
      result.guest_pc = pc;
      return result;
    }
    const uint32_t instruction = LoadInstruction(memory, pc);
    result.guest_pc = pc;
    result.instruction = instruction;
    ++result.instructions_executed;

    if (instruction == 0x4E800020) {
      result.exit_reason = PPCInterpreterExitReason::kCompleted;
      return result;
    }

    if (LookupOpcode(instruction) != PPCOpcode::lswi) {
      result.exit_reason = PPCInterpreterExitReason::kUnsupportedInstruction;
      return result;
    }

    PPCDecodeData decoded{};
    decoded.address = pc;
    decoded.code = instruction;
    const uint32_t target_register = decoded.X.RT();
    const uint32_t base_register = decoded.X.RA();
    const uint32_t byte_count = decoded.X.RB() == 0 ? 32 : decoded.X.RB();
    const uint32_t base_address =
        base_register == 0 ? 0
                           : static_cast<uint32_t>(context->r[base_register]);
    if (!CanRead(memory, base_address, byte_count)) {
      result.exit_reason = PPCInterpreterExitReason::kMemoryFault;
      return result;
    }

    const auto* bytes = memory.TranslateVirtual<const uint8_t*>(base_address);
    for (uint32_t index = 0; index < byte_count; ++index) {
      const uint32_t target = (target_register + index / 4) & 31;
      if (index % 4 == 0) {
        context->r[target] = 0;
      }
      context->r[target] = (context->r[target] << 8) | bytes[index];
    }
    pc += 4;
  }

  result.exit_reason = PPCInterpreterExitReason::kInstructionBudgetExceeded;
  result.guest_pc = pc;
  return result;
}

}  // namespace xe::cpu::ppc
