#include "session.h"

#include <hidsdi.h>
#include <hidpi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bc {
namespace ui {
namespace {

Session* g_active = nullptr;   // the hook procedure has no user pointer; one session per process

// The HidP_* parsing functions live in hid.dll but the MinGW import library does not list them: bind them when first needed.
struct HidApi {
  decltype(&HidP_GetCaps) getCaps = nullptr;
  decltype(&HidP_GetValueCaps) getValueCaps = nullptr;
  decltype(&HidP_GetButtonCaps) getButtonCaps = nullptr;
  decltype(&HidP_GetUsageValue) getUsageValue = nullptr;
  decltype(&HidP_GetUsages) getUsages = nullptr;
  bool ok() const { return getCaps && getValueCaps && getButtonCaps && getUsageValue && getUsages; }
};

template <class Fn>
Fn Bind(HMODULE m, const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
}

const HidApi& Hid() {
  static const HidApi api = [] {
    HidApi a;
    if (HMODULE m = LoadLibraryW(L"hid.dll")) {
      a.getCaps = Bind<decltype(a.getCaps)>(m, "HidP_GetCaps");
      a.getValueCaps = Bind<decltype(a.getValueCaps)>(m, "HidP_GetValueCaps");
      a.getButtonCaps = Bind<decltype(a.getButtonCaps)>(m, "HidP_GetButtonCaps");
      a.getUsageValue = Bind<decltype(a.getUsageValue)>(m, "HidP_GetUsageValue");
      a.getUsages = Bind<decltype(a.getUsages)>(m, "HidP_GetUsages");
    }
    return a;
  }();
  return api;
}

int ActionIndex(const char* id) {
  const auto& a = KeyActions();
  for (size_t i = 0; i < a.size(); i++)
    if (std::strcmp(a[i].id, id) == 0) return static_cast<int>(i);
  return -1;
}

}  // namespace

// One precision touchpad: the report layout (which link collection carries which finger) read once from the preparsed data.
struct Session::Pad {
  std::vector<BYTE> storage;
  PHIDP_PREPARSED_DATA ppd = nullptr;
  struct Slot {
    USHORT link = 0;
    bool x = false, y = false, tip = false;
  };
  std::map<USHORT, Slot> slots;
  LONG xmin = 0, xmax = 1, ymin = 0, ymax = 1;
  bool ok = false;
};

Session::Session(HWND owner, UINT msgAction) : owner_(owner), msg_(msgAction) { g_active = this; }

Session::~Session() {
  SessionConfig off;
  Configure(off);
  EndCapture();
  UpdateHook();
  if (g_active == this) g_active = nullptr;
}

void Session::Post(int actionIndex, LPARAM kind) {
  if (actionIndex < 0 || !owner_) return;
  PostMessageW(owner_, msg_, static_cast<WPARAM>(actionIndex), kind);
}

// ------------------------------------------------------------------------------------------------ configuration
void Session::Configure(const SessionConfig& c) {
  const bool hotChanged = c.hotMods != cfg_.hotMods || c.hotVk != cfg_.hotVk;
  const bool numOn = c.numLock && !cfg_.numLock;
  cfg_ = c;
  UpdateHook();
  UpdateRawInput();
  if (hotChanged || (!hotkeyOn_ && cfg_.hotVk)) {
    if (hotkeyOn_) {
      UnregisterHotKey(owner_, kHotkeyId);
      hotkeyOn_ = false;
    }
    if (cfg_.hotVk && owner_) hotkeyOn_ = RegisterHotKey(owner_, kHotkeyId, cfg_.hotMods | MOD_NOREPEAT, cfg_.hotVk) != FALSE;
  }
  if (!cfg_.hotVk && hotkeyOn_) {
    UnregisterHotKey(owner_, kHotkeyId);
    hotkeyOn_ = false;
  }
  if (numOn && !(GetKeyState(VK_NUMLOCK) & 1)) {   // "keep Num Lock on" starts by switching it on
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_NUMLOCK;
    in[0].ki.wScan = in[1].ki.wScan = 0x45;
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
  }
}

void Session::UpdateHook() {
  const bool need = cfg_.winLock || cfg_.numLock || (cfg_.remap && !cfg_.binds.empty()) || capture_;
  if (need && !hook_) {
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &Session::HookProc, GetModuleHandleW(nullptr), 0);
  } else if (!need && hook_) {
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    held_.clear();
  }
}

