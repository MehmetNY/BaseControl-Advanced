// The BaseControl window: owns the backend (service pipe / in-process controller / simulator), the Ui, the snapshot copy
// the pages read from, the request bookkeeping (drafts, pending operations, consent / experimental / activation dialogs)
// and the Win32 window, tray icon and offscreen renderer used for the README screenshots.
#pragma once
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../common/controller.h"
#include "../common/settings_reg.h"
#include "../sim/sim.h"
#include "../svc/backend.h"
#include "i18n.h"
#include "ui.h"

namespace bc {
namespace hw { class WinWorld; }
class PipeBackend;
namespace ui {

enum class Page {
  Overview, Prefs, Devices, GpuInfo, Osd, Keys,   // general settings
  Profiles, Fan, Custom, Apps,                     // performance
  Lighting, Battery, Display, DeviceInfo, Safety, About, Count
};

struct Options {
  std::string mode = "auto";   // auto | local | sim
  std::string lang;            // "" = stored preference / system language
  bool tray = false;           // start hidden in the tray
  // offscreen rendering for documentation (--shot)
  std::string shotPage, shotOut, shotFont;
  int shotW = 1280, shotH = 800;
  float shotScale = 1.0f;
  bool shotAdvanced = false;   // Custom profiles: advanced CPU panel open
  bool shotDrafts = false;     // Custom profiles: leave a few sliders in the "draft" state
  std::string script;          // --script file: automation for tests
};

struct Draft {
  bool active = false;
  int v = 0;
  int liveAtStart = 0;
};

struct History {
  std::deque<float> cpuW, gpuW, cpuT, gpuT, cpuRpm, gpuRpm, cpuLoad, gpuLoad;
};

class App {
 public:
  explicit App(const Options& o);
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  int RunWindow(HINSTANCE hi, int show);
  int RunShot();                         // renders opt.shotPage into opt.shotOut and returns 0 on success

  // ---------------------------------------------------------------- for pages
  Ui& ui() { return ui_; }
  const KV& snap() const { return snap_; }
  const History& hist() const { return hist_; }
  bool simulated() const { return mode_ == "sim"; }
  const std::string& mode() const { return mode_; }
  bool connected() const { return backend_ && backend_->Connected(); }
  std::string BackendStatus() const { return backend_ ? backend_->Status() : std::string(); }

  bool Has(const std::string& k) const { return snap_.Has(k); }
  std::string S(const std::string& k, const std::string& def = "") const { return snap_.Str(k, def); }
  long long I(const std::string& k, long long def = 0) const { return snap_.Int(k, def); }
  double N(const std::string& k, double def = 0) const { return snap_.Num(k, def); }
  bool B(const std::string& k, bool def = false) const { return snap_.Bool(k, def); }
  bool CapOk(const std::string& cap) const { return cap.empty() || snap_.Str("cap." + cap) == "ok"; }
  std::string CapState(const std::string& cap) const { return snap_.Str("cap." + cap, "unknown"); }
  std::string CapWhy(const std::string& cap) const { return snap_.Str("capwhy." + cap); }
  std::string Fmt1(const std::string& k, int dec = 1, const char* unit = "") const;  // "—" when absent

  Draft& GetDraft(const std::string& key) { return drafts_[key]; }
  void DiscardDraft(const std::string& key) { drafts_.erase(key); }
  bool Pending(const std::string& key) const;
  const std::map<std::string, Draft>& drafts() const { return drafts_; }
  void ClearDrafts() { drafts_.clear(); }
  double RecentOk(const std::string& key) const;   // seconds since the last successful apply of `key` (large = never)

  // Submit requests. `label` is what the toast says ("PL1 set to 95 W"). Returns the operation id.
  uint64_t Set(const std::string& key, const std::string& value, const std::string& label = "", const KV& args = KV(), std::function<void(const Result&)> done = nullptr);
  uint64_t Cmd(const std::string& name, const KV& args = KV(), const std::string& label = "", std::function<void(const Result&)> done = nullptr);
  uint64_t Cfg(const std::string& key, const std::string& value, std::function<void(const Result&)> done = nullptr);
  // Several settings in order; stops at the first failure.
  void Chain(std::vector<Request> reqs, std::vector<std::string> labels);

  void Go(Page p);
  Page page() const { return page_; }

  // free-form UI state (text fields, tabs)
  std::string& Str(const std::string& k) { return strState_[k]; }
  int& Int(const std::string& k, int def = 0);
  bool& Flag(const std::string& k, bool def = false);

  // UI-local preferences (ui.kv). Written to disk a moment after the last change (sliders change them every frame).
  const KV& prefs() const { return prefs_; }
  std::string Pref(const std::string& k, const std::string& def = "") const { return prefs_.Str(k, def); }
  void SetPref(const std::string& k, const std::string& v);
  bool PrefB(const std::string& k, bool def) const { return prefs_.Bool(k, def); }
  void FlushPrefs();

