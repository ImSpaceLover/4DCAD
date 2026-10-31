// Native file dialogs (Windows: common dialogs; Linux: zenity if available).
// Return an empty string when cancelled or unavailable.
#pragma once
#include <string>

namespace fd {
std::string openFileDialog(const char* title);
std::string saveFileDialog(const char* title, const std::string& defaultName);
bool nativeDialogsAvailable();
}  // namespace fd
