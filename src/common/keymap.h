// Key remapping model shared by the UI (Keys page, keyboard hook) and the tests: the physical keyboard (scan codes, so a
// binding survives a change of input language), the actions a key can be given and the text format the bindings are stored in.
// Pure C++, no Windows types.
#pragma once
#include <string>
#include <vector>

namespace bc {

constexpr int kKeyExt = 0x100;   // key id = scan code | (extended key ? kKeyExt : 0)

struct KeyAction {
  const char* id;      // stored in ui.kv
  const char* label;   // English text; the UI translates it
  bool repeat;         // may fire again while the key is held (volume)
};
const std::vector<KeyAction>& KeyActions();
const KeyAction* FindKeyAction(const std::string& id);

struct KeyBind {
  int key = 0;
  std::string action;
};
// "66=mic_mute,67=osd_toggle". Unknown actions and malformed pieces are dropped; a key is bound at most once.
std::vector<KeyBind> ParseKeyMap(const std::string& text);
std::string FormatKeyMap(const std::vector<KeyBind>& v);
const KeyBind* FindBind(const std::vector<KeyBind>& v, int key);
void SetBind(std::vector<KeyBind>* v, int key, const std::string& action);
bool RemoveBind(std::vector<KeyBind>* v, int key);

struct PhysKey {
  int id;            // scan code | kKeyExt
  float w;           // width in key units
  const char* tr;    // legend on a Turkish Q keyboard
  const char* us;    // legend on a US keyboard
  bool mappable;     // false: never reaches Windows (the Fn key is handled by the firmware)
};
// Rows of an ISO laptop keyboard, top (function row) first. Every row is 15 units wide; the ISO Enter key is listed in two rows
// with the same id.
const std::vector<std::vector<PhysKey>>& KeyboardRows();
// Legend of a key id on the given layout; "" when the id is not on the drawn keyboard.
std::string KeyLegend(int id, bool turkish);

}  // namespace bc
