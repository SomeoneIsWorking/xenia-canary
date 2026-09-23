/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/main_win.h"

DECLARE_bool(log_to_stdout);

// A wide character entry point is required for functions like _get_wpgmptr.
int wmain(int argc_ignored, wchar_t** argv_ignored) {
  xe::ConsoleAppEntryInfo entry_info = xe::GetConsoleAppEntryInfo();

  std::vector<std::string> args;
  if (!xe::ParseWin32LaunchArguments(entry_info.transparent_options,
                                     entry_info.positional_usage,
                                     entry_info.positional_options, &args)) {
    return EXIT_FAILURE;
  }

  // A transparent app hands its arguments, and with them its standard output,
  // to the entry point's own parser: a test suite's Catch session lists and
  // reports there, and a log line would corrupt that protocol. Its log still
  // goes to the log file.
  if (entry_info.transparent_options) {
    cvars::log_to_stdout = false;
  }

  int result = xe::InitializeWin32App(entry_info.name);
  if (result) {
    return result;
  }

  result = entry_info.entry_point(args);

  xe::ShutdownWin32App();

  return result;
}
