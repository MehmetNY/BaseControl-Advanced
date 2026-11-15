// Offscreen rendering (README screenshots, `--shot`) and the click-script driver (`--script`) used to test the UI under
// Wine without a person at the keyboard. Both run the exact same Frame() code as the real window.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "../common/registers.h"
#include "../sim/fake_oem.h"
#include "../common/util.h"
#include "app_internal.h"
#include "page.h"

namespace bc {
namespace ui {

namespace {
// Eight hours of a lived-in laptop for screenshots (simulator only): a few hours on battery, a charge, a sleep and the charger again.
std::vector<int> DemoBatteryHistory() {
  std::vector<int> h;
  auto run = [&](int minutes, double from, double to, bool ac) {
    for (int i = 0; i < minutes; i++) {
      const double v = from + (to - from) * (static_cast<double>(i) / std::max(1, minutes - 1));
      h.push_back(static_cast<int>(std::lround(v)) + (ac ? 200 : 0));
    }
  };
  run(70, 90, 71, false);
  run(18, 72, 80, true);
  run(120, 80, 45, false);
  for (int i = 0; i < 26; i++) h.push_back(-1);   // asleep
  run(40, 44, 33, false);
  run(110, 34, 76, true);
  run(480 - static_cast<int>(h.size()), 76, 76, true);
  return h;
}
}  // namespace

namespace {

bool PngClsid(CLSID* out) {
  UINT n = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&n, &size);
  if (!size) return false;
  std::vector<char> buf(size);
  Gdiplus::ImageCodecInfo* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
  Gdiplus::GetImageEncoders(n, size, info);
  for (UINT i = 0; i < n; i++)
    if (wcscmp(info[i].MimeType, L"image/png") == 0) {
      *out = info[i].Clsid;
      return true;
    }
  return false;
}

struct Canvas {
  int pw, ph;
  float scale;
  std::unique_ptr<Gdiplus::Bitmap> bmp;
  std::unique_ptr<Gdiplus::Graphics> g;
  Canvas(int w, int h, float s) : pw(static_cast<int>(w * s)), ph(static_cast<int>(h * s)), scale(s) {
    bmp.reset(new Gdiplus::Bitmap(pw, ph, PixelFormat32bppARGB));
    g.reset(new Gdiplus::Graphics(bmp.get()));
  }
  bool Save(const std::string& path) {
    CLSID png;
    if (!PngClsid(&png)) return false;
    return bmp->Save(Wide(path).c_str(), &png, nullptr) == Gdiplus::Ok;
  }
};
}  // namespace

void App::ApplyShotState() {
  bool ok = false;
  const Page p = PageFromName(opt_.shotPage, &ok);
  if (ok) Go(p);
  if (opt_.shotDrafts && ok && p == Page::Display) {
    // a warmer, more saturated draft: the preview follows it and the picture is marked DRAFT
    struct C { const char* key; int v; };
    for (const C& c : {C{"disp.c.kelvin", 5200}, C{"disp.c.sat", 130}}) {
      if (!Has(c.key)) continue;
      Draft& dr = GetDraft(c.key);
      dr.liveAtStart = static_cast<int>(N(c.key));
      dr.v = c.v;
      dr.active = true;
    }
    return;
  }
  if (opt_.shotDrafts) {
    struct D { const char* key; int delta; };
    // modest edits (a bit quieter, a small GPU offset): the screenshots must not look like a recommendation to overclock
    static const D kDrafts[] = {{"perf.pl1", -25}, {"perf.pl2", -10}, {"perf.gpu.core", 50}, {"perf.gpu.temp", -5}};
    for (const D& d : kDrafts) {
      const SettingDef* def = FindSetting(d.key);
      if (!def) continue;
      const std::string liveKey = d.key;
      if (!Has(liveKey)) continue;
      Draft& dr = GetDraft(d.key);
      dr.liveAtStart = static_cast<int>(N(liveKey));
      dr.v = std::min(def->max, std::max(def->min, dr.liveAtStart + d.delta));
      dr.active = true;
    }
  }
}

int App::RunShot() {
  MakeBackend();
  const double t0 = NowSeconds();
  while (NowSeconds() - t0 < 25) {
    Tick();
    if (Has("host.product") && Has("cap.ec") && connected()) break;
    Sleep(50);
  }
  if (!connected() || !Has("host.product")) {
    std::fprintf(stderr, "shot: no data from the backend within 25 s\n");
    return 3;
  }
  Canvas cv(opt_.shotW, opt_.shotH, opt_.shotScale);
  const Rect win(0, 0, static_cast<float>(opt_.shotW), static_cast<float>(opt_.shotH));

  auto setLoad = [&](double cpu, double gpu) {
    if (!d_->simWorld) return;
    std::lock_guard<std::recursive_mutex> lk(d_->simWorld->plat.mu);
    d_->simWorld->plat.SetLoad(cpu, gpu);
  };
  auto step = [&](int frames, int sleepMs) {
    for (int i = 0; i < frames; i++) {
      Tick();
      Frame(cv.g.get(), cv.scale, win);
      if (sleepMs > 0) Sleep(static_cast<DWORD>(sleepMs));
    }
  };

  // ------------------------------------------------------------------ click script
  if (!opt_.script.empty()) {
    std::ifstream in(opt_.script);
    if (!in) {
      std::fprintf(stderr, "script: cannot open %s\n", opt_.script.c_str());
      return 2;
    }
    ui_.recordHits = true;
    int failures = 0;
    std::string line;
    int lineNo = 0;
    auto mouseTo = [&](float x, float y) {
      ui_.in.mouse = {x, y};
      ui_.in.inWindow = true;
    };
    auto press = [&](float x, float y) {
      mouseTo(x, y);
      step(1, 20);
      ui_.in.down = true;
      ui_.in.pressed = true;
      step(1, 20);
    };
    auto release = [&]() {
      ui_.in.down = false;
      ui_.in.released = true;
      step(2, 30);
    };
    // "let name ui:key|snap:key" remembers a value; expectations accept "$name" (its value), "!want" (must differ) and "*" (anything non-empty)
    std::map<std::string, std::string> vars;
    auto source = [&](const std::string& src) -> std::string {
      if (src.rfind("ui:", 0) == 0) return Str(src.substr(3));
      if (src.rfind("snap:", 0) == 0) return S(src.substr(5));
      return src;
    };
    auto check = [&](int at, const std::string& what, const std::string& got, std::string want) {
      bool negate = false;
      if (!want.empty() && want[0] == '!') {
        negate = true;
        want.erase(0, 1);
      }
      if (!want.empty() && want[0] == '$') {
        auto it = vars.find(want.substr(1));
        want = it == vars.end() ? std::string() : it->second;
      }
      const bool anything = want == "*";
      const bool contains = !want.empty() && want[0] == '~';   // "~text": the value contains the text
      const bool same = anything ? !got.empty() : contains ? got.find(want.substr(1)) != std::string::npos : got == want;
      if (same != negate) {
        std::printf("ok   line %d: %s = %s\n", at, what.c_str(), got.size() > 60 ? (got.substr(0, 57) + "...").c_str() : got.c_str());
      } else {
        std::printf("FAIL line %d: %s = '%s', expected %s'%s'\n", at, what.c_str(), got.c_str(), negate ? "anything but " : "", want.c_str());
        failures++;
      }
    };
    step(6, 25);
    while (std::getline(in, line)) {
      lineNo++;
      line = Trim(line);
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      std::string cmd;
      ss >> cmd;
      if (cmd == "page") {
        std::string n;
        ss >> n;
        bool ok = false;
        const Page p = PageFromName(n, &ok);
        if (!ok) { std::printf("FAIL line %d: unknown page %s\n", lineNo, n.c_str()); failures++; continue; }
        Go(p);
        step(8, 25);
      } else if (cmd == "wait") {
        int ms = 0;
        ss >> ms;
        const double end = NowSeconds() + ms / 1000.0;
        while (NowSeconds() < end) step(1, 40);
      } else if (cmd == "move") {
        float x, y;
        ss >> x >> y;
        mouseTo(x, y);
        step(2, 20);
      } else if (cmd == "click") {
        float x, y;
        ss >> x >> y;
        press(x, y);
        release();
      } else if (cmd == "clickid" || cmd == "dragid" || cmd == "hoverid") {
        std::string id;
        ss >> id;
        step(2, 20);
        auto it = ui_.hits.find(id);
        if (it == ui_.hits.end()) {
          std::printf("FAIL line %d: no widget %s on screen\n", lineNo, id.c_str());
          failures++;
          continue;
        }
        const Rect r = it->second;
        if (cmd == "hoverid") {
          mouseTo(r.cx(), r.cy());
          step(25, 20);
        } else if (cmd == "clickid") {
          press(r.cx(), r.cy());
          release();
          step(6, 40);
        } else {
          float frac = 0.5f;
          ss >> frac;
          const float x0 = r.x + 10, x1 = r.r() - 10;
          press(x0 + (x1 - x0) * 0.5f, r.cy());
          for (int i = 1; i <= 6; i++) {
            mouseTo(x0 + (x1 - x0) * (0.5f + (frac - 0.5f) * static_cast<float>(i) / 6.0f), r.cy());
            step(1, 20);
          }
          release();
        }
      } else if (cmd == "dragby") {
        // press the centre of a widget, move by (dx, dy) in small steps, release (curve handles, anything that is dragged in two directions)
        std::string id;
        float dx = 0, dy = 0;
        ss >> id >> dx >> dy;
        step(2, 20);
        auto it = ui_.hits.find(id);
        if (it == ui_.hits.end()) {
          std::printf("FAIL line %d: no widget %s on screen\n", lineNo, id.c_str());
          failures++;
          continue;
        }
        const Rect r = it->second;
        press(r.cx(), r.cy());
        for (int i = 1; i <= 8; i++) {
          mouseTo(r.cx() + dx * static_cast<float>(i) / 8.0f, r.cy() + dy * static_cast<float>(i) / 8.0f);
          step(1, 20);
        }
        release();
        step(4, 30);
      } else if (cmd == "wheel") {
        float x, y, n;
        ss >> x >> y >> n;
        mouseTo(x, y);
        ui_.in.wheel = n;
        step(4, 25);
      } else if (cmd == "key") {
        std::string k;
        ss >> k;
        KeyEvent ev;
        if (k == "ENTER") ev.vk = VK_RETURN;
        else if (k == "ESC") ev.vk = VK_ESCAPE;
        else if (k == "BACK") ev.vk = VK_BACK;
        else if (k == "TAB") ev.vk = VK_TAB;
        else ev.vk = static_cast<int>(std::strtol(k.c_str(), nullptr, 0));
        ui_.in.keys.push_back(ev);
        step(3, 20);
      } else if (cmd == "type") {
        std::string rest;
        std::getline(ss, rest);
        rest = Trim(rest);
        for (char c : rest) ui_.in.chars.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        step(3, 20);
      } else if (cmd == "shot") {
        std::string f;
        ss >> f;
        step(10, 25);
        if (!cv.Save(f)) { std::printf("FAIL line %d: cannot save %s\n", lineNo, f.c_str()); failures++; }
        else std::printf("shot %s\n", f.c_str());
      } else if (cmd == "expect") {
        std::string key, want;
        ss >> key;
        std::getline(ss, want);
        want = Trim(want);
        step(8, 40);
        check(lineNo, key, S(key), want);
      } else if (cmd == "expectdraft") {
        std::string key;
        int active = 0, v = 0;
        ss >> key >> active;
        const bool hasV = static_cast<bool>(ss >> v);
        const Draft& d = GetDraft(key);
        const bool good = (d.active ? 1 : 0) == active && (!hasV || d.v == v);
        if (good) std::printf("ok   line %d: draft %s active=%d v=%d\n", lineNo, key.c_str(), d.active ? 1 : 0, d.v);
        else { std::printf("FAIL line %d: draft %s active=%d v=%d (expected active=%d%s)\n", lineNo, key.c_str(), d.active ? 1 : 0, d.v, active, hasV ? (" v=" + std::to_string(v)).c_str() : ""); failures++; }
      } else if (cmd == "dump") {
        std::string key;
        ss >> key;
        std::printf("%s = %s\n", key.c_str(), S(key).c_str());
      } else if (cmd == "hits") {
        for (auto& h : ui_.hits) std::printf("hit %-40s %.0f,%.0f %.0fx%.0f\n", h.first.c_str(), h.second.x, h.second.y, h.second.w, h.second.h);
      } else if (cmd == "load") {
        double c, g;
        ss >> c >> g;
        setLoad(c, g);
      } else if (cmd == "external") {
        // Another program changes a value behind Control Center Advanced's back (simulator only): the LIVE readings must follow.
        std::string key, tok;
        ss >> key >> tok;
        const double val = std::atof(tok.c_str());
        bool done = false;
        if (d_->simWorld) {
          std::lock_guard<std::recursive_mutex> lk(d_->simWorld->plat.mu);
          sim::Platform& pl = d_->simWorld->plat;
          const uint8_t b = static_cast<uint8_t>(std::max(0.0, std::min(255.0, val)));
          if (key == "ec.pl1") { pl.ec[reg::kPl1] = b; done = true; }
          else if (key == "ec.pl2") { pl.ec[reg::kPl2] = b; done = true; }
          else if (key == "ec.pl4") { pl.ec[reg::kPl4] = b; done = true; }
          else if (key == "gpu.ctgp") { pl.ec[reg::kGpuCtgpOffset] = static_cast<uint8_t>(std::max(0.0, val - 125.0)); done = true; }
          else if (key == "power.ac") { pl.ac = val != 0; done = true; }
          else if (key == "disp.sat") { d_->simWorld->display.saturationOk = val != 0; done = true; }   // the Windows colour-effect API is there / missing
          else if (key.rfind("storage.", 0) == 0) {
            // storage.health 0|1 (health queries refused / allowed), storage.fail 0|1, storage.<n>.temp|warn|wear <value>
            sim::Storage& st = d_->simWorld->storage;
            std::lock_guard<std::mutex> sl(st.mu);
            if (key == "storage.health") { st.healthDenied = val == 0; done = true; }
            else if (key == "storage.fail") { st.failRead = val != 0; done = true; }
            else {
              int idx = -1;
              char field[16] = {0};
              if (std::sscanf(key.c_str(), "storage.%d.%15s", &idx, field) == 2 && idx >= 0 && idx < static_cast<int>(st.drives.size())) {
                StorageDrive& d = st.drives[static_cast<size_t>(idx)];
                const std::string f = field;
                if (f == "temp") { d.tempC = static_cast<int>(val); done = true; }
                else if (f == "warn") { d.critWarn = static_cast<int>(val); done = true; }
                else if (f == "wear") { d.wearPct = static_cast<int>(val); done = true; }
              }
            }
          }
          else if (key == "oem.installed") {   // the vendor's Control Center is there / uninstalled (an uninstalled one is not running either)
            d_->simWorld->oemProbe.installed = val != 0;
            if (val == 0) {
              d_->simWorld->oemProbe.running = false;
              if (d_->simWorld->oemWorld) d_->simWorld->oemWorld->SetServiceUp(false);
            }
            done = true;
          }
          else if (key == "oem.running") {     // its services were started / stopped (the link follows: connecting fails while they are down)
            d_->simWorld->oemProbe.running = val != 0;
            if (d_->simWorld->oemWorld) d_->simWorld->oemWorld->SetServiceUp(val != 0);
            done = true;
          }
          else if (key == "oem.silent") {      // the broker works but the service does not answer
            if (d_->simWorld->oemWorld) d_->simWorld->oemWorld->SetSilent(val != 0);
            done = true;
          }
          else if (key.rfind("kb.", 0) == 0) {   // the vendor's service reports a keyboard light change that another program made
            // kb.power 0|1, kb.bright 0..4, kb.speed 0..4, kb.effect <id>, kb.direction -1..5, kb.color #rrggbb (every block), kb.timer <seconds>
            if (d_->simWorld->oemWorld) {
              const std::string what = key.substr(3);
              long long v = static_cast<long long>(val);
              if (what == "color") v = std::strtoll(tok.c_str() + (tok.size() && tok[0] == '#' ? 1 : 0), nullptr, 16);
              d_->simWorld->oemWorld->ExternalKeyboard(what, v);
              done = true;
            }
          }
          else if (key == "batt.mode") {   // 0 full capacity, 1 balanced, 2 eco
            pl.ec[reg::kBatteryMode] = static_cast<uint8_t>((pl.ec[reg::kBatteryMode] & ~0x38) | (val < 0.5 ? 0x08 : val < 1.5 ? 0x18 : 0x28));
            done = true;
          }
        }
        if (!done) { std::printf("FAIL line %d: cannot change %s from outside\n", lineNo, key.c_str()); failures++; }
        step(4, 30);
      } else if (cmd == "cmd") {
        // a controller command as a background watcher would send it: cmd light.idle.state on=1
        std::string name, kv;
        ss >> name;
        KV args;
        while (ss >> kv) {
          const size_t eq = kv.find('=');
          if (eq != std::string::npos) args.Set(kv.substr(0, eq), kv.substr(eq + 1));
        }
        CmdQuiet(name, args);
        step(8, 30);
      } else if (cmd == "battery_history") {
        // fills the controller's eight hour battery history with a lived-in demo (simulator only; "empty" = a single point)
        std::string what;
        ss >> what;
        if (d_->ctl) d_->ctl->SeedBatteryHistory(what == "empty" ? std::vector<int>{276} : DemoBatteryHistory());
        else { std::printf("FAIL line %d: battery_history needs the in-process controller\n", lineNo); failures++; }
        step(6, 30);
      } else if (cmd == "notify") {
        // a power event from Windows (simulator only): resume, suspend, display_on, display_off, ac_change
        std::string ev;
        ss >> ev;
        if (d_->ctl) d_->ctl->Notify(ev);
        else { std::printf("FAIL line %d: notify needs the in-process controller\n", lineNo); failures++; }
        step(4, 30);
      } else if (cmd == "running") {
        // the programs the simulated computer is running (names separated by spaces; none = nothing besides the shell)
        std::vector<std::string> exes{"explorer.exe"};
        std::string n;
        while (ss >> n) exes.push_back(n);
        if (d_->simWorld) {
          std::lock_guard<std::recursive_mutex> lk(d_->simWorld->plat.mu);
          d_->simWorld->power.exes = exes;
        } else {
          std::printf("FAIL line %d: running needs the simulator\n", lineNo);
          failures++;
        }
        step(4, 30);
      } else if (cmd == "capture") {
        // pretend the Keys page received a key press (scan code | 0x100 for extended keys)
        int id = 0;
        ss >> id;
        FeedCapturedKey(id);
        step(3, 20);
      } else if (cmd == "fire") {
        std::string act;
        ss >> act;
        RunAction(act);
        step(6, 30);
      } else if (cmd == "expectpref") {
        std::string key, want;
        ss >> key;
        std::getline(ss, want);
        want = Trim(want);
        step(2, 20);
        check(lineNo, "pref " + key, Pref(key, ""), want);
      } else if (cmd == "expectui") {
        // the page's own state (a draft kept in App::Str); no value = must be empty
        std::string key, want;
        ss >> key;
        std::getline(ss, want);
        want = Trim(want);
        step(2, 20);
        if (want.empty()) {
          const std::string got = Str(key);
          if (got.empty()) std::printf("ok   line %d: ui %s is empty\n", lineNo, key.c_str());
          else { std::printf("FAIL line %d: ui %s = '%s', expected empty\n", lineNo, key.c_str(), got.c_str()); failures++; }
        } else {
          check(lineNo, "ui " + key, Str(key), want);
        }
      } else if (cmd == "expectmodal") {
        // whether a dialog is on screen: 1 = open, 0 = closed
        int want = 0;
        ss >> want;
        step(2, 20);
        const bool open = ui_.ModalOpen();
        if (open == (want != 0)) std::printf("ok   line %d: dialog is %s\n", lineNo, open ? "open" : "closed");
        else { std::printf("FAIL line %d: dialog is %s, expected %s\n", lineNo, open ? "open" : "closed", want != 0 ? "open" : "closed"); failures++; }
      } else if (cmd == "expecthit") {
        // whether a control is on screen right now: "expecthit <id> 1" (present) or "expecthit <id> 0" (absent)
        std::string id;
        int want = 1;
        ss >> id >> want;
        step(2, 20);
        const bool there = ui_.hits.find(id) != ui_.hits.end();
        if (there == (want != 0)) std::printf("ok   line %d: %s is %s\n", lineNo, id.c_str(), there ? "on screen" : "not on screen");
        else { std::printf("FAIL line %d: %s is %s, expected %s\n", lineNo, id.c_str(), there ? "on screen" : "not on screen", want != 0 ? "on screen" : "not on screen"); failures++; }
      } else if (cmd == "expectint") {
        // a number the page keeps in App::Int (a tile choice, a tab)
        std::string key, want;
        ss >> key;
        std::getline(ss, want);
        want = Trim(want);
        step(2, 20);
        check(lineNo, "int " + key, std::to_string(Int(key)), want);
      } else if (cmd == "let") {
        std::string name, src;
        ss >> name >> src;
        vars[name] = source(src);
        std::printf("let  %s = %s\n", name.c_str(), vars[name].c_str());
      } else if (cmd == "say") {
        std::string rest;
        std::getline(ss, rest);
        std::printf("%s\n", Trim(rest).c_str());
      } else {
        std::printf("FAIL line %d: unknown command %s\n", lineNo, cmd.c_str());
        failures++;
      }
    }
    std::printf("script finished: %d failure(s)\n", failures);
    return failures ? 1 : 0;
  }

  // ------------------------------------------------------------------ one screenshot
  bool known = false;
  const Page p = PageFromName(opt_.shotPage, &known);
  const bool charts = known && (p == Page::Overview || p == Page::Fan || p == Page::Custom || p == Page::DeviceInfo || p == Page::Osd);
  const double preroll = charts ? 12.0 : 3.0;
  const double start = NowSeconds();
  while (NowSeconds() - start < preroll) {
    const double t = NowSeconds() - start;
    if (t < 3) setLoad(0.10, 0.04);
    else if (t < 8.5) setLoad(0.92, 0.78);
    else setLoad(0.55, 0.38);
    Tick();
    Sleep(40);
  }
  ApplyShotState();
  step(6, 20);
  if (known && p == Page::Custom) {
    // the sliders belong to the Customized mode: switch to it so the picture shows live values, not a hint
    KV m;
    m.Set("name", "Customized");
    Cmd("profile.apply", m);
    step(12, 30);
    const double calm = NowSeconds() + 4.6;   // let the "applied" toast fade out before the picture is taken
    while (NowSeconds() < calm) step(1, 40);
  }
  if (opt_.shotDrafts && known && p == Page::Apps) {
    // a lived-in Application profiles page: four programs, one of them running right now
    struct Row { const char* exe; const char* profile; const char* name; };
    const Row rows[] = {{"cyberpunk2077.exe", "Overboost", "Cyberpunk 2077"},
                        {"blender.exe", "Overboost", "Blender"},
                        {"cs2.exe", "Enthusiast", "Counter-Strike 2"},
                        {"chrome.exe", "Balanced", "Google Chrome"}};
    for (const Row& r : rows) {
      KV k;
      k.Set("exe", r.exe);
      k.Set("profile", r.profile);
      k.Set("name", r.name);
      Cmd("app.set", k);
      step(4, 30);
    }
    if (d_->simWorld) {
      std::lock_guard<std::recursive_mutex> lk(d_->simWorld->plat.mu);
      d_->simWorld->power.exes = {"explorer.exe", "cs2.exe", "chrome.exe"};
    }
    const double until = NowSeconds() + 5.5;   // the controller looks at the running programs every few seconds
    while (NowSeconds() < until) step(1, 40);
  }
  if (opt_.shotDrafts && known && p == Page::Battery && d_->ctl) {
    d_->ctl->SeedBatteryHistory(DemoBatteryHistory());
    step(8, 30);
  }
  ApplyShotState();   // drafts need live values from the frames above
  step(24, 25);
  if (!cv.Save(opt_.shotOut)) {
    std::fprintf(stderr, "shot: cannot save %s\n", opt_.shotOut.c_str());
    return 4;
  }
  std::printf("wrote %s (%dx%d)\n", opt_.shotOut.c_str(), cv.pw, cv.ph);
  return 0;
}

}  // namespace ui
}  // namespace bc
