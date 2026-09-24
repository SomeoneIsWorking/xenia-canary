/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_CODE_CACHE_BASE_H_
#define XENIA_CPU_BACKEND_CODE_CACHE_BASE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"

namespace xe {
namespace cpu {
namespace backend {

struct EmitFunctionInfo {
  struct _code_size {
    size_t prolog;
    size_t body;
    size_t epilog;
    size_t tail;
    size_t total;
  } code_size;
  size_t prolog_stack_alloc_offset;
  size_t stack_size;
#if XE_ARCH_ARM64
  // Offset from SP where x30 (LR) is saved.  ARM64 callees save LR
  // explicitly at varying offsets; the unwind info generator needs this
  // to tell the unwinder where to find the return address.
  // Currently only used by the POSIX DWARF .eh_frame generator; the
  // Windows .xdata format encodes LR saves differently.  Adds 8 bytes
  // to the struct on ARM64 Windows builds where it is unused, to avoid
  // #if clutter in the backend/emitter code that sets it.
  size_t lr_save_offset;
#endif
};

// CRTP base class for JIT code caches. Contains all platform-independent
// logic for memory management, indirection tables, code placement, and
// function lookup. Derived classes provide architecture-specific hooks:
//
//   void FillCode(void* address, size_t size)
//     Fill unused code regions with trap instructions (0xCC / BRK).
//
//   void FlushCodeRange(void* address, size_t size)
//     Flush I-cache after writing code (no-op on x86, required on ARM64).
//
//   UnwindReservation RequestUnwindReservation(uint8_t* entry_address)
//     Reserve space for platform-specific unwind info.
//
//   void PlaceCode(uint32_t guest_address, void* machine_code,
//                  const EmitFunctionInfo& func_info,
//                  void* code_execute_address,
//                  UnwindReservation unwind_reservation)
//     Register unwind info and perform platform-specific post-placement.
//
//   void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
//                     void* code_execute_address, size_t code_size)
//     Optional hook called after code is placed outside the critical section
//     (used for VTune integration on x64). Default is no-op.
//
//   static constexpr bool kRelocatableLayout
//     Optional. False (the default) maps the indirection table and generated
//     code at the fixed addresses below 4 GB that x64 code embeds; an
//     indirection slot then holds the host code address itself. True lets the
//     host place both anywhere; a slot then holds the code's offset from the
//     generated-code execute base, and emitted code adds that base back.
template <typename Derived>
class CodeCacheBase : public CodeCache {
 public:
  ~CodeCacheBase() override {
    if (indirection_table_base_) {
      xe::memory::DeallocFixed(indirection_table_base_, kIndirectionTableSize,
                               xe::memory::DeallocationType::kRelease);
    }
    if (mapping_ != xe::memory::kFileMappingHandleInvalid) {
      if (generated_code_write_base_ &&
          generated_code_write_base_ != generated_code_execute_base_) {
        xe::memory::UnmapFileView(mapping_, generated_code_write_base_,
                                  kGeneratedCodeSize);
      }
      if (generated_code_execute_base_) {
        xe::memory::UnmapFileView(mapping_, generated_code_execute_base_,
                                  kGeneratedCodeSize);
      }
      xe::memory::CloseFileMappingHandle(mapping_, file_name_);
      mapping_ = xe::memory::kFileMappingHandleInvalid;
    }
  }

  const std::filesystem::path& file_name() const override { return file_name_; }
  uintptr_t execute_base_address() const override {
    return reinterpret_cast<uintptr_t>(generated_code_execute_base_);
  }
  size_t total_size() const override { return kGeneratedCodeSize; }

  bool has_indirection_table() { return indirection_table_base_ != nullptr; }

  // The host address of the indirection slot of guest address 0; the slot of
  // a guest address is this plus the address. Only slots of guest addresses
  // at or above kIndirectionTableBase exist.
  uintptr_t indirection_table_origin() const {
    return reinterpret_cast<uintptr_t>(indirection_table_base_) -
           kIndirectionTableBase;
  }

  // The host address an indirection entry of 0 denotes: an entry is the
  // target's offset from this base.
  uintptr_t indirection_entry_base() const { return indirection_entry_base_; }

  uint32_t EncodeGuestEntry(const void* host_code) const override {
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(host_code) - indirection_entry_base_;
    assert_true(offset <= UINT32_MAX);
    return static_cast<uint32_t>(offset);
  }

  void set_indirection_default(uint32_t default_value) {
    indirection_default_value_ = default_value;
  }

