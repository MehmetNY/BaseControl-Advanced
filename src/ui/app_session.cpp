// The part of the App that belongs to the signed-in user's desktop: what the keyboard hook and the touchpad corner trigger, the
// overlay window, the automatic refresh rate, the delayed write of the preferences. Everything here runs on the UI thread.
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../common/fsutil.h"
#include "../common/keymap.h"
#include "../common/osdcfg.h"
#include "../common/util.h"
#include "app_internal.h"
#include "page.h"

namespace bc {
namespace ui {

using hw::LocalDataDir;

namespace {

// ------------------------------------------------------------------------------------------------ Windows helpers
// Published COM identifiers of the Core Audio API (written out so the build does not depend on which import library has them).
const GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIidEndpointVolume = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

bool ComReady() {
  static const bool ok = [] {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  }();
  return ok;
}

// Flips the mute state of the default microphone. Returns 1 = now muted, 0 = now live, -1 = no microphone / failure.
int ToggleMicrophone() {
  if (!ComReady()) return -1;
  int result = -1;
  IMMDeviceEnumerator* en = nullptr;
  if (SUCCEEDED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, reinterpret_cast<void**>(&en))) && en) {
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)) && dev) {
      IAudioEndpointVolume* vol = nullptr;
      if (SUCCEEDED(dev->Activate(kIidEndpointVolume, CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&vol))) && vol) {
        BOOL muted = FALSE;
        if (SUCCEEDED(vol->GetMute(&muted)) && SUCCEEDED(vol->SetMute(!muted, nullptr))) result = muted ? 0 : 1;
        vol->Release();
      }
      dev->Release();
    }
    en->Release();
  }
  return result;
}

void TapKey(WORD vk, bool extended = false) {
  INPUT in[2] = {};
  in[0].type = in[1].type = INPUT_KEYBOARD;
  in[0].ki.wVk = in[1].ki.wVk = vk;
  in[0].ki.wScan = in[1].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
  in[0].ki.dwFlags = extended ? KEYEVENTF_EXTENDEDKEY : 0;
  in[1].ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | KEYEVENTF_KEYUP;
  SendInput(2, in, sizeof(INPUT));   // injected, so the keyboard hook ignores it
}

// Image name (lower case) of the program that owns the foreground window; "" when it cannot be read.
std::string ForegroundExe() {
  HWND fg = GetForegroundWindow();
  if (!fg) return "";
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (!pid) return "";
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return "";
  wchar_t buf[2 * MAX_PATH];
  DWORD n = ARRAYSIZE(buf);
  std::string out;
  if (QueryFullProcessImageNameW(h, 0, buf, &n)) out = NormalizeExeName(Narrow(std::wstring(buf, n)));
  CloseHandle(h);
  return out;
}

std::string PrefsFile() { return fs::Join(LocalDataDir(), "ui.kv"); }

}  // namespace

// ------------------------------------------------------------------------------------------------ preferences
void App::SetPref(const std::string& k, const std::string& v) {
  if (prefs_.Has(k) && prefs_.Str(k) == v) return;
  prefs_.Set(k, v);
  prefsDirty_ = true;
  prefsT_ = NowSeconds();
  dirty_ = true;
}

void App::FlushPrefs() {
  if (!prefsDirty_) return;
  prefsDirty_ = false;
  if (shot_) return;
  fs::MakeDirs(LocalDataDir());
  fs::WriteAtomic(PrefsFile(), prefs_.Serialize());
}

// ------------------------------------------------------------------------------------------------ session
bool App::SessionLive() const {
  // An offscreen render of the simulator draws the controls as if the real window were running (README pictures, script tests).
  return shot_ ? opt_.mode == "sim" : d_->hwnd != nullptr;
}

bool App::TouchpadGesture() const {
  if (d_->session) return d_->session->TouchpadPresent();
  return shot_ && opt_.mode == "sim";
}

