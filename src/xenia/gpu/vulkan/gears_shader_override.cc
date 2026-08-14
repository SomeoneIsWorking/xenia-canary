/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/gears_shader_override.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/gpu/shader.h"
#include "xenia/ui/vulkan/vulkan_device.h"
#include "xenia/ui/vulkan/vulkan_util.h"

namespace xe {
namespace gpu {
namespace vulkan {
namespace {

bool ParseHex64(const std::string& text, uint64_t& value_out) {
  if (text.size() != 16) {
    return false;
  }
  char* end = nullptr;
  value_out = std::strtoull(text.c_str(), &end, 16);
  return end == text.c_str() + text.size();
}

}  // namespace

uint64_t GearsShaderOverride::HashUcode(const Shader& shader) {
  uint64_t hash = UINT64_C(0xCBF29CE484222325);
  for (uint32_t dword : shader.ucode_data()) {
    const uint8_t bytes[4] = {uint8_t(dword >> 24), uint8_t(dword >> 16),
                              uint8_t(dword >> 8), uint8_t(dword)};
    for (uint8_t byte : bytes) {
      hash ^= byte;
      hash *= UINT64_C(0x100000001B3);
    }
  }
  return hash;
}

bool GearsShaderOverride::Initialize(
    const ui::vulkan::VulkanDevice* vulkan_device) {
  const char* setting = std::getenv("GEARS_ORACLE_PS_OVERRIDE_SPV");
  if (!setting || !*setting) {
    return true;
  }

  const std::string value(setting);
  const size_t first_separator = value.find(':');
  const size_t second_separator = value.find(':', first_separator + 1);
  if (first_separator == std::string::npos ||
      second_separator == std::string::npos ||
      !ParseHex64(value.substr(0, first_separator), target_hash_) ||
      !ParseHex64(value.substr(first_separator + 1,
                               second_separator - first_separator - 1),
                  target_modification_)) {
    XELOGE("gears: GEARS_ORACLE_PS_OVERRIDE_SPV must be "
           "<16-hex-hash>:<16-hex-modification>:<path>; NOTHING will be "
           "substituted");
    target_hash_ = 0;
    return false;
  }

  const std::filesystem::path path(value.substr(second_separator + 1));
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    XELOGE("gears: diagnostic shader {} could not be opened; NOTHING will be "
           "substituted", path.string());
    target_hash_ = 0;
    return false;
  }
  const std::streamoff length = input.tellg();
  if (length < 5 * std::streamoff(sizeof(uint32_t)) ||
      length % std::streamoff(sizeof(uint32_t)) != 0) {
    XELOGE("gears: diagnostic shader {} is not a complete SPIR-V module ({} "
           "bytes); NOTHING will be substituted", path.string(), length);
    target_hash_ = 0;
    return false;
  }

  std::vector<uint32_t> words(size_t(length) / sizeof(uint32_t));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(words.data()), length);
  if (!input || words.front() != UINT32_C(0x07230203)) {
    XELOGE("gears: diagnostic shader {} has invalid SPIR-V contents; NOTHING "
           "will be substituted", path.string());
    target_hash_ = 0;
    return false;
  }

  module_ = ui::vulkan::util::CreateShaderModule(
      vulkan_device, words.data(), words.size() * sizeof(uint32_t));
  if (module_ == VK_NULL_HANDLE) {
    XELOGE("gears: diagnostic shader {} was rejected by Vulkan; NOTHING will "
           "be substituted", path.string());
    target_hash_ = 0;
    return false;
  }
  XELOGW("gears: DIAGNOSTIC ARMED: pixel shader {:016X} modification {:016X} "
         "will use {} ({} SPIR-V words); matching output is an instrument "
         "reading, not an oracle render", target_hash_, target_modification_,
         path.string(), words.size());
  return true;
}

void GearsShaderOverride::Shutdown(
    const ui::vulkan::VulkanDevice* vulkan_device) {
  if (!target_hash_) {
    return;
  }
  const uint64_t scanned = scanned_.load(std::memory_order_relaxed);
  const uint64_t matched = matched_.load(std::memory_order_relaxed);
  if (matched) {
    XELOGI("gears: diagnostic shader override matched {} of {} pixel pipeline "
           "creation(s)", matched, scanned);
  } else {
    XELOGW("gears: diagnostic shader override scanned {} pixel pipeline "
           "creation(s), matched 0; target {:016X} modification {:016X} was "
           "NOT observed", scanned, target_hash_, target_modification_);
  }
  {
    std::lock_guard<std::mutex> lock(observed_modifications_mutex_);
    if (observed_modifications_.empty()) {
      XELOGW("gears: target shader hash {:016X} was absent from all {} scanned "
             "pixel pipeline creation(s)", target_hash_, scanned);
    } else {
      for (uint64_t modification : observed_modifications_) {
        XELOGI("gears: target shader hash {:016X} was observed with "
               "modification {:016X}", target_hash_, modification);
      }
    }
  }
  if (module_ != VK_NULL_HANDLE) {
    const ui::vulkan::VulkanDevice::Functions& dfn =
        vulkan_device->functions();
    dfn.vkDestroyShaderModule(vulkan_device->device(), module_, nullptr);
    module_ = VK_NULL_HANDLE;
  }
  target_hash_ = 0;
}

VkShaderModule GearsShaderOverride::Select(uint64_t ucode_hash,
                                           uint64_t modification) {
  if (!target_hash_) {
    return VK_NULL_HANDLE;
  }
  scanned_.fetch_add(1, std::memory_order_relaxed);
  if (!Matches(ucode_hash, modification)) {
    return VK_NULL_HANDLE;
  }
  matched_.fetch_add(1, std::memory_order_relaxed);
  if (!substitution_reported_.test_and_set(std::memory_order_relaxed)) {
    XELOGW("gears: DIAGNOSTIC SUBSTITUTION: pixel shader {:016X} modification "
           "{:016X} is not using the title's translated module", ucode_hash,
           modification);
  }
  return module_;
}

void GearsShaderOverride::Observe(uint64_t ucode_hash,
                                  uint64_t modification) {
  if (!target_hash_ || ucode_hash != target_hash_) {
    return;
  }
  std::lock_guard<std::mutex> lock(observed_modifications_mutex_);
  if (observed_modifications_.insert(modification).second) {
    XELOGI("gears: target shader {:016X} reached a draw with modification "
           "{:016X}", ucode_hash, modification);
  }
}

bool GearsShaderOverride::Matches(uint64_t ucode_hash,
                                  uint64_t modification) const {
  return module_ != VK_NULL_HANDLE && ucode_hash == target_hash_ &&
         modification == target_modification_;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
