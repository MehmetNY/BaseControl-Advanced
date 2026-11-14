#include <cstring>

#include "mini_test.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (auto& c : mt::Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    mt::Current() = c.name;
    int before = mt::Failures();
    c.fn();
    ran++;
    if (mt::Failures() == before) std::printf("ok   %s\n", c.name);
    else std::printf("FAIL %s\n", c.name);
  }
  std::printf("\n%d tests, %d checks, %d failures\n", ran, mt::Checks(), mt::Failures());
  return mt::Failures() ? 1 : 0;
}