  // ---------------------------------------------------------------- session layer (keyboard hook, shortcuts, overlay)
  // These run in the signed-in user's UI process: the service cannot see the keyboard or draw over a game.
  void ApplySession();                    // re-reads the preferences and (re)configures hook, hotkey, overlay and tray icon
  bool SessionLive() const;               // the real window is running (not an offscreen render)
  bool TouchpadGesture() const;           // a precision touchpad is present, so the corner gesture can work
  void BeginKeyCapture();                 // the next key pressed anywhere is reported to TakeCapturedKey (Keys page)
  void CancelKeyCapture();
  bool KeyCapturing() const;
  bool TakeCapturedKey(int* id);
  void FeedCapturedKey(int id);           // tests: pretend the key was pressed
  void RunAction(const std::string& id);  // what a mapped key, the overlay hotkey or the touchpad corner does
  bool OverlayShown() const;

  // elevation helpers (service management)
  void RunElevated(const std::wstring& args, const std::string& doneToast);
  void OpenUrl(const std::string& url);
  void* WindowHandle() const;            // the main window (owner of native dialogs); null while rendering offscreen
  void CopyText(const std::string& text);
  void ApplyAccent();
  void ApplyLanguage();
  std::string ProbeReport() const;   // text for "copy diagnostics"

  float contentWidth() const { return contentW_; }
  bool docked() const { return docked_; }    // a page's side panel is shown right now (the page itself must not draw it again)
  bool shotMode() const { return shot_; }
  void Quit();                           // really exit (tray menu); closing the window may only hide it
  bool Autostart() const;
  void SetAutostart(bool on);

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;

  // ---- backend / data
  void MakeBackend();
  void Tick();                           // results, snapshot, history
  void SessionTick();                    // overlay refresh and key-capture timeout (real window only)
  void AutoRefresh();                    // "lower the refresh rate on battery / follow the profile"
  void UpdateInterest();                 // keep polling while the overlay is up even if the window is hidden
  void Announce(const std::string& title, const std::string& text);   // toast in the window, tray balloon otherwise
  void PushHistory();
  struct Op {
    uint64_t id = 0;
    Request req;
    std::string label;
    std::function<void(const Result&)> done;
  };
  void OnResult(const Op& op, const Result& r);
  struct ChainSt;
  void ChainNext(const std::shared_ptr<ChainSt>& st);
  uint64_t Submit(const Request& r, const std::string& label, std::function<void(const Result&)> done);
  void AskConsent(const Op& op, const std::string& name, const Result& r);
  void AskExperimental(const Op& op);
  void AskActivation(const Op& op);
  void AskApproval(const Op& op, const Result& r);
  std::string CodeText(const std::string& code) const;

  // ---- frame
  void Frame(Gdiplus::Graphics* g, float scale, const Rect& win);
  void DrawRail(const Rect& r);
  void DrawTopBar(const Rect& r);
  void DrawPage(const Rect& r);
  void DrawDock(const Rect& r);
  void DrawOffline(const Rect& r);
  void ApplyShotState();

  Options opt_;
  Ui ui_;
  std::string mode_ = "offline";
  std::unique_ptr<IBackend> backend_;
  KV snap_;
  History hist_;
  double lastHistT_ = 0;
  std::map<std::string, Draft> drafts_;
  std::vector<Op> ops_;
  std::map<std::string, int> pendingKeys_;
  std::map<std::string, double> okAt_;
  std::map<std::string, std::string> strState_;
  std::map<std::string, int> intState_;
  std::map<std::string, bool> flagState_;
  KV prefs_;
  Page page_ = Page::Overview;
  float contentW_ = 800;
  bool docked_ = false;
  bool shot_ = false;
  bool dirty_ = true;
  double lastTickT_ = 0;
  bool prefsDirty_ = false;
  double prefsT_ = 0;
  std::string autoKey_;                  // inputs of the refresh-rate automation when it last decided
  int autoUserHz_ = -1;                  // the rate to go back to when the automation stops lowering it
  double autoT_ = 0;
};

// Pages (each in its own file). `r` is the scroll viewport; they return the content height.
float PageOverview(App& a, const Rect& r);
float PagePrefs(App& a, const Rect& r);
float PageDevices(App& a, const Rect& r);
float PageGpuInfo(App& a, const Rect& r);
float PageOsd(App& a, const Rect& r);
float PageKeys(App& a, const Rect& r);
float PageProfiles(App& a, const Rect& r);
float PageFan(App& a, const Rect& r);
float PageCustom(App& a, const Rect& r);
float PageCustomDock(App& a, const Rect& r);   // advanced CPU registers, shown as a docked panel on the right
float PageApps(App& a, const Rect& r);
float PageLighting(App& a, const Rect& r);
float PageBattery(App& a, const Rect& r);
float PageDisplay(App& a, const Rect& r);
float PageDeviceInfo(App& a, const Rect& r);
float PageSafety(App& a, const Rect& r);
float PageAbout(App& a, const Rect& r);

std::string PageTitle(Page p);

}  // namespace ui
}  // namespace bc