  void AddIndirection(uint32_t guest_address, uint32_t host_address) {
    if (!indirection_table_base_) {
      return;
    }
    uint32_t* indirection_slot = reinterpret_cast<uint32_t*>(
        indirection_table_base_ + (guest_address - kIndirectionTableBase));
    *indirection_slot = host_address;
  }

  void InvalidateGuestEntry(uint32_t guest_address) override {
    AddIndirection(guest_address, indirection_default_value_);
  }

  uint32_t LookupGuestEntry(uint32_t guest_address) const override {
    if (!indirection_table_base_) {
      return 0;
    }
    auto* slot = reinterpret_cast<const uint32_t*>(
        indirection_table_base_ + (guest_address - kIndirectionTableBase));
    return *slot;
  }

  bool RedirectGuestEntry(uint32_t guest_address,
                          uint32_t host_address) override {
    if (!indirection_table_base_ || !host_address) {
      return false;
    }
    AddIndirection(guest_address, host_address);
    return true;
  }

  void CommitExecutableRange(uint32_t guest_low, uint32_t guest_high) {
    if (!indirection_table_base_) {
      return;
    }
    xe::memory::AllocFixed(
        indirection_table_base_ + (guest_low - kIndirectionTableBase),
        guest_high - guest_low, xe::memory::AllocationType::kCommit,
        xe::memory::PageAccess::kReadWrite);
    uint32_t* p = reinterpret_cast<uint32_t*>(indirection_table_base_);
    for (uint32_t address = guest_low; address < guest_high; ++address) {
      p[(address - kIndirectionTableBase) / 4] = indirection_default_value_;
    }
  }

  void PlaceHostCode(uint32_t guest_address, void* machine_code,
                     const EmitFunctionInfo& func_info,
                     void*& code_execute_address_out,
                     void*& code_write_address_out) {
    PlaceGuestCode(guest_address, machine_code, func_info, nullptr,
                   code_execute_address_out, code_write_address_out);
  }

  void PlaceGuestCode(uint32_t guest_address, void* machine_code,
                      const EmitFunctionInfo& func_info,
                      GuestFunction* function_info,
                      void*& code_execute_address_out,
                      void*& code_write_address_out) {
    using namespace xe::literals;
    uint8_t* code_execute_address;
    {
      auto global_lock = global_critical_region_.Acquire();

      code_execute_address =
          generated_code_execute_base_ + generated_code_offset_;
      code_execute_address_out = code_execute_address;
      uint8_t* code_write_address =
          generated_code_write_base_ + generated_code_offset_;
      code_write_address_out = code_write_address;
      generated_code_offset_ += xe::round_up(func_info.code_size.total, 16);

      auto tail_write_address =
          generated_code_write_base_ + generated_code_offset_;

      auto unwind_reservation = self().RequestUnwindReservation(
          generated_code_write_base_ + generated_code_offset_);
      generated_code_offset_ += xe::round_up(unwind_reservation.data_size, 16);

      auto end_write_address =
          generated_code_write_base_ + generated_code_offset_;

      size_t high_mark = generated_code_offset_;

      generated_code_map_.emplace_back(
          (uint64_t(code_execute_address - generated_code_execute_base_)
           << 32) |
              generated_code_offset_,
          function_info);

      // Commit memory if needed.
      EnsureCommitted(high_mark);

      // Copy code.
      std::memcpy(code_write_address, machine_code, func_info.code_size.total);

      // Fill unused tail/unwind gap with arch-specific trap instructions.
      self().FillCode(
          tail_write_address,
          static_cast<size_t>(end_write_address - tail_write_address));

      // Flush I-cache for code and fill regions at the addresses they execute
      // from, which differ from the written ones when the views are separate.
      self().FlushCodeRange(code_execute_address, func_info.code_size.total);
      if (tail_write_address < end_write_address) {
        self().FlushCodeRange(
            ExecuteAddressOf(tail_write_address),
            static_cast<size_t>(end_write_address - tail_write_address));
      }

      // Platform-specific unwind registration.
      self().PlaceCode(guest_address, machine_code, func_info,
                       code_execute_address, unwind_reservation);
    }

    // Post-placement hook (e.g. VTune notification).
    self().OnCodePlaced(guest_address, function_info, code_execute_address,
                        func_info.code_size.total);

    // Fix up indirection table.
    if (guest_address && indirection_table_base_) {
      AddIndirection(guest_address, EncodeGuestEntry(code_execute_address));
    }
  }

