/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/deferred_command_buffer_recorder.h"

#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/threading.h"

namespace xe {
namespace gpu {
namespace vulkan {

DeferredCommandBufferRecorder::DeferredCommandBufferRecorder(
    const VulkanCommandProcessor& command_processor)
    : command_processor_(command_processor),
      thread_([this] { RecordEnqueued(); }) {}

DeferredCommandBufferRecorder::~DeferredCommandBufferRecorder() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
  }
  enqueued_.notify_one();
  thread_.join();
}

void DeferredCommandBufferRecorder::Begin(VkCommandBuffer command_buffer) {
  std::lock_guard<std::mutex> lock(mutex_);
  assert_true(pending_.empty() && !recording_);
  command_buffer_ = command_buffer;
}

void DeferredCommandBufferRecorder::Enqueue(
    DeferredCommandBuffer& deferred_command_buffer) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    assert_true(command_buffer_ != VK_NULL_HANDLE);
    std::unique_ptr<DeferredCommandBuffer> commands;
    if (free_.empty()) {
      // The stream capacity is exchanged with the deferred command buffer, so
      // it doesn't need to be preallocated.
      commands = std::make_unique<DeferredCommandBuffer>(command_processor_, 0);
    } else {
      commands = std::move(free_.back());
      free_.pop_back();
    }
    commands->Swap(deferred_command_buffer);
    pending_.push_back(std::move(commands));
  }
  enqueued_.notify_one();
}

void DeferredCommandBufferRecorder::AwaitRecorded() {
  std::unique_lock<std::mutex> lock(mutex_);
  recorded_.wait(lock, [this] { return pending_.empty() && !recording_; });
}

void DeferredCommandBufferRecorder::RecordEnqueued() {
  xe::threading::set_name("GPU Command Recording");
  std::unique_lock<std::mutex> lock(mutex_);
  while (true) {
    enqueued_.wait(lock, [this] { return shutdown_ || !pending_.empty(); });
    if (pending_.empty()) {
      // Shutting down with everything recorded.
      return;
    }
    std::unique_ptr<DeferredCommandBuffer> commands =
        std::move(pending_.front());
    pending_.pop_front();
    VkCommandBuffer command_buffer = command_buffer_;
    recording_ = true;
    lock.unlock();
    commands->Execute(command_buffer);
    commands->Reset();
    lock.lock();
    free_.push_back(std::move(commands));
    recording_ = false;
    if (pending_.empty()) {
      recorded_.notify_all();
    }
  }
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
