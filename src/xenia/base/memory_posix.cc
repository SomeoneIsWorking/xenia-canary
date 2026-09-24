/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/memory.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string.h"

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <atomic>

#include "third_party/fmt/include/fmt/format.h"
#endif

#if XE_PLATFORM_ANDROID
#include <dlfcn.h>
#include <linux/ashmem.h>
#include <string.h>
#include <sys/ioctl.h>

#include "xenia/base/main_android.h"
#endif

namespace xe {
namespace memory {

#if XE_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (xe::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ =
          reinterpret_cast<decltype(android_ASharedMemory_create_)>(
              dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() { return getpagesize(); }
size_t allocation_granularity() { return page_size(); }

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      return PROT_READ | PROT_EXEC;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

PageAccess ToXeniaProtectFlags(const char* protection) {
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == 'x') {
    return PageAccess::kExecuteReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == 'x') {
    return PageAccess::kExecuteReadOnly;
  }
  if (protection[0] == 'r' && protection[1] == 'w' && protection[2] == '-') {
    return PageAccess::kReadWrite;
  }
  if (protection[0] == 'r' && protection[1] == '-' && protection[2] == '-') {
    return PageAccess::kReadOnly;
  }
  return PageAccess::kNoAccess;
}

bool IsWritableExecutableMemorySupported() {
#if XE_PLATFORM_MAC
  // Apple silicon maps memory writable and executable at once only with
  // MAP_JIT, which a shared mapping cannot take; code is written through a
  // separate read-write view of the same pages instead.
  return false;
#else
  return true;
#endif
}

struct MappedFileRange {
  uintptr_t region_begin;
  uintptr_t region_end;
};

std::vector<MappedFileRange> mapped_file_ranges;
std::mutex g_mapped_file_ranges_mutex;

// Maps at base_address only if the range is free, as MAP_FIXED_NOREPLACE does.
// Without that flag the kernel treats base_address as a hint and may place the
// mapping elsewhere; such a mapping is released and the request fails.
static void* MapAtRequestedAddress(void* base_address, size_t length, int prot,
                                   int flags, int fd, off_t offset) {
#ifdef MAP_FIXED_NOREPLACE
  if (base_address != nullptr) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#endif
  void* result = mmap(base_address, length, prot, flags, fd, offset);
  if (result == MAP_FAILED) {
    return nullptr;
  }
  if (base_address != nullptr && result != base_address) {
    munmap(result, length);
    return nullptr;
  }
  return result;
}

void* AllocFixed(void* base_address, size_t length,
                 AllocationType allocation_type, PageAccess access) {
  // mmap does not support reserve / commit, so ignore allocation_type.
  uint32_t prot = ToPosixProtectFlags(access);
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

  if (base_address != nullptr) {
    if (allocation_type == AllocationType::kCommit) {
      if (Protect(base_address, length, access)) {
        return base_address;
      }
      return nullptr;
    }
  }

  return MapAtRequestedAddress(base_address, length, prot, flags, -1, 0);
}

bool DeallocFixed(void* base_address, size_t length,
                  DeallocationType deallocation_type) {
  const auto region_begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t region_end =
      reinterpret_cast<uintptr_t>(base_address) + length;

  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (const auto& mapped_range : mapped_file_ranges) {
    if (region_begin >= mapped_range.region_begin &&
        region_end <= mapped_range.region_end) {
      switch (deallocation_type) {
        case DeallocationType::kDecommit:
          return Protect(base_address, length, PageAccess::kNoAccess);
        case DeallocationType::kRelease:
          return false;
        default:
          assert_unhandled_case(deallocation_type);
      }
    }
  }

  switch (deallocation_type) {
    case DeallocationType::kDecommit:
      return Protect(base_address, length, PageAccess::kNoAccess);
    case DeallocationType::kRelease:
      return munmap(base_address, length) == 0;
    default:
      assert_unhandled_case(deallocation_type);
  }
}

bool Protect(void* base_address, size_t length, PageAccess access,
             PageAccess* out_old_access) {
  if (out_old_access) {
    size_t length_copy = length;
    QueryProtect(base_address, length_copy, *out_old_access);
  }

  uint32_t prot = ToPosixProtectFlags(access);
  return mprotect(base_address, length, prot) == 0;
}

#if XE_PLATFORM_MAC
static PageAccess ToXeniaProtectFlags(vm_prot_t protection) {
  bool read = protection & VM_PROT_READ;
  bool write = protection & VM_PROT_WRITE;
  bool execute = protection & VM_PROT_EXECUTE;
  if (read && write && execute) {
    return PageAccess::kExecuteReadWrite;
  }
  if (read && execute) {
    return PageAccess::kExecuteReadOnly;
  }
  if (read && write) {
    return PageAccess::kReadWrite;
  }
  if (read) {
    return PageAccess::kReadOnly;
  }
  return PageAccess::kNoAccess;
}

// The region containing address, from the kernel's map of this task.
static bool QueryRegion(mach_vm_address_t address, mach_vm_address_t& begin,
                        mach_vm_address_t& end, PageAccess& access) {
  mach_vm_address_t region_address = address;
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info;
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  if (mach_vm_region(mach_task_self(), &region_address, &region_size,
                     VM_REGION_BASIC_INFO_64,
                     reinterpret_cast<vm_region_info_t>(&info), &count,
                     &object_name) != KERN_SUCCESS ||
      region_address > address) {
    return false;
  }
  begin = region_address;
  end = region_address + region_size;
  access = ToXeniaProtectFlags(info.protection);
  return true;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  auto address = reinterpret_cast<mach_vm_address_t>(base_address);
  mach_vm_address_t begin, end;
  if (!QueryRegion(address, begin, end, access_out)) {
    return false;
  }
  // Extend over the following regions with the same protection.
  mach_vm_address_t next_begin, next_end;
  PageAccess next_access;
  while (QueryRegion(end, next_begin, next_end, next_access) &&
         next_begin == end && next_access == access_out) {
    end = next_end;
  }
  length = size_t(end - address);
  return true;
}
#else
bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
  // No generic POSIX solution exists. The Linux solution should work on all
  // Linux kernel based OS, including Android.
  std::ifstream memory_maps;
  memory_maps.open("/proc/self/maps", std::ios_base::in);
  std::string maps_entry_string;

