// Fan curve model + EC table packing (CPU 0xF00.., GPU 0xF30..). 10 active rows, 16 slots per array.
// Row semantics (documented for the Uniwill RamFan1 format): row i applies when up[i-1] < T <= up[i];
// row 0 is forced to duty 0 ("below the first threshold the EC decides"); padding rows carry up=0xFF.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace bc {
namespace fan {

inline constexpr int kActiveRows = 10;
inline constexpr int kSlots = 16;
inline constexpr int kMinSpacing = 4;   // deg C between adjacent thresholds
inline constexpr int kMaxTemp = 105;

struct Row {
  int temp = 0;  // up threshold, deg C
  int pct = 0;   // duty percent (row 0 is always 0)
};
using Curve = std::vector<Row>;

struct TableImage {
  uint8_t up[kSlots];
  uint8_t down[kSlots];
  uint8_t duty[kSlots];  // raw = percent*2
};

bool Validate(const Curve& c, std::string* why);            // structural rules (not "good taste")
// Minimum duty (percent) that a curve must reach at a given temperature. Applies to every custom curve we write:
// the EC's own protection is bypassed in custom mode, so the floor is the safety net.
int SafetyFloor(int tempC);
bool Monotonic(const Curve& c);                             // duty never decreases with temperature
Curve Sanitize(Curve c);                                    // enforce spacing / ranges / row 0 / monotonic (UI drag helper)
TableImage Pack(const Curve& c);                            // c must pass Validate()
bool Unpack(const TableImage& img, Curve* out, std::string* why);  // parses what the EC currently holds
int DutyAt(const Curve& c, int tempC);                      // evaluation rule, for the preview chart
std::string ToText(const Curve& c);                         // "30:0,45:20,..."
bool FromText(const std::string& s, Curve* out);

// ---- editing helpers for the curve editor (every result is Sanitize()d, so it passes Validate())
// Moves row `index` to (temp, pct): row 0 only changes its temperature (its duty is always 0), the last row keeps 100 %.
// The temperature stays at least kMinSpacing from both neighbours; the duty never drops below the previous row's and rises as
// far as the safety floor and monotonic order require (later rows are pushed up, never down).
Curve MovePoint(Curve c, int index, int temp, int pct);
// Splits the step that covers `temp` in two (same duty on both sides) so it can be dragged. No-op when the table is full or
// `temp` is closer than kMinSpacing to an existing row.
Curve AddPoint(Curve c, int temp);
// Removes row `index` (never row 0, never below two rows). The next row's higher duty then covers the removed step.
Curve RemovePoint(Curve c, int index);

Curve DefaultCpu();
Curve DefaultGpu();
Curve QuietCpu();
Curve TurboCpu();
Curve QuietGpu();
Curve TurboGpu();

}  // namespace fan
}  // namespace bc
