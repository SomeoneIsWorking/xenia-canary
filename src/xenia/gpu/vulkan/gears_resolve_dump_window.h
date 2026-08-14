#ifndef XENIA_GPU_VULKAN_GEARS_RESOLVE_DUMP_WINDOW_H_
#define XENIA_GPU_VULKAN_GEARS_RESOLVE_DUMP_WINDOW_H_

#include <cstdint>

namespace xe::gpu::vulkan {

// Selection state is separate from the numeric frame index because frame zero
// is a real frame in a one-frame GPU trace. Treating zero as "not selected"
// made exact-state trace resolve dumps impossible.
class GearsResolveDumpWindow {
 public:
  void Select(uint64_t first_frame) {
    first_frame_ = first_frame;
    selected_ = true;
  }

  void SetFrameCount(uint32_t frame_count) { frame_count_ = frame_count; }

  bool selected() const { return selected_; }
  uint64_t first_frame() const { return first_frame_; }

  bool Contains(uint64_t frame) const {
    return selected_ && frame >= first_frame_ &&
           frame - first_frame_ < frame_count_;
  }

 private:
  uint64_t first_frame_ = 0;
  uint32_t frame_count_ = 1;
  bool selected_ = false;
};

}  // namespace xe::gpu::vulkan

#endif  // XENIA_GPU_VULKAN_GEARS_RESOLVE_DUMP_WINDOW_H_
