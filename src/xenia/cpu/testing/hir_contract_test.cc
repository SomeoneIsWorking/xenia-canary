/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/hir/instr.h"
#include "xenia/cpu/hir/value.h"

#include <cstring>

#include "third_party/catch/include/catch.hpp"

#if defined(XE_TEST_HIR_FATAL_CASES) && XE_PLATFORM_WIN32
#include <cstdlib>

#include "xenia/base/platform_win.h"
#endif

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_op.h"
#endif

using namespace xe::cpu::hir;

#if defined(XE_TEST_HIR_FATAL_CASES)
namespace {
void PrepareFatalTest() {
#if XE_PLATFORM_WIN32
  // Deliberate aborts must not open CRT or Windows Error Reporting dialogs.
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
}
}  // namespace

TEST_CASE("HIR rejects invalid insert type", "[.hir-fatal]") {
  PrepareFatalTest();
  Value value{}, part{}, index{};
  value.type = VEC128_TYPE;
  value.Insert(&index, &part, FLOAT32_TYPE);
  FAIL("Invalid insertion returned instead of aborting");
}

TEST_CASE("HIR rejects vector input to scalar select", "[.hir-fatal]") {
  PrepareFatalTest();
  Value value{}, part{}, control{};
  value.type = part.type = VEC128_TYPE;
  control.type = INT8_TYPE;
  control.constant.u8 = 1;
  value.Select(&part, &control);
  FAIL("Invalid scalar selection returned instead of aborting");
}
#endif

TEST_CASE("HIR opcode subset predicates retain their defaults", "[hir]") {
  for (unsigned opcode = 0; opcode <= __OPCODE_MAX_VALUE; ++opcode) {
    OpcodeInfo info{};
    info.num = static_cast<Opcode>(opcode);
    Instr instruction{};
    instruction.opcode = &info;
    const bool fake = opcode == OPCODE_NOP || opcode == OPCODE_COMMENT ||
                      opcode == OPCODE_CONTEXT_BARRIER ||
                      opcode == OPCODE_SOURCE_OFFSET;
    REQUIRE(instruction.IsFake() == fake);
    REQUIRE((std::strcmp(GetOpcodeName(info.num), "invalid opcode") == 0) ==
            (opcode == __OPCODE_MAX_VALUE));
  }
}

TEST_CASE("HIR constant insertion supports the declared integer lane widths",
          "[hir]") {
  Value value{}, part{}, index{};
  value.type = VEC128_TYPE;
  part.constant.u32 = 0xABCDEF01;
  index.constant.u8 = 0;
  value.Insert(&index, &part, INT8_TYPE);
  REQUIRE(value.constant.v128.u8[3] == 1);
  value.Insert(&index, &part, INT16_TYPE);
  REQUIRE(value.constant.v128.u16[1] == 0xEF01);
  value.Insert(&index, &part, INT32_TYPE);
  REQUIRE(value.constant.v128.u32[0] == 0xABCDEF01);
}

TEST_CASE("HIR scalar constant selection preserves each scalar representation",
          "[hir]") {
  Value value{}, part{}, control{};
  control.type = INT8_TYPE;
  for (unsigned type = INT8_TYPE; type <= FLOAT64_TYPE; ++type) {
    value.constant.u64 = 0;
    value.type = part.type = static_cast<TypeName>(type);
    part.constant.u64 = 0xFEDCBA9876543210;
    control.constant.u8 = 0;
    value.Select(&part, &control);
    REQUIRE(value.constant.u64 == 0);
    control.constant.u8 = 1;
    value.Select(&part, &control);
    const unsigned widths[] = {1, 2, 4, 8, 4, 8};
    REQUIRE(std::memcmp(&value.constant, &part.constant, widths[type]) == 0);
  }
}

#if XE_ARCH_AMD64
TEST_CASE("X64 instruction keys encode every value type consistently",
          "[hir]") {
  using xe::cpu::backend::x64::GetValueKeyType;
  using xe::cpu::backend::x64::InstrKey;
  for (unsigned type = INT8_TYPE; type <= VEC128_TYPE; ++type) {
    Value value{};
    value.type = static_cast<TypeName>(type);
    Instr instruction{};
    instruction.opcode = &OPCODE_SELECT_info;
    instruction.dest = &value;
    instruction.src1.value = &value;
    instruction.src2.value = &value;
    instruction.src3.value = &value;
    const unsigned encoded = 4 + type;
    InstrKey key(&instruction);
    REQUIRE(GetValueKeyType(value.type) == encoded);
    REQUIRE(key.dest == encoded);
    REQUIRE(key.src1 == encoded);
    REQUIRE(key.src2 == encoded);
    REQUIRE(key.src3 == encoded);
    REQUIRE(key.opcode == OPCODE_SELECT);
  }
}
#endif