void App::UpdateInterest() {
  if (backend_) backend_->SetInterest(d_->windowVisible || d_->overlayWanted);
}

void App::ApplySession() {
  Impl& d = *d_;
  dirty_ = true;
  if (shot_ || !d.hwnd) return;

  d.hideOnClose = PrefB("tray", true);
  if (PrefB("tray.icon", true) || opt_.tray) d.AddTray();
  else d.RemoveTray();

  if (!d.session) d.session.reset(new Session(d.hwnd, kMsgAction));
  SessionConfig sc;
  sc.winLock = PrefB("keys.winlock", false);
  sc.numLock = PrefB("keys.numlock", false);
  sc.remap = PrefB("keys.remap", false);
  sc.binds = ParseKeyMap(Pref("keys.map"));
  sc.touchpadCorner = PrefB("keys.tpcorner", false);
  const OsdConfig oc = OsdFromKv(prefs_);
  unsigned mods = 0, vk = 0;
  // The shortcut is only taken from other programs while the overlay is enabled.
  if (oc.enabled && ParseHotkey(oc.hotkey, &mods, &vk)) {
    sc.hotMods = mods;
    sc.hotVk = vk;
  }
  d.session->Configure(sc);
  d.lastOverlayT = 0;   // redraw the overlay at the next tick
}

// ------------------------------------------------------------------------------------------------ key capture (Keys page)
void App::BeginKeyCapture() {
  Impl& d = *d_;
  d.capturing = true;
  d.injectedKey = -1;
  d.captureT = NowSeconds();
  if (d.session) d.session->BeginCapture();
  dirty_ = true;
}

void App::CancelKeyCapture() {
  Impl& d = *d_;
  if (!d.capturing) return;
  d.capturing = false;
  if (d.session) d.session->EndCapture();
  dirty_ = true;
}

bool App::KeyCapturing() const { return d_->capturing; }

bool App::TakeCapturedKey(int* id) {
  Impl& d = *d_;
  if (d.injectedKey >= 0) {
    if (id) *id = d.injectedKey;
    d.injectedKey = -1;
    d.capturing = false;
    return true;
  }
  if (d.session && d.session->TakeCaptured(id)) {
    d.capturing = false;
    return true;
  }
  return false;
}

void App::FeedCapturedKey(int id) {
  if (d_->capturing) d_->injectedKey = id;
}

// ------------------------------------------------------------------------------------------------ actions
void App::Announce(const std::string& title, const std::string& text) {
  if (!PrefB("notify", true)) return;
  Impl& d = *d_;
  if (d.hwnd && GetForegroundWindow() == d.hwnd) ui_.Toast(title, ToastKind::Info, text);
  else d.Balloon(title, text);
}

void App::Impl::Balloon(const std::string& title, const std::string& text) {
  if (!trayAdded) return;
  NOTIFYICONDATAW n = nid;
  n.uFlags = NIF_INFO;
  n.dwInfoFlags = NIIF_NOSOUND;
  std::wstring t = Wide(title), b = Wide(text);
  if (t.size() >= ARRAYSIZE(n.szInfoTitle)) t.resize(ARRAYSIZE(n.szInfoTitle) - 1);
  if (b.size() >= ARRAYSIZE(n.szInfo)) b.resize(ARRAYSIZE(n.szInfo) - 1);
  wcsncpy(n.szInfoTitle, t.c_str(), ARRAYSIZE(n.szInfoTitle) - 1);
  wcsncpy(n.szInfo, b.c_str(), ARRAYSIZE(n.szInfo) - 1);
  Shell_NotifyIconW(NIM_MODIFY, &n);
}

bool App::OverlayShown() const { return d_->osd && d_->osd->Shown(); }

