/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <CoreFoundation/CoreFoundation.h>
#include <crt_externs.h>
#include <spawn.h>
#include <sys/wait.h>

#include <string>

#include "xenia/base/logging.h"
#include "xenia/base/system.h"

namespace xe {

// Hands a URL or path to Launch Services through open(1), without a shell.
static void OpenWithLaunchServices(const std::string& target) {
  const char* arguments[] = {"open", target.c_str(), nullptr};
  pid_t child;
  int result =
      posix_spawnp(&child, "open", nullptr, nullptr,
                   const_cast<char* const*>(arguments), *_NSGetEnviron());
  if (result != 0) {
    XELOGE("Could not run open for {}: error {}", target, result);
    return;
  }
  int status;
  waitpid(child, &status, 0);
}

void LaunchWebBrowser(const std::string_view url) {
  OpenWithLaunchServices(std::string(url));
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  OpenWithLaunchServices(path.string());
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* title;
  CFOptionFlags level;
  switch (type) {
    default:
    case SimpleMessageBoxType::Help:
      title = "Xenia Help";
      level = kCFUserNotificationNoteAlertLevel;
      break;
    case SimpleMessageBoxType::Warning:
      title = "Xenia Warning";
      level = kCFUserNotificationCautionAlertLevel;
      break;
    case SimpleMessageBoxType::Error:
      title = "Xenia Error";
      level = kCFUserNotificationStopAlertLevel;
      break;
  }
  CFStringRef title_string =
      CFStringCreateWithCString(nullptr, title, kCFStringEncodingUTF8);
  CFStringRef message_string = CFStringCreateWithBytes(
      nullptr, reinterpret_cast<const UInt8*>(message.data()),
      CFIndex(message.size()), kCFStringEncodingUTF8, false);
  CFOptionFlags response;
  CFUserNotificationDisplayAlert(0, level, nullptr, nullptr, nullptr,
                                 title_string, message_string, nullptr, nullptr,
                                 nullptr, &response);
  if (message_string) {
    CFRelease(message_string);
  }
  if (title_string) {
    CFRelease(title_string);
  }
}

// Darwin schedules by thread QoS rather than process priority classes.
bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
