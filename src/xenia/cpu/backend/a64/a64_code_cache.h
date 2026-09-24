/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
#define XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_

#include <memory>

#include "xenia/cpu/backend/code_cache_base.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64CodeCache : public CodeCacheBase<A64CodeCache> {
 public:
  ~A64CodeCache() override = default;

  static std::unique_ptr<A64CodeCache> Create();

  virtual bool Initialize();

  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }

  // Nothing below 4 GB is reachable on every host (a 64-bit Darwin process
  // reserves it all), so emitted code addresses the indirection table and
  // its entries from bases kept in the backend context.
  static constexpr bool kRelocatableLayout = true;

  // CRTP hooks for CodeCacheBase.
  void FillCode(void* write_address, size_t size);
  void FlushCodeRange(void* address, size_t size);

  // Virtual for platform-specific overrides (_win.cc / _posix.cc).
  virtual UnwindReservation RequestUnwindReservation(uint8_t* entry_address) {
    return UnwindReservation();
  }
  virtual void PlaceCode(uint32_t guest_address, void* machine_code,
                         const EmitFunctionInfo& func_info,
                         void* code_execute_address,
                         UnwindReservation unwind_reservation) {}

 protected:
  A64CodeCache() = default;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
