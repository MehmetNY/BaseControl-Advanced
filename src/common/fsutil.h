// BaseControl Advanced - small file-system helpers that behave the same on Windows (UTF-8 path -> wide API, atomic
// replace) and on POSIX hosts (unit tests). Only whole-file operations: the app never streams big files.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bc {
namespace fs {

// Path arguments are always UTF-8. Directory separators may be '/' or '\\'.
bool Exists(const std::string& path);
bool IsDir(const std::string& path);
bool MakeDirs(const std::string& path);                       // mkdir -p
bool Remove(const std::string& path);                         // file only; true when it is gone afterwards
bool Rename(const std::string& from, const std::string& to);  // replaces `to` atomically when it exists
// Reads a whole file (max `maxBytes`, default 8 MiB). Returns false when missing/unreadable/too large.
bool ReadAll(const std::string& path, std::string* out, size_t maxBytes = (8u << 20));
// Writes `data` to "<path>.tmp", flushes it to disk and renames over `path`. A crash leaves either the old or the new file.
bool WriteAtomic(const std::string& path, const std::string& data);
// Appends one line (used by the rolling log); creates the file when needed.
bool Append(const std::string& path, const std::string& data);
uint64_t FileSize(const std::string& path);                  // 0 when missing
std::string Join(const std::string& a, const std::string& b); // a + "/" + b without doubling separators

}  // namespace fs
}  // namespace bc
