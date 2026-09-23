/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_DEFERRED_COMMAND_BUFFER_RECORDER_H_
#define XENIA_GPU_VULKAN_DEFERRED_COMMAND_BUFFER_RECORDER_H_

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/ui/vulkan/vulkan_api.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

// Records the commands of a submission into its Vulkan command buffer on a
// thread of its own, in the order they're enqueued, so that recording them
// overlaps the command processor preparing the following ones. The commands
// are enqueued as the contents of a deferred command buffer, which is left
// empty to continue recording into.
//
// Everything a command references must stay valid until its submission
// completes, as for deferred command buffers generally, and a descriptor set
// must not be updated after a command binding it has been enqueued, since the
// binding may already have been recorded.
class DeferredCommandBufferRecorder {
 public:
  explicit DeferredCommandBufferRecorder(
      const VulkanCommandProcessor& command_processor);
  DeferredCommandBufferRecorder(const DeferredCommandBufferRecorder&) = delete;
  DeferredCommandBufferRecorder& operator=(
      const DeferredCommandBufferRecorder&) = delete;
  ~DeferredCommandBufferRecorder();

  // Starts the commands of a submission, recorded into command_buffer, which
  // must be in the recording state. All commands of the previous submission
  // must have been awaited.
  void Begin(VkCommandBuffer command_buffer);
  // Moves the commands of deferred_command_buffer to the recording thread.
  void Enqueue(DeferredCommandBuffer& deferred_command_buffer);
  // Returns once every enqueued command has been recorded into the command
  // buffer of the submission.
  void AwaitRecorded();

 private:
  void RecordEnqueued();

  const VulkanCommandProcessor& command_processor_;

  std::mutex mutex_;
  std::condition_variable enqueued_;
  std::condition_variable recorded_;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  std::deque<std::unique_ptr<DeferredCommandBuffer>> pending_;
  std::vector<std::unique_ptr<DeferredCommandBuffer>> free_;
  bool recording_ = false;
  bool shutdown_ = false;

  std::thread thread_;
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_DEFERRED_COMMAND_BUFFER_RECORDER_H_
