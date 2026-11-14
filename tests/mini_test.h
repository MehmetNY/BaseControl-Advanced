// Tiny dependency-free test harness.
#pragma once
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace mt {
struct Case {
  const char* name;
  void (*fn)();
};
inline std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}
struct Reg {
  Reg(const char* n, void (*f)()) { Registry().push_back({n, f}); }
};
inline int& Failures() { static int f = 0; return f; }
inline int& Checks() { static int c = 0; return c; }
inline const char*& Current() { static const char* c = ""; return c; }

template <class T>
std::string Show(const T& v) {
  std::ostringstream o;
  o << v;
  return o.str();
}
inline std::string Show(const bool& v) { return v ? "true" : "false"; }
inline std::string Show(const std::string& v) { return "\"" + v + "\""; }
inline std::string Show(const char* v) { return std::string("\"") + v + "\""; }
inline std::string Show(const unsigned char& v) { return Show(static_cast<int>(v)); }
}  // namespace mt

#define TEST(name)                                  \
  static void name();                               \
  static mt::Reg reg_##name(#name, name);           \
  static void name()

#define CHECK(cond)                                                                          \
  do {                                                                                       \
    mt::Checks()++;                                                                          \
    if (!(cond)) {                                                                           \
      mt::Failures()++;                                                                      \
      std::printf("  FAIL [%s] %s:%d: CHECK(%s)\n", mt::Current(), __FILE__, __LINE__, #cond); \
    }                                                                                        \
  } while (0)

#define CHECK_EQ(a, b)                                                                                                    \
  do {                                                                                                                    \
    mt::Checks()++;                                                                                                       \
    auto va_ = (a);                                                                                                       \
    auto vb_ = (b);                                                                                                       \
    if (!(va_ == vb_)) {                                                                                                  \
      mt::Failures()++;                                                                                                   \
      std::printf("  FAIL [%s] %s:%d: %s == %s  (%s vs %s)\n", mt::Current(), __FILE__, __LINE__, #a, #b, mt::Show(va_).c_str(), \
                  mt::Show(vb_).c_str());                                                                                 \
    }                                                                                                                     \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                       \
  do {                                                                                                              \
    mt::Checks()++;                                                                                                 \
    double va_ = (a), vb_ = (b);                                                                                    \
    if (!(std::fabs(va_ - vb_) <= (tol))) {                                                                         \
      mt::Failures()++;                                                                                             \
      std::printf("  FAIL [%s] %s:%d: |%s - %s| <= %s  (%g vs %g)\n", mt::Current(), __FILE__, __LINE__, #a, #b, #tol, va_, vb_); \
    }                                                                                                               \
  } while (0)