void App::RunAction(const std::string& id) {
  Impl& d = *d_;
  auto toggleDevice = [&](const char* key, const std::string& name) {
    const bool on = B(key, true);
    const std::string label = on ? Tf("%s switched off", name.c_str()) : Tf("%s switched on", name.c_str());
    Set(key, on ? "0" : "1", "", KV(), [this, label](const Result& r) {
      if (r.ok) Announce(label, "");
    });
  };

  if (id == "mic_mute") {
    const int r = ToggleMicrophone();
    if (r < 0) Announce(T("Microphone"), T("No microphone found."));
    else Announce(T("Microphone"), r == 1 ? T("Muted") : T("On"));
  } else if (id == "vol_mute") {
    TapKey(VK_VOLUME_MUTE);
  } else if (id == "vol_up") {
    TapKey(VK_VOLUME_UP);
  } else if (id == "vol_down") {
    TapKey(VK_VOLUME_DOWN);
  } else if (id == "media_play") {
    TapKey(VK_MEDIA_PLAY_PAUSE);
  } else if (id == "media_next") {
    TapKey(VK_MEDIA_NEXT_TRACK);
  } else if (id == "media_prev") {
    TapKey(VK_MEDIA_PREV_TRACK);
  } else if (id == "osd_toggle") {
    if (!PrefB("osd.on", false)) {
      d.osdVisible = true;
      SetPref("osd.on", "1");
      ApplySession();
    } else {
      d.osdVisible = !d.osdVisible;
      d.lastOverlayT = 0;
    }
    dirty_ = true;
  } else if (id == "profile_next") {
    const std::vector<std::string> names = Split(S("profile.list"), ',');
    if (names.empty() || !connected()) {
      Announce(T("Performance profile"), T("Not connected to the BaseControl service."));
      return;
    }
    const std::string cur = S("profile.active");
    const auto it = std::find(names.begin(), names.end(), cur);
    const size_t next = it == names.end() ? 0 : (static_cast<size_t>(it - names.begin()) + 1) % names.size();
    KV a;
    a.Set("name", names[next]);
    const std::string name = names[next];
    Cmd("profile.apply", a, "", [this, name](const Result& r) {
      if (r.ok) Announce(T("Performance profile"), name);
      else ui_.Toast(Tf("Could not apply: %s", name.c_str()), ToastKind::Error, CodeText(r.code));
    });
  } else if (id == "fan_boost") {
    const std::string cur = S("fan.mode", "auto");
    if (cur == "boost") {
      const std::string back = Str("fan.prev").empty() ? std::string("auto") : Str("fan.prev");
      Set("fan.mode", back, "", KV(), [this](const Result& r) {
        if (r.ok) Announce(T("Fans"), T("Back to normal"));
      });
    } else {
      Str("fan.prev") = cur;
      Set("fan.mode", "boost", "", KV(), [this](const Result& r) {
        if (r.ok) Announce(T("Fans"), T("Maximum speed"));
      });
    }
  } else if (id == "kbd_light") {
    // off -> 1 -> 2 -> 3 -> 4 -> off
    auto mk = [](const char* key, const char* v) {
      Request r;
      r.op = "set";
      r.key = key;
      r.value = v;
      return r;
    };
    if (!B("light.kbd.power", true)) {
      Chain({mk("light.kbd.bright", "1"), mk("light.kbd.power", "1")}, {"", ""});
    } else {
      const int lv = static_cast<int>(I("light.kbd.bright", 4));
      if (lv >= 4) Set("light.kbd.power", "0");
      else Set("light.kbd.bright", std::to_string(lv + 1));
    }
  } else if (id == "touchpad_toggle") {
    toggleDevice("dev.touchpad", T("Touchpad"));
  } else if (id == "wifi_toggle") {
    toggleDevice("dev.wifi", T("Wi-Fi"));
  } else if (id == "cam_toggle") {
    toggleDevice("dev.cam", T("Camera"));
  } else if (id == "screenshot") {
    ShellExecuteW(nullptr, L"open", L"ms-screenclip:", nullptr, nullptr, SW_SHOWNORMAL);
  } else if (id == "calculator") {
    ShellExecuteW(nullptr, L"open", L"calc.exe", nullptr, nullptr, SW_SHOWNORMAL);
  } else if (id == "show_window") {
    if (d.hwnd) {
      d.SetVisible(true);
      ShowWindow(d.hwnd, IsIconic(d.hwnd) ? SW_RESTORE : SW_SHOW);
      SetForegroundWindow(d.hwnd);
    }
  } else if (id == "lock") {
    LockWorkStation();
  }
  // "disable" and unknown ids do nothing on purpose
}