  while (std::getline(memory_maps, maps_entry_string)) {
    std::stringstream entry_stream(maps_entry_string);
    uintptr_t map_region_begin, map_region_end;
    char separator;
    char protection[5];  // 4 chars (e.g., "r-xp") + null terminator

    entry_stream >> std::hex >> map_region_begin >> separator >>
        map_region_end >> protection;

    if (map_region_begin <= reinterpret_cast<uintptr_t>(base_address) &&
        map_region_end > reinterpret_cast<uintptr_t>(base_address)) {
      length = map_region_end - reinterpret_cast<uintptr_t>(base_address);

      access_out = ToXeniaProtectFlags(protection);

      // Look at the next consecutive mappings
      while (std::getline(memory_maps, maps_entry_string)) {
        std::stringstream next_entry_stream(maps_entry_string);
        uintptr_t next_map_region_begin, next_map_region_end;
        char next_protection[5];  // 4 chars (e.g., "r-xp") + null terminator

        next_entry_stream >> std::hex >> next_map_region_begin >> separator >>
            next_map_region_end >> next_protection;
        if (map_region_end == next_map_region_begin &&
            access_out == ToXeniaProtectFlags(next_protection)) {
          length =
              next_map_region_end - reinterpret_cast<uintptr_t>(base_address);
          continue;
        }
        break;
      }

      memory_maps.close();
      return true;
    }
  }

  memory_maps.close();
  return false;
}
#endif  // XE_PLATFORM_MAC

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path,
                                          size_t length, PageAccess access,
                                          bool commit) {
#if XE_PLATFORM_ANDROID
  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? sharedmem_fd : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), xe::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return ashmem_fd;
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
#if XE_PLATFORM_MAC
  // Darwin limits shared memory names to PSHMNAMLEN (31) characters. The name
  // is removed below before anything could reopen it, so a short name unique
  // to this process and call is enough.
  static std::atomic<uint32_t> darwin_mapping_serial{0};
  std::filesystem::path full_path =
      fmt::format("/xe{}.{}", getpid(), darwin_mapping_serial++);
#else
  auto full_path = "/" / path;
#endif
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate(ret, length) < 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  // UNLINK THE NAME NOW, while still holding the fd.
  //
  // The mapping is kept alive by the descriptor, not by the name, so removing
  // the name here costs nothing and makes the object impossible to leak: the
  // kernel reclaims it when the last descriptor closes, however the process
  // ends. Nothing reopens these by name -- every caller maps through the handle
  // returned here.
  //
  // This replaces an atexit/at_quick_exit cleanup, which cannot run for the
  // case that actually leaks. A crash left behind one 4.6 GB guest-memory
  // object and one 256 MB code cache PER RUN, and 43 of each accumulated in
  // /dev/shm across two days of oracle runs until they hit the user's 6.3 GB
  // tmpfs quota. After that every new run took SIGBUS while writing its code
  // cache during "Initializing Processor" -- a crash caused entirely by the
  // debris of earlier crashes, which is as confusing as it sounds.
  shm_unlink(full_path.c_str());
  return ret;
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle,
                            const std::filesystem::path& path) {
  // The name was already unlinked at creation, so closing the descriptor is
  // the whole of the cleanup: the object goes away with the last mapping.
  close(handle);
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length,
                  PageAccess access, size_t file_offset) {
  uint32_t prot = ToPosixProtectFlags(access);

  void* result = MapAtRequestedAddress(base_address, length, prot, MAP_SHARED,
                                       handle, off_t(file_offset));

  if (result != nullptr) {
    std::lock_guard guard(g_mapped_file_ranges_mutex);
    mapped_file_ranges.push_back(
        {reinterpret_cast<uintptr_t>(result),
         reinterpret_cast<uintptr_t>(result) + length});
    return result;
  }

  return nullptr;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address,
                   size_t length) {
  std::lock_guard guard(g_mapped_file_ranges_mutex);
  for (auto mapped_range = mapped_file_ranges.begin();
       mapped_range != mapped_file_ranges.end();) {
    if (mapped_range->region_begin ==
            reinterpret_cast<uintptr_t>(base_address) &&
        mapped_range->region_end ==
            reinterpret_cast<uintptr_t>(base_address) + length) {
      mapped_file_ranges.erase(mapped_range);
      return munmap(base_address, length) == 0;
    }
    ++mapped_range;
  }
  // TODO: Implement partial file unmapping.
  assert_always("Error: Partial unmapping of files not yet supported.");
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace xe