  // Reserves length bytes of executable memory beside the generated code, for
  // code the host writes itself later. The block is filled with traps; write
  // through write_address_out and flush the execute range after each change.
  void ReserveHostCode(size_t length, void*& execute_address_out,
                       void*& write_address_out) {
    size_t high_mark;
    uint8_t* write_address;
    {
      auto global_lock = global_critical_region_.Acquire();
      write_address = generated_code_write_base_ + generated_code_offset_;
      generated_code_offset_ += xe::round_up(length, 16);
      high_mark = generated_code_offset_;
    }
    EnsureCommitted(high_mark);
    self().FillCode(write_address, length);
    execute_address_out = ExecuteAddressOf(write_address);
    write_address_out = write_address;
    self().FlushCodeRange(execute_address_out, length);
  }

  uint32_t PlaceData(const void* data, size_t length) {
    size_t high_mark;
    uint8_t* data_address = nullptr;
    {
      auto global_lock = global_critical_region_.Acquire();
      data_address = generated_code_write_base_ + generated_code_offset_;
      generated_code_offset_ += xe::round_up(length, 16);
      high_mark = generated_code_offset_;
    }
    EnsureCommitted(high_mark);
    std::memcpy(data_address, data, length);
    return uint32_t(uintptr_t(data_address));
  }

  GuestFunction* LookupFunction(uint64_t host_pc) override {
    if (generated_code_map_.empty()) {
      return nullptr;
    }
    const uint64_t code_base = execute_base_address();
    const uint64_t code_end = code_base + total_size();
    if (host_pc < code_base || host_pc >= code_end) {
      return nullptr;
    }
    uint32_t key = uint32_t(host_pc - code_base);
    void* fn_entry = std::bsearch(
        &key, generated_code_map_.data(), generated_code_map_.size(),
        sizeof(std::pair<uint32_t, Function*>),
        [](const void* key_ptr, const void* element_ptr) {
          auto key = *reinterpret_cast<const uint32_t*>(key_ptr);
          auto element =
              reinterpret_cast<const std::pair<uint64_t, GuestFunction*>*>(
                  element_ptr);
          if (key < (element->first >> 32)) {
            return -1;
          } else if (key > uint32_t(element->first)) {
            return 1;
          } else {
            return 0;
          }
        });
    if (fn_entry) {
      return reinterpret_cast<const std::pair<uint64_t, GuestFunction*>*>(
                 fn_entry)
          ->second;
    } else {
      return nullptr;
    }
  }

 protected:
  static constexpr size_t kIndirectionTableSize = 0x1FFFFFFF;
  static constexpr uintptr_t kIndirectionTableBase = 0x80000000;
  static constexpr size_t kGeneratedCodeSize = 0x0FFFFFFF;
  static constexpr uintptr_t kGeneratedCodeExecuteBase = 0xA0000000;
  static const uintptr_t kGeneratedCodeWriteBase =
      kGeneratedCodeExecuteBase + kGeneratedCodeSize + 1;
  static constexpr size_t kMaximumFunctionCount = 1000000;
  static constexpr bool kRelocatableLayout = false;

  struct UnwindReservation {
    size_t data_size = 0;
    size_t table_slot = 0;
    uint8_t* entry_address = 0;
  };

  CodeCacheBase() = default;

