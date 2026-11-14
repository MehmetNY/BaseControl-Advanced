// Control Center Advanced - small portable helpers (no windows.h; unit-testable on any host).
#pragma once
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

namespace bc {

// On mingw the project is always built with -D__USE_MINGW_ANSI_STDIO=1 so that %zu / %lld behave like C99.
#if defined(__MINGW32__)
#define BC_PRINTF(a, b) __attribute__((format(gnu_printf, a, b)))
#elif defined(__GNUC__)
#define BC_PRINTF(a, b) __attribute__((format(printf, a, b)))
#else
#define BC_PRINTF(a, b)
#endif
std::string Fmt(const char* fmt, ...) BC_PRINTF(1, 2);

std::vector<std::string> Split(const std::string& s, char sep, bool keepEmpty = false);
std::string Join(const std::vector<std::string>& v, const std::string& sep);
std::string Trim(const std::string& s);
bool StartsWith(const std::string& s, const std::string& p);
bool EndsWith(const std::string& s, const std::string& p);
std::string Lower(std::string s);
bool EqNoCase(const std::string& a, const std::string& b);

bool ParseInt(const std::string& s, long long& out);      // decimal or 0x-hex, whole string
bool ParseDouble(const std::string& s, double& out);
bool ParseBool(const std::string& s, bool& out);          // 1/0/true/false/on/off/yes/no

// Percent-style escaping so arbitrary text can travel in "key=value" lines and pipe messages.
std::string Escape(const std::string& s);                 // escapes \\, \n, \r, \t, '=' and '|' as %XX-free backslash forms
std::string Unescape(const std::string& s);

// UTF-8 <-> UTF-16 (portable; wchar_t is 16-bit on Windows, 32-bit elsewhere, so we use char16_t).
std::u16string Utf8ToUtf16(const std::string& s);
std::string Utf16ToUtf8(const std::u16string& s);

template <class T>
inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline int RoundToStep(int v, int step, int base = 0) {
  if (step <= 1) return v;
  int d = v - base;
  int r = (d >= 0) ? (d + step / 2) / step : -((-d + step / 2) / step);
  return base + r * step;
}

uint32_t Crc32(const void* data, size_t n);
std::string Hex(uint64_t v, int width = 0);

}  // namespace bc
