/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/export_resolver.h"

#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::test {
namespace {

struct CallbackState {
  uint32_t calls = 0;
};

void TestCallback(ppc::PPCContext*, kernel::KernelState*, void* context) {
  ++static_cast<CallbackState*>(context)->calls;
}

}  // namespace

TEST_CASE("ExportResolver preserves typed function callbacks",
          "[export_resolver]") {
  ExportResolver resolver;
  Export first_export(1, Export::Type::kFunction, "FirstCallback");
  Export second_export(2, Export::Type::kFunction, "SecondCallback");
  std::vector<Export*> table{nullptr, &first_export, &second_export};
  resolver.RegisterTable("test.xex", &table);
  CallbackState first_state;
  CallbackState second_state;
  resolver.SetFunctionMapping("test.xex", 1, TestCallback, &first_state);
  resolver.SetFunctionMapping("test.xex", 2, TestCallback, &second_state);

  Export* resolved_first = resolver.GetExportByOrdinal("test.xex", 1);
  Export* resolved_second = resolver.GetExportByOrdinal("test.xex", 2);
  REQUIRE(resolved_first == &first_export);
  REQUIRE(resolved_second == &second_export);
  REQUIRE(resolved_first->is_implemented());
  REQUIRE(resolved_second->is_implemented());
  REQUIRE(resolved_first->function_data.trampoline == TestCallback);
  REQUIRE(resolved_second->function_data.trampoline == TestCallback);
  REQUIRE(resolved_first->function_data.callback_context == &first_state);
  REQUIRE(resolved_second->function_data.callback_context == &second_state);

  resolved_first->function_data.trampoline(
      nullptr, nullptr, resolved_first->function_data.callback_context);
  resolved_second->function_data.trampoline(
      nullptr, nullptr, resolved_second->function_data.callback_context);
  resolved_second->function_data.trampoline(
      nullptr, nullptr, resolved_second->function_data.callback_context);
  REQUIRE(first_state.calls == 1);
  REQUIRE(second_state.calls == 2);
}

}  // namespace xe::cpu::test