  bool Initialize() {
    // A relocatable layout lets the host choose every address.
    constexpr bool kRelocatable = Derived::kRelocatableLayout;
    void* const table_request =
        kRelocatable ? nullptr : reinterpret_cast<void*>(kIndirectionTableBase);
    void* const execute_request =
        kRelocatable ? nullptr
                     : reinterpret_cast<void*>(kGeneratedCodeExecuteBase);
    void* const write_request =
        kRelocatable ? nullptr
                     : reinterpret_cast<void*>(kGeneratedCodeWriteBase);
    indirection_table_base_ = reinterpret_cast<uint8_t*>(
        xe::memory::AllocFixed(table_request, kIndirectionTableSize,
                               xe::memory::AllocationType::kReserve,
                               xe::memory::PageAccess::kReadWrite));
    if (!indirection_table_base_) {
      XELOGE("Unable to allocate code cache indirection table");
      XELOGE(
          "This is likely because the {:X}-{:X} range is in use by some "
          "other system DLL",
          static_cast<uint64_t>(kIndirectionTableBase),
          kIndirectionTableBase + kIndirectionTableSize);
    }

    file_name_ =
        fmt::format("xenia_code_cache_{}", Clock::QueryHostTickCount());
    mapping_ = xe::memory::CreateFileMappingHandle(
        file_name_, kGeneratedCodeSize,
        xe::memory::PageAccess::kExecuteReadWrite, false);
    if (mapping_ == xe::memory::kFileMappingHandleInvalid) {
      XELOGE("Unable to create code cache mmap");
      return false;
    }

    if (xe::memory::IsWritableExecutableMemoryPreferred()) {
      generated_code_execute_base_ =
          reinterpret_cast<uint8_t*>(xe::memory::MapFileView(
              mapping_, execute_request, kGeneratedCodeSize,
              xe::memory::PageAccess::kExecuteReadWrite, 0));
      generated_code_write_base_ = generated_code_execute_base_;
      if (!generated_code_execute_base_ || !generated_code_write_base_) {
        XELOGE("Unable to allocate code cache generated code storage");
        XELOGE(
            "This is likely because the {:X}-{:X} range is in use by some "
            "other system DLL",
            uint64_t(kGeneratedCodeExecuteBase),
            uint64_t(kGeneratedCodeExecuteBase + kGeneratedCodeSize));
        return false;
      }
    } else {
      generated_code_execute_base_ = reinterpret_cast<uint8_t*>(
          xe::memory::MapFileView(mapping_, execute_request, kGeneratedCodeSize,
                                  xe::memory::PageAccess::kExecuteReadOnly, 0));
      generated_code_write_base_ = reinterpret_cast<uint8_t*>(
          xe::memory::MapFileView(mapping_, write_request, kGeneratedCodeSize,
                                  xe::memory::PageAccess::kReadWrite, 0));
      if (!generated_code_execute_base_ || !generated_code_write_base_) {
        XELOGE("Unable to allocate code cache generated code storage");
        XELOGE(
            "This is likely because the {:X}-{:X} and {:X}-{:X} ranges are "
            "in use by some other system DLL",
            uint64_t(kGeneratedCodeExecuteBase),
            uint64_t(kGeneratedCodeExecuteBase + kGeneratedCodeSize),
            uint64_t(kGeneratedCodeWriteBase),
            uint64_t(kGeneratedCodeWriteBase + kGeneratedCodeSize));
        return false;
      }
    }

    // Fixed-layout entries are absolute addresses below 4 GB.
    indirection_entry_base_ =
        kRelocatable ? reinterpret_cast<uintptr_t>(generated_code_execute_base_)
                     : 0;
    if (kRelocatable) {
      XELOGI(
          "Code cache: indirection table at {:016X}, generated code executes "
          "at {:016X} (written at {:016X})",
          reinterpret_cast<uint64_t>(indirection_table_base_),
          reinterpret_cast<uint64_t>(generated_code_execute_base_),
          reinterpret_cast<uint64_t>(generated_code_write_base_));
    }

    generated_code_map_.reserve(kMaximumFunctionCount);
    return true;
  }

  // Default no-op for the OnCodePlaced hook.
  void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
                    void* code_execute_address, size_t code_size) {}

  std::filesystem::path file_name_;
  xe::memory::FileMappingHandle mapping_ =
      xe::memory::kFileMappingHandleInvalid;
  xe::global_critical_region global_critical_region_;
  uint32_t indirection_default_value_ = 0xFEEDF00D;
  uint8_t* indirection_table_base_ = nullptr;
  uint8_t* generated_code_execute_base_ = nullptr;
  uint8_t* generated_code_write_base_ = nullptr;
  uintptr_t indirection_entry_base_ = 0;
  size_t generated_code_offset_ = 0;
  std::atomic<size_t> generated_code_commit_mark_ = {0};
  std::vector<std::pair<uint64_t, GuestFunction*>> generated_code_map_;

 private:
  Derived& self() { return static_cast<Derived&>(*this); }

  uint8_t* ExecuteAddressOf(uint8_t* write_address) const {
    return generated_code_execute_base_ +
           (write_address - generated_code_write_base_);
  }

  void EnsureCommitted(size_t high_mark) {
    using namespace xe::literals;
    size_t old_commit_mark, new_commit_mark;
    do {
      old_commit_mark = generated_code_commit_mark_;
      if (high_mark <= old_commit_mark) {
        break;
      }
      new_commit_mark = old_commit_mark + 16_MiB;
      if (generated_code_execute_base_ == generated_code_write_base_) {
        xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                               xe::memory::AllocationType::kCommit,
                               xe::memory::PageAccess::kExecuteReadWrite);
      } else {
        xe::memory::AllocFixed(generated_code_execute_base_, new_commit_mark,
                               xe::memory::AllocationType::kCommit,
                               xe::memory::PageAccess::kExecuteReadOnly);
        xe::memory::AllocFixed(generated_code_write_base_, new_commit_mark,
                               xe::memory::AllocationType::kCommit,
                               xe::memory::PageAccess::kReadWrite);
      }
    } while (generated_code_commit_mark_.compare_exchange_weak(
        old_commit_mark, new_commit_mark));
  }
};

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_CODE_CACHE_BASE_H_