// ------------------------------------------------------------------------------------------------ overlay and housekeeping
void App::SessionTick() {
  Impl& d = *d_;
  const double now = NowSeconds();

  if (d.capturing && now - d.captureT > 20) {   // nobody pressed anything: do not keep swallowing keys
    CancelKeyCapture();
    ui_.Toast(T("No key was pressed."), ToastKind::Info);
  }

  const OsdConfig cfg = OsdFromKv(prefs_);
  bool want = cfg.enabled && d.osdVisible;
  if (want && cfg.gamesOnly) {
    const std::string fg = ForegroundExe();
    want = !fg.empty() && std::find(cfg.games.begin(), cfg.games.end(), fg) != cfg.games.end();
  }
  const bool wasWanted = d.overlayWanted;
  d.overlayWanted = want;
  if (want != wasWanted) UpdateInterest();
  if (!want && !(d.osd && d.osd->Shown())) return;
  if (want && now - d.lastOverlayT < 0.5) return;
  d.lastOverlayT = now;
  if (!d.osd) d.osd.reset(new OsdWindow());
  d.osd->Update(cfg, OsdDataFrom(snap_), want, ui_.gfx.Family());
}

// "Lower the rate on battery" and "Follow the performance profile". It reacts when one of its inputs changes (power source,
// profile, the two switches) and then leaves the rate alone, so a rate you pick by hand stays until the next change.
void App::AutoRefresh() {
  if (!backend_ || !backend_->Connected()) return;
  const double now = NowSeconds();
  if (now - autoT_ < 1.0) return;
  autoT_ = now;
  std::vector<int> rates;
  for (const std::string& p : Split(S("disp.refresh.list"), ',')) {
    long long n = 0;
    if (ParseInt(Trim(p), n) && n >= 30 && n <= 480) rates.push_back(static_cast<int>(n));
  }
  std::sort(rates.begin(), rates.end());
  rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
  if (rates.size() < 2 || !Has("disp.refresh") || !Has("power.ac")) return;

  const bool batt = PrefB("disp.auto.batt", false), mode = PrefB("disp.auto.mode", false);
  const bool ac = B("power.ac", true);
  const std::string prof = S("profile.active");
  const std::string key = Fmt("%d|%d|%d|%s", batt ? 1 : 0, mode ? 1 : 0, ac ? 1 : 0, prof.c_str());
  if (key == autoKey_) return;
  const bool first = autoKey_.empty();
  autoKey_ = key;
  const int cur = static_cast<int>(I("disp.refresh"));

  int want = 0;
  if (batt && !ac) want = rates.front();
  else if (mode) want = (prof == "Quiet") ? rates.front() : rates.back();
  if (!want) {
    // nothing asks for a particular rate any more: give back the one the user had
    if (autoUserHz_ > 0 && std::find(rates.begin(), rates.end(), autoUserHz_) != rates.end() && autoUserHz_ != cur)
      Set("disp.refresh", std::to_string(autoUserHz_));
    autoUserHz_ = -1;
    return;
  }
  if (first && cur == want) return;
  if (cur != want) {
    if (autoUserHz_ < 0) autoUserHz_ = cur;
    Set("disp.refresh", std::to_string(want), Tf("Refresh rate set to %d Hz", want));
  }
}

}  // namespace ui
}  // namespace bc
