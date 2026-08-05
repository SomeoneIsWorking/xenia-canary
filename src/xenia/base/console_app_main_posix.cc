/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

int main(int argc, char** argv) {
  xe::ConsoleAppEntryInfo entry_info = xe::GetConsoleAppEntryInfo();

  if (!entry_info.transparent_options) {
    cvar::ParseLaunchArguments(argc, argv, entry_info.positional_usage,
                               entry_info.positional_options);
  }

  // Initialize logging. Needs parsed cvars.
  //
  // NOT optional, though it was commented out here while the windowed entry
  // point (ui/windowed_app_main_posix.cc) calls it. Logger::AppendLine blocks
  // when the logger was never initialised, so the first thing that logs hangs
  // the process -- for the GPU trace tools that is the Vulkan loader chattering
  // through the debug messenger during EnumeratePhysicalDevices, long before
  // any trace file is opened, and with no output to say so.
  xe::InitializeLogging(entry_info.name);

  std::vector<std::string> args;
  for (int n = 0; n < argc; n++) {
    args.emplace_back(argv[n]);
  }

  int result = entry_info.entry_point(args);

  xe::ShutdownLogging();

  return result;
}
