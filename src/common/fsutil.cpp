#include "fsutil.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "util.h"

namespace bc {
namespace fs {
namespace {

#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
  std::u16string u = Utf8ToUtf16(s);
  std::wstring w;
  w.reserve(u.size());
  for (char16_t c : u) w.push_back(static_cast<wchar_t>(c));
  for (auto& c : w)
    if (c == L'/') c = L'\\';
  return w;
}
#endif

FILE* Open(const std::string& path, const char* mode) {
#if defined(_WIN32)
  std::wstring m;
  for (const char* p = mode; *p; ++p) m.push_back(static_cast<wchar_t>(*p));
  return _wfopen(Wide(path).c_str(), m.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

std::string Join(const std::string& a, const std::string& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const char last = a.back();
  if (last == '/' || last == '\\') return a + b;
  return a + "/" + b;
}

bool Exists(const std::string& path) {
#if defined(_WIN32)
  return GetFileAttributesW(Wide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
#endif
}

bool IsDir(const std::string& path) {
#if defined(_WIN32)
  const DWORD a = GetFileAttributesW(Wide(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool MakeDirs(const std::string& path) {
  if (path.empty()) return false;
  if (IsDir(path)) return true;
  // create every missing component
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    const char c = path[i];
    cur.push_back(c);
    const bool end = (i + 1 == path.size());
    if (c == '/' || c == '\\' || end) {
      if (cur.size() == 1 && (c == '/' || c == '\\')) continue;
      if (cur.size() == 2 && cur[1] == ':') continue;  // "C:"
      if (IsDir(cur)) continue;
#if defined(_WIN32)
      if (!CreateDirectoryW(Wide(cur).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
#else
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
#endif
    }
  }
  return IsDir(path);
}

bool Remove(const std::string& path) {
#if defined(_WIN32)
  if (!DeleteFileW(Wide(path).c_str())) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
  return true;
#else
  if (::unlink(path.c_str()) != 0) return errno == ENOENT;
  return true;
#endif
}

bool Rename(const std::string& from, const std::string& to) {
#if defined(_WIN32)
  return MoveFileExW(Wide(from).c_str(), Wide(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

bool ReadAll(const std::string& path, std::string* out, size_t maxBytes) {
  FILE* f = Open(path, "rb");
  if (!f) return false;
  std::string s;
  char buf[8192];
  size_t n;
  bool ok = true;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    if (s.size() + n > maxBytes) {
      ok = false;
      break;
    }
    s.append(buf, n);
  }
  if (std::ferror(f)) ok = false;
  std::fclose(f);
  if (ok && out) *out = std::move(s);
  return ok;
}

bool WriteAtomic(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  FILE* f = Open(tmp, "wb");
  if (!f) return false;
  bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = (std::fflush(f) == 0) && ok;
#if defined(_WIN32)
  ok = (_commit(_fileno(f)) == 0) && ok;
#else
  ok = (::fsync(fileno(f)) == 0) && ok;
#endif
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) {
    Remove(tmp);
    return false;
  }
  if (!Rename(tmp, path)) {
    Remove(tmp);
    return false;
  }
  return true;
}

bool Append(const std::string& path, const std::string& data) {
  FILE* f = Open(path, "ab");
  if (!f) return false;
  const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  return ok;
}

uint64_t FileSize(const std::string& path) {
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA d;
  if (!GetFileAttributesExW(Wide(path).c_str(), GetFileExInfoStandard, &d)) return 0;
  return (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
#else
  struct stat st;
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_size);
#endif
}

}  // namespace fs
}  // namespace bc
