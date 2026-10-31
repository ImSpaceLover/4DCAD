#include "filedialog.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace fd {

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(n, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(n - 1);
  return w;
}
static std::string narrow(const wchar_t* w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  std::string s(n, 0);
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  s.resize(n - 1);
  return s;
}
static const wchar_t* FILTER = L"4DCAD files (*.json)\0*.json\0All files (*.*)\0*.*\0";

bool nativeDialogsAvailable() { return true; }

std::string openFileDialog(const char* title) {
  wchar_t buf[4096] = L"";
  std::wstring t = widen(title);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.lpstrFilter = FILTER;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 4096;
  ofn.lpstrTitle = t.c_str();
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameW(&ofn) ? narrow(buf) : std::string();
}

std::string saveFileDialog(const char* title, const std::string& defaultName) {
  wchar_t buf[4096] = L"";
  std::wstring d = widen(defaultName);
  wcsncpy(buf, d.c_str(), 4095);
  std::wstring t = widen(title);
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof ofn;
  ofn.lpstrFilter = FILTER;
  ofn.lpstrFile = buf;
  ofn.nMaxFile = 4096;
  ofn.lpstrTitle = t.c_str();
  ofn.lpstrDefExt = L"json";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetSaveFileNameW(&ofn) ? narrow(buf) : std::string();
}
#else
static std::string run(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return {};
  char buf[4096];
  std::string out;
  while (fgets(buf, sizeof buf, p)) out += buf;
  pclose(p);
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}
bool nativeDialogsAvailable() { return std::system("command -v zenity > /dev/null 2>&1") == 0; }
std::string openFileDialog(const char* title) {
  if (!nativeDialogsAvailable()) return {};
  return run(std::string("zenity --file-selection --title='") + title + "' 2>/dev/null");
}
std::string saveFileDialog(const char* title, const std::string& defaultName) {
  if (!nativeDialogsAvailable()) return {};
  return run(std::string("zenity --file-selection --save --confirm-overwrite --title='") + title + "' --filename='" + defaultName + "' 2>/dev/null");
}
#endif

}  // namespace fd
