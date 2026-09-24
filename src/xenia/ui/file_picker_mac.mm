/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#import <AppKit/AppKit.h>

#include <memory>

#include "xenia/base/string.h"
#include "xenia/ui/file_picker.h"

namespace xe {
namespace ui {

// A modal NSOpenPanel or NSSavePanel. Like the other pickers, Show runs on the
// thread that owns the application's user interface.
class MacFilePicker : public FilePicker {
 public:
  bool Show(Window* parent_window) override;
};

bool MacFilePicker::Show(Window* parent_window) {
  @autoreleasepool {
    NSSavePanel* panel;
    if (mode() == Mode::kSave) {
      panel = [NSSavePanel savePanel];
      if (!file_name().empty()) {
        panel.nameFieldStringValue = [NSString stringWithUTF8String:file_name().c_str()];
      }
    } else {
      NSOpenPanel* open_panel = [NSOpenPanel openPanel];
      open_panel.canChooseFiles = type() == Type::kFile;
      open_panel.canChooseDirectories = type() == Type::kDirectory;
      open_panel.allowsMultipleSelection = multi_selection();
      panel = open_panel;
    }
    panel.title = [NSString stringWithUTF8String:title().c_str()];
    if ([panel runModal] != NSModalResponseOK) {
      return false;
    }
    std::vector<std::filesystem::path> selected;
    if (mode() == Mode::kSave) {
      selected.push_back(xe::to_path(std::string(panel.URL.path.UTF8String)));
    } else {
      for (NSURL* url in static_cast<NSOpenPanel*>(panel).URLs) {
        selected.push_back(xe::to_path(std::string(url.path.UTF8String)));
      }
    }
    set_selected_files(std::move(selected));
    return true;
  }
}

std::unique_ptr<FilePicker> FilePicker::Create() { return std::make_unique<MacFilePicker>(); }

}  // namespace ui
}  // namespace xe