LRESULT CALLBACK Session::HookProc(int code, WPARAM wp, LPARAM lp) {
  if (code == HC_ACTION && g_active) {
    if (g_active->OnKey(*reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp), wp)) return 1;
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

// Runs inside the hook: keep it short. Returns true to swallow the key.
bool Session::OnKey(const KBDLLHOOKSTRUCT& k, WPARAM wp) {
  if (k.flags & LLKHF_INJECTED) return false;   // never react to synthetic keys (ours or another program's)
  const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
  const int id = static_cast<int>(k.scanCode & 0xFF) | ((k.flags & LLKHF_EXTENDED) ? kKeyExt : 0);
  const bool win = k.vkCode == VK_LWIN || k.vkCode == VK_RWIN;
  if (!down) {
    if (held_.erase(id)) return true;   // the matching key-down was swallowed
    if ((cfg_.winLock && win) || (cfg_.numLock && k.vkCode == VK_NUMLOCK)) return true;
    return false;
  }
  if (capture_) {
    captured_ = id;
    haveCaptured_ = true;
    capture_ = false;   // the hook itself is removed later, from the UI thread
    held_.insert(id);
    Post(0, 1);
    return true;
  }
  if (cfg_.winLock && win) return true;
  if (cfg_.numLock && k.vkCode == VK_NUMLOCK) return true;
  if (cfg_.remap) {
    if (const KeyBind* b = FindBind(cfg_.binds, id)) {
      const bool first = held_.insert(id).second;
      const KeyAction* a = FindKeyAction(b->action);
      if (a && b->action != "disable" && (first || a->repeat)) Post(ActionIndex(a->id), 0);
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------------------------------------ key capture
void Session::BeginCapture() {
  capture_ = true;
  haveCaptured_ = false;
  UpdateHook();
}

void Session::EndCapture() {
  capture_ = false;
  UpdateHook();
}

bool Session::TakeCaptured(int* id) {
  if (!haveCaptured_) return false;
  haveCaptured_ = false;
  if (id) *id = captured_;
  UpdateHook();
  return true;
}

// ------------------------------------------------------------------------------------------------ precision touchpad
bool Session::TouchpadPresent() {
  if (touchpadCache_ >= 0) return touchpadCache_ == 1;
  touchpadCache_ = 0;
  UINT n = 0;
  if (GetRawInputDeviceList(nullptr, &n, sizeof(RAWINPUTDEVICELIST)) != 0 || n == 0) return false;
  std::vector<RAWINPUTDEVICELIST> list(n);
  n = GetRawInputDeviceList(list.data(), &n, sizeof(RAWINPUTDEVICELIST));
  if (n == static_cast<UINT>(-1)) return false;
  for (UINT i = 0; i < n; i++) {
    if (list[i].dwType != RIM_TYPEHID) continue;
    RID_DEVICE_INFO info = {};
    info.cbSize = sizeof info;
    UINT sz = sizeof info;
    if (GetRawInputDeviceInfoW(list[i].hDevice, RIDI_DEVICEINFO, &info, &sz) != static_cast<UINT>(-1) && info.dwType == RIM_TYPEHID &&
        info.hid.usUsagePage == 0x0D && info.hid.usUsage == 0x05) {
      touchpadCache_ = 1;
      break;
    }
  }
  return touchpadCache_ == 1;
}

void Session::UpdateRawInput() {
  const bool want = cfg_.touchpadCorner && owner_ && TouchpadPresent();
  if (want == rawOn_) return;
  RAWINPUTDEVICE rid = {};
  rid.usUsagePage = 0x0D;
  rid.usUsage = 0x05;
  if (want) {
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = owner_;
  } else {
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    tracks_.clear();
    lastTap_ = 0;
  }
  if (RegisterRawInputDevices(&rid, 1, sizeof rid)) rawOn_ = want;
}

Session::Pad* Session::PadFor(HANDLE dev) {
  auto it = pads_.find(dev);
  if (it != pads_.end()) return it->second.get();
  std::unique_ptr<Pad> p(new Pad());
  UINT sz = 0;
  if (Hid().ok() && GetRawInputDeviceInfoW(dev, RIDI_PREPARSEDDATA, nullptr, &sz) == 0 && sz) {
    p->storage.resize(sz);
    if (GetRawInputDeviceInfoW(dev, RIDI_PREPARSEDDATA, p->storage.data(), &sz) == sz) {
      p->ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(p->storage.data());
      HIDP_CAPS caps = {};
      if (Hid().getCaps(p->ppd, &caps) == HIDP_STATUS_SUCCESS) {
        USHORT nv = caps.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> vc(nv ? nv : 1);
        if (nv && Hid().getValueCaps(HidP_Input, vc.data(), &nv, p->ppd) == HIDP_STATUS_SUCCESS) {
          bool first = true;
          for (USHORT i = 0; i < nv; i++) {
            if (vc[i].UsagePage != 0x01 || vc[i].IsRange) continue;
            Pad::Slot& s = p->slots[vc[i].LinkCollection];
            s.link = vc[i].LinkCollection;
            if (vc[i].NotRange.Usage == 0x30) {
              s.x = true;
              if (first || vc[i].LogicalMax > p->xmax) p->xmax = vc[i].LogicalMax;
              if (first || vc[i].LogicalMin < p->xmin) p->xmin = vc[i].LogicalMin;
              first = false;
            } else if (vc[i].NotRange.Usage == 0x31) {
              s.y = true;
              if (vc[i].LogicalMax > p->ymax) p->ymax = vc[i].LogicalMax;
              if (vc[i].LogicalMin < p->ymin) p->ymin = vc[i].LogicalMin;
            }
          }
        }
        USHORT nb = caps.NumberInputButtonCaps;
        std::vector<HIDP_BUTTON_CAPS> bc(nb ? nb : 1);
        if (nb && Hid().getButtonCaps(HidP_Input, bc.data(), &nb, p->ppd) == HIDP_STATUS_SUCCESS) {
          for (USHORT i = 0; i < nb; i++) {
            if (bc[i].UsagePage != 0x0D) continue;
            const bool has = bc[i].IsRange ? (bc[i].Range.UsageMin <= 0x42 && bc[i].Range.UsageMax >= 0x42) : bc[i].NotRange.Usage == 0x42;
            if (has) p->slots[bc[i].LinkCollection].tip = true;
          }
        }
        p->ok = !p->slots.empty() && p->xmax > p->xmin && p->ymax > p->ymin;
      }
    }
  }
  Pad* raw = p.get();
  pads_[dev] = std::move(p);
  return raw;
}

void Session::OnRawInput(HRAWINPUT h) {
  if (!rawOn_) return;
  UINT size = 0;
  if (GetRawInputData(h, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || !size) return;
  std::vector<BYTE> buf(size);
  if (GetRawInputData(h, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;
  const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf.data());
  if (ri->header.dwType != RIM_TYPEHID) return;
  Pad* pad = PadFor(ri->header.hDevice);
  if (!pad || !pad->ok) return;
  const RAWHID& hid = ri->data.hid;
  for (DWORD i = 0; i < hid.dwCount; i++) {
    PCHAR rep = reinterpret_cast<PCHAR>(const_cast<BYTE*>(hid.bRawData) + static_cast<size_t>(i) * hid.dwSizeHid);
    for (const auto& kv : pad->slots) {
      const Pad::Slot& s = kv.second;
      if (!s.x || !s.y) continue;
      ULONG x = 0, y = 0;
      if (Hid().getUsageValue(HidP_Input, 0x01, s.link, 0x30, &x, pad->ppd, rep, hid.dwSizeHid) != HIDP_STATUS_SUCCESS) continue;
      if (Hid().getUsageValue(HidP_Input, 0x01, s.link, 0x31, &y, pad->ppd, rep, hid.dwSizeHid) != HIDP_STATUS_SUCCESS) continue;
      bool down = true;
      if (s.tip) {
        USAGE u[16];
        ULONG cnt = 16;
        down = false;
        if (Hid().getUsages(HidP_Input, 0x0D, s.link, u, &cnt, pad->ppd, rep, hid.dwSizeHid) == HIDP_STATUS_SUCCESS)
          for (ULONG j = 0; j < cnt; j++)
            if (u[j] == 0x42) down = true;
      }
      Finger(s.link, down, static_cast<long>(x), static_cast<long>(y), *pad);
    }
  }
}

// A tap is a short touch that stays put; two taps in the top-left corner within 0.7 s toggle the touchpad.
void Session::Finger(int link, bool down, long x, long y, const Pad& pad) {
  Track& t = tracks_[link];
  const DWORD now = GetTickCount();
  const double w = static_cast<double>(pad.xmax - pad.xmin), h = static_cast<double>(pad.ymax - pad.ymin);
  const double fx = static_cast<double>(x - pad.xmin) / w, fy = static_cast<double>(y - pad.ymin) / h;
  const bool corner = fx < 0.16 && fy < 0.20;
  if (down && !t.down) {
    t.down = true;
    t.t = now;
    t.sx = x;
    t.sy = y;
    t.inCorner = corner;
    t.moved = false;
  } else if (down && t.down) {
    if (std::fabs(static_cast<double>(x - t.sx)) > w * 0.03 || std::fabs(static_cast<double>(y - t.sy)) > h * 0.03) t.moved = true;
  } else if (!down && t.down) {
    t.down = false;
    if (t.inCorner && !t.moved && now - t.t < 350) {
      if (lastTap_ && now - lastTap_ < 700) {
        lastTap_ = 0;
        if (now - lastFire_ > 900) {
          lastFire_ = now;
          Post(ActionIndex("touchpad_toggle"), 0);
        }
      } else {
        lastTap_ = now;
      }
    } else {
      lastTap_ = 0;
    }
  }
}

}  // namespace ui
}  // namespace bc
