#include "keymap.h"

#include <algorithm>
#include <cstdlib>

#include "i18n_mark.h"
#include "util.h"

namespace bc {

const std::vector<KeyAction>& KeyActions() {
  static const std::vector<KeyAction> k = {
      {"mic_mute", N_("Mute / unmute the microphone"), false},
      {"vol_mute", N_("Mute / unmute the sound"), false},
      {"vol_up", N_("Volume up"), true},
      {"vol_down", N_("Volume down"), true},
      {"media_play", N_("Play / pause media"), false},
      {"media_next", N_("Next track"), false},
      {"media_prev", N_("Previous track"), false},
      {"osd_toggle", N_("Show / hide the in-game overlay"), false},
      {"profile_next", N_("Next performance profile"), false},
      {"fan_boost", N_("Maximum fans on / off"), false},
      {"kbd_light", N_("Cycle the keyboard backlight"), false},
      {"touchpad_toggle", N_("Touchpad on / off"), false},
      {"wifi_toggle", N_("Wi-Fi on / off"), false},
      {"cam_toggle", N_("Camera on / off"), false},
      {"screenshot", N_("Take a screenshot (Snipping Tool)"), false},
      {"calculator", N_("Open Calculator"), false},
      {"show_window", N_("Open Control Center Advanced"), false},
      {"lock", N_("Lock the computer"), false},
      {"disable", N_("Disable this key"), false},
  };
  return k;
}

const KeyAction* FindKeyAction(const std::string& id) {
  for (const KeyAction& a : KeyActions())
    if (id == a.id) return &a;
  return nullptr;
}

std::vector<KeyBind> ParseKeyMap(const std::string& text) {
  std::vector<KeyBind> out;
  for (const std::string& piece : Split(text, ',')) {
    const size_t eq = piece.find('=');
    if (eq == std::string::npos) continue;
    long long key = 0;
    if (!ParseInt(Trim(piece.substr(0, eq)), key) || key <= 0 || key > 0x1FF) continue;
    const std::string action = Trim(piece.substr(eq + 1));
    if (!FindKeyAction(action)) continue;
    SetBind(&out, static_cast<int>(key), action);
  }
  return out;
}

std::string FormatKeyMap(const std::vector<KeyBind>& v) {
  std::string s;
  for (const KeyBind& b : v) {
    if (!s.empty()) s += ",";
    s += std::to_string(b.key) + "=" + b.action;
  }
  return s;
}

const KeyBind* FindBind(const std::vector<KeyBind>& v, int key) {
  for (const KeyBind& b : v)
    if (b.key == key) return &b;
  return nullptr;
}

void SetBind(std::vector<KeyBind>* v, int key, const std::string& action) {
  for (KeyBind& b : *v)
    if (b.key == key) {
      b.action = action;
      return;
    }
  KeyBind nb;
  nb.key = key;
  nb.action = action;
  v->push_back(nb);
}

bool RemoveBind(std::vector<KeyBind>* v, int key) {
  const size_t n = v->size();
  v->erase(std::remove_if(v->begin(), v->end(), [key](const KeyBind& b) { return b.key == key; }), v->end());
  return v->size() != n;
}

const std::vector<std::vector<PhysKey>>& KeyboardRows() {
  constexpr int E = kKeyExt;
  static const std::vector<std::vector<PhysKey>> rows = {
      // function row
      {{0x01, 1.5f, "Esc", "Esc", true}, {0x3B, 1, "F1", "F1", true}, {0x3C, 1, "F2", "F2", true}, {0x3D, 1, "F3", "F3", true},
       {0x3E, 1, "F4", "F4", true}, {0x3F, 1, "F5", "F5", true}, {0x40, 1, "F6", "F6", true}, {0x41, 1, "F7", "F7", true},
       {0x42, 1, "F8", "F8", true}, {0x43, 1, "F9", "F9", true}, {0x44, 1, "F10", "F10", true}, {0x57, 1, "F11", "F11", true},
       {0x58, 1, "F12", "F12", true}, {E | 0x53, 1.5f, "Del", "Del", true}},
      // numbers
      {{0x29, 1, "\xC3\xA9", "`", true}, {0x02, 1, "1", "1", true}, {0x03, 1, "2", "2", true}, {0x04, 1, "3", "3", true},
       {0x05, 1, "4", "4", true}, {0x06, 1, "5", "5", true}, {0x07, 1, "6", "6", true}, {0x08, 1, "7", "7", true},
       {0x09, 1, "8", "8", true}, {0x0A, 1, "9", "9", true}, {0x0B, 1, "0", "0", true}, {0x0C, 1, "*", "-", true},
       {0x0D, 1, "-", "=", true}, {0x0E, 2, "Sil", "Backspace", true}},
      // Q row (the ISO Enter key starts here)
      {{0x0F, 1.5f, "Tab", "Tab", true}, {0x10, 1, "Q", "Q", true}, {0x11, 1, "W", "W", true}, {0x12, 1, "E", "E", true},
       {0x13, 1, "R", "R", true}, {0x14, 1, "T", "T", true}, {0x15, 1, "Y", "Y", true}, {0x16, 1, "U", "U", true},
       {0x17, 1, "I", "I", true}, {0x18, 1, "O", "O", true}, {0x19, 1, "P", "P", true}, {0x1A, 1, "\xC4\x9E", "[", true},
       {0x1B, 1, "\xC3\x9C", "]", true}, {0x1C, 1.5f, "Enter", "Enter", true}},
      // A row
      {{0x3A, 1.75f, "Caps", "Caps", true}, {0x1E, 1, "A", "A", true}, {0x1F, 1, "S", "S", true}, {0x20, 1, "D", "D", true},
       {0x21, 1, "F", "F", true}, {0x22, 1, "G", "G", true}, {0x23, 1, "H", "H", true}, {0x24, 1, "J", "J", true},
       {0x25, 1, "K", "K", true}, {0x26, 1, "L", "L", true}, {0x27, 1, "\xC5\x9E", ";", true}, {0x28, 1, "\xC4\xB0", "'", true},
       {0x2B, 1, ",", "\\", true}, {0x1C, 1.25f, "Enter", "Enter", true}},
      // Z row
      {{0x2A, 1.25f, "Sol Shift", "L Shift", true}, {0x56, 1, "<", "<", true}, {0x2C, 1, "Z", "Z", true}, {0x2D, 1, "X", "X", true},
       {0x2E, 1, "C", "C", true}, {0x2F, 1, "V", "V", true}, {0x30, 1, "B", "B", true}, {0x31, 1, "N", "N", true},
       {0x32, 1, "M", "M", true}, {0x33, 1, "\xC3\x96", ",", true}, {0x34, 1, "\xC3\x87", ".", true}, {0x35, 1, ".", "/", true},
       {E | 0x48, 1, "\xE2\x86\x91", "\xE2\x86\x91", true}, {0x36, 1.75f, "Sa\xC4\x9F Shift", "R Shift", true}},
      // bottom row
      {{0x1D, 1.25f, "Sol Ctrl", "L Ctrl", true}, {0, 1, "Fn", "Fn", false}, {E | 0x5B, 1.25f, "Win", "Win", true},
       {0x38, 1.25f, "Alt", "Alt", true}, {0x39, 4.75f, "Bo\xC5\x9Fluk", "Space", true}, {E | 0x38, 1.25f, "Alt Gr", "Alt Gr", true},
       {E | 0x1D, 1.25f, "Sa\xC4\x9F Ctrl", "R Ctrl", true}, {E | 0x4B, 1, "\xE2\x86\x90", "\xE2\x86\x90", true},
       {E | 0x50, 1, "\xE2\x86\x93", "\xE2\x86\x93", true}, {E | 0x4D, 1, "\xE2\x86\x92", "\xE2\x86\x92", true}},
  };
  return rows;
}

std::string KeyLegend(int id, bool turkish) {
  if (id <= 0) return "";
  for (const auto& row : KeyboardRows())
    for (const PhysKey& k : row)
      if (k.id == id) return turkish ? k.tr : k.us;
  return "";
}

}  // namespace bc
