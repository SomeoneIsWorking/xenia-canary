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

[[nodiscard]] bool CanAccess(Memory& memory, uint32_t address, uint32_t size,
                             bool write) {
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
  const uint32_t access = static_cast<uint32_t>(first_heap->QueryRangeAccess(
      address, static_cast<uint32_t>(last_address)));
  const uint32_t required = write ? 0x2 : 0x1;
  if ((access & required) != required) {
    return false;
  }
  for (uint32_t offset = 0; offset < size; ++offset) {
    if (memory.LookupVirtualMappedRange(address + offset) != nullptr) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool CanRead(Memory& memory, uint32_t address, uint32_t size) {
  return CanAccess(memory, address, size, false);
}

[[nodiscard]] bool CanWrite(Memory& memory, uint32_t address, uint32_t size) {
  return CanAccess(memory, address, size, true);
}

[[nodiscard]] uint32_t LoadInstruction(Memory& memory, uint32_t address) {
  const auto* bytes = memory.TranslateVirtual<const uint8_t*>(address);
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

[[nodiscard]] uint32_t LoadBigEndian(Memory& memory, uint32_t address,
                                     uint32_t size) {
  const auto* bytes = memory.TranslateVirtual<const uint8_t*>(address);
  uint32_t value = 0;
  for (uint32_t index = 0; index < size; ++index) {
    value = (value << 8) | bytes[index];
  }
  return value;
}

void StoreBigEndian(Memory& memory, uint32_t address, uint32_t value,
                    uint32_t size) {
  auto* bytes = memory.TranslateVirtual<uint8_t*>(address);
  for (uint32_t index = 0; index < size; ++index) {
    const uint32_t shift = (size - index - 1) * 8;
    bytes[index] = static_cast<uint8_t>(value >> shift);
  }
}

[[nodiscard]] uint32_t BaseValue(const PPCContext& context,
                                 uint32_t base_register) {
  return base_register == 0 ? 0
                            : static_cast<uint32_t>(context.r[base_register]);
}

[[nodiscard]] uint32_t EffectiveAddress(const PPCContext& context,
                                        uint32_t base_register,
                                        int32_t displacement) {
  const uint32_t base = BaseValue(context, base_register);
  return base + static_cast<uint32_t>(displacement);
}

[[nodiscard]] bool ConditionBit(const PPCContext& context, uint32_t bit_index) {
  return ((context.cr() >> (31 - bit_index)) & 1) != 0;
}

void SetConditionField(PPCContext& context, uint32_t field, bool less,
                       bool greater, bool equal) {
  switch (field) {
    case 0:
      context.cr0.cr0_lt = less;
      context.cr0.cr0_gt = greater;
      context.cr0.cr0_eq = equal;
      break;
    case 1:
      context.cr1.cr1_fx = less;
      context.cr1.cr1_fex = greater;
      context.cr1.cr1_vx = equal;
      break;
    case 2:
      context.cr2.cr2_0 = less;
      context.cr2.cr2_1 = greater;
      context.cr2.cr2_2 = equal;
      break;
    case 3:
      context.cr3.cr3_0 = less;
      context.cr3.cr3_1 = greater;
      context.cr3.cr3_2 = equal;
      break;
    case 4:
      context.cr4.cr4_0 = less;
      context.cr4.cr4_1 = greater;
      context.cr4.cr4_2 = equal;
      break;
    case 5:
      context.cr5.cr5_0 = less;
      context.cr5.cr5_1 = greater;
      context.cr5.cr5_2 = equal;
      break;
    case 6:
      context.cr6.cr6_all_equal = less;
      context.cr6.cr6_1 = greater;
      context.cr6.cr6_none_equal = equal;
      break;
    case 7:
      context.cr7.cr7_0 = less;
      context.cr7.cr7_1 = greater;
      context.cr7.cr7_2 = equal;
      break;
    default:
      break;
  }
}

void Compare(PPCContext& context, uint32_t field, uint32_t lhs, uint32_t rhs,
             bool is_signed) {
  const bool less = is_signed
                        ? static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs)
                        : lhs < rhs;
  const bool greater =
      is_signed ? static_cast<int32_t>(lhs) > static_cast<int32_t>(rhs)
                : lhs > rhs;
  SetConditionField(context, field, less, greater, lhs == rhs);
}

[[nodiscard]] bool BranchTaken(PPCContext& context, uint32_t bo, uint32_t bi) {
  bool taken = true;
  if ((bo & 0x04) == 0) {
    context.ctr -= 1;
    const bool counter_is_zero = context.ctr == 0;
    taken = (bo & 0x02) != 0 ? counter_is_zero : !counter_is_zero;
  }
  if ((bo & 0x10) == 0) {
    const bool condition = ConditionBit(context, bi);
    taken = taken && ((bo & 0x08) != 0 ? condition : !condition);
  }
  return taken;
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

    PPCDecodeData decoded{};
    decoded.address = pc;
    decoded.code = instruction;
    switch (LookupOpcode(instruction)) {
      case PPCOpcode::addi:
        context->r[decoded.D.RT()] = BaseValue(*context, decoded.D.RA()) +
                                     static_cast<uint32_t>(decoded.D.SIMM());
        break;
      case PPCOpcode::addis:
        context->r[decoded.D.RT()] =
            BaseValue(*context, decoded.D.RA()) +
            (static_cast<uint32_t>(decoded.D.SIMM()) << 16);
        break;
      case PPCOpcode::mulli:
        context->r[decoded.D.RT()] = BaseValue(*context, decoded.D.RA()) *
                                     static_cast<uint32_t>(decoded.D.SIMM());
        break;
      case PPCOpcode::ori:
        context->r[decoded.D.RA()] =
            static_cast<uint32_t>(context->r[decoded.D.RS()]) |
            decoded.D.UIMM();
        break;
      case PPCOpcode::oris:
        context->r[decoded.D.RA()] =
            static_cast<uint32_t>(context->r[decoded.D.RS()]) |
            (decoded.D.UIMM() << 16);
        break;
      case PPCOpcode::xori:
        context->r[decoded.D.RA()] =
            static_cast<uint32_t>(context->r[decoded.D.RS()]) ^
            decoded.D.UIMM();
        break;
      case PPCOpcode::xoris:
        context->r[decoded.D.RA()] =
            static_cast<uint32_t>(context->r[decoded.D.RS()]) ^
            (decoded.D.UIMM() << 16);
        break;
      case PPCOpcode::andix:
        context->r[decoded.D.RA()] =
            static_cast<uint32_t>(context->r[decoded.D.RS()]) &
            decoded.D.UIMM();
        break;
      case PPCOpcode::orx:
        context->r[decoded.X.RT()] =
            static_cast<uint32_t>(context->r[decoded.X.RA()]) |
            static_cast<uint32_t>(context->r[decoded.X.RB()]);
        break;
      case PPCOpcode::xorx:
        context->r[decoded.X.RT()] =
            static_cast<uint32_t>(context->r[decoded.X.RA()]) ^
            static_cast<uint32_t>(context->r[decoded.X.RB()]);
        break;
      case PPCOpcode::andx:
        context->r[decoded.X.RT()] =
            static_cast<uint32_t>(context->r[decoded.X.RA()]) &
            static_cast<uint32_t>(context->r[decoded.X.RB()]);
        break;
      case PPCOpcode::lwz:
      case PPCOpcode::lbz:
      case PPCOpcode::lhz: {
        const uint32_t size = LookupOpcode(instruction) == PPCOpcode::lwz   ? 4
                              : LookupOpcode(instruction) == PPCOpcode::lhz ? 2
                                                                            : 1;
        const uint32_t address =
            EffectiveAddress(*context, decoded.D.RA(), decoded.D.SIMM());
        if (!CanRead(memory, address, size)) {
          result.exit_reason = PPCInterpreterExitReason::kMemoryFault;
          return result;
        }
        context->r[decoded.D.RT()] = LoadBigEndian(memory, address, size);
        break;
      }
      case PPCOpcode::stw:
      case PPCOpcode::stb:
      case PPCOpcode::sth: {
        const uint32_t size = LookupOpcode(instruction) == PPCOpcode::stw   ? 4
                              : LookupOpcode(instruction) == PPCOpcode::sth ? 2
                                                                            : 1;
        const uint32_t address =
            EffectiveAddress(*context, decoded.D.RA(), decoded.D.SIMM());
        if (!CanWrite(memory, address, size)) {
          result.exit_reason = PPCInterpreterExitReason::kMemoryFault;
          return result;
        }
        StoreBigEndian(memory, address,
                       static_cast<uint32_t>(context->r[decoded.D.RS()]), size);
        break;
      }
      case PPCOpcode::cmpi:
        Compare(*context, decoded.D.CRFD(),
                static_cast<uint32_t>(context->r[decoded.D.RA()]),
                static_cast<uint32_t>(decoded.D.SIMM()), true);
        break;
      case PPCOpcode::cmpli:
        Compare(*context, decoded.D.CRFD(),
                static_cast<uint32_t>(context->r[decoded.D.RA()]),
                decoded.D.UIMM(), false);
        break;
      case PPCOpcode::bx: {
        const uint32_t target = decoded.I.ADDR();
        if ((target & 3) != 0 || target < function.address() ||
            target > function.end_address()) {
          result.exit_reason =
              PPCInterpreterExitReason::kUnsupportedInstruction;
          return result;
        }
        if (decoded.I.LK()) {
          context->lr = pc + 4;
        }
        pc = target;
        continue;
      }
      case PPCOpcode::bcx: {
        if (!BranchTaken(*context, decoded.B.BO(), decoded.B.BI())) {
          pc += 4;
          continue;
        }
        const uint32_t target = decoded.B.ADDR();
        if ((target & 3) != 0 || target < function.address() ||
            target > function.end_address()) {
          result.exit_reason =
              PPCInterpreterExitReason::kUnsupportedInstruction;
          return result;
        }
        if (decoded.B.LK()) {
          context->lr = pc + 4;
        }
        pc = target;
        continue;
      }
      case PPCOpcode::lswi: {
        const uint32_t target_register = decoded.X.RT();
        const uint32_t base_register = decoded.X.RA();
        const uint32_t byte_count = decoded.X.RB() == 0 ? 32 : decoded.X.RB();
        const uint32_t base_address =
            base_register == 0
                ? 0
                : static_cast<uint32_t>(context->r[base_register]);
        if (!CanRead(memory, base_address, byte_count)) {
          result.exit_reason = PPCInterpreterExitReason::kMemoryFault;
          return result;
        }

        const auto* bytes =
            memory.TranslateVirtual<const uint8_t*>(base_address);
        for (uint32_t index = 0; index < byte_count; ++index) {
          const uint32_t target = (target_register + index / 4) & 31;
          if (index % 4 == 0) {
            context->r[target] = 0;
          }
          context->r[target] = (context->r[target] << 8) | bytes[index];
        }
        break;
      }
      default:
        result.exit_reason = PPCInterpreterExitReason::kUnsupportedInstruction;
        return result;
    }
    pc += 4;
  }

  result.exit_reason = PPCInterpreterExitReason::kInstructionBudgetExceeded;
  result.guest_pc = pc;
  return result;
}

}  // namespace xe::cpu::ppc
