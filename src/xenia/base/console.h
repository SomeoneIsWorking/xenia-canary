/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_CONSOLE_H_
#define XENIA_BASE_CONSOLE_H_

namespace xe {

// Returns true if there is a user-visible console attached to receive stdout.
bool has_console_attached();

// Whether this executable is a CONSOLE app (xenia-gpu-vulkan-trace-dump and
// friends). Distinct from has_console_attached(), which on POSIX is
// isatty(stdin) and is therefore FALSE whenever a tool is run from a script
// with stdin redirected -- exactly how these tools are used. Reporting an
// argument error through a GUI message box in that situation hangs the tool
// behind a dialog nobody is looking at, and the dialog outlives the process
// that spawned it.
bool is_console_app();
void set_console_app(bool value);

void AttachConsole();

}  // namespace xe

#endif  // XENIA_BASE_CONSOLE_H_
