/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_GEARS_SHADER_OVERRIDE_H_
#define XENIA_GPU_VULKAN_GEARS_SHADER_OVERRIDE_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>

#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace ui {
namespace vulkan {
class VulkanDevice;
}
}  // namespace ui
namespace gpu {
class Shader;
namespace vulkan {

// Diagnostic-only replacement for one exact translated pixel shader. The
// selector includes the translation modification because one guest microcode
// hash may have multiple incompatible interpolator interfaces.
class GearsShaderOverride {
 public:
  static uint64_t HashUcode(const Shader& shader);

  bool Initialize(const ui::vulkan::VulkanDevice* vulkan_device);
  void Shutdown(const ui::vulkan::VulkanDevice* vulkan_device);

  void Observe(uint64_t ucode_hash, uint64_t modification);
  bool Matches(uint64_t ucode_hash, uint64_t modification) const;
  VkShaderModule Select(uint64_t ucode_hash, uint64_t modification);

 private:
  uint64_t target_hash_ = 0;
  uint64_t target_modification_ = 0;
  VkShaderModule module_ = VK_NULL_HANDLE;
  std::atomic<uint64_t> scanned_{0};
  std::atomic<uint64_t> matched_{0};
  std::atomic_flag substitution_reported_ = ATOMIC_FLAG_INIT;
  std::mutex observed_modifications_mutex_;
  std::set<uint64_t> observed_modifications_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_GEARS_SHADER_OVERRIDE_H_
