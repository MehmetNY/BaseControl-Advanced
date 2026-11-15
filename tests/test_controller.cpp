// Controller flows against the simulated machine and the in-memory stand-in for the vendor's Control Center service: probing, routing of
// every profile value / fan / battery / keyboard change through the service with read-back from the EC, the kill switch, the one feature
// that still writes to the EC (the extended GPU power), live values, application profiles, persistence.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../src/common/controller.h"
#include "../src/common/fan.h"
#include "../src/common/registers.h"
#include "../src/oem/link.h"
#include "../src/oem/proto.h"
#include "../src/sim/fake_oem.h"
#include "../src/sim/sim.h"
#include "mini_test.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace bc;

namespace {

// The stand-in service answers from another thread, so waiting for it is real time (the controller's virtual clock does not move).
bool WaitFor(const std::function<bool()>& pred, int ms = 4000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!pred()) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

Controller::Options Opts(const std::string& dir = "") {
  Controller::Options o;
  o.manual = true;
  o.mode = "sim";
  o.dataDir = dir;
  return o;
}

struct Rig {
  sim::World w;
  Controller c;
  bool linked = false;
  explicit Rig(sim::Model m = sim::Model::Exact, const std::string& dir = "", bool waitForService = true) : w(m), c(w.hw(), Opts(dir)) {
    c.Start();
    if (waitForService) linked = Link();
  }

  // Waits until the stand-in service has told everything the pages show, then lets the controller publish it.
  bool Link() {
    const bool ok = WaitFor([&] {
      if (!w.oemLink || !w.oemLink->Connected()) return false;
      const KV st = w.oemLink->State();
      return st.Has("oem.fan.OperatingMode") && st.Has("oem.table.Name") && st.Has("oem.kb.effect") && st.Has("oem.batt.HealthProtectionStatus");
    });
    c.PollOnce(true);
    return ok;
  }

  Result Set(const std::string& k, const std::string& v, const KV& args = KV()) {
    Request r;
    r.op = "set";
    r.key = k;
    r.value = v;
    r.args = args;
    return c.ExecuteSync(r);
  }
  Result Cmd(const std::string& k, const KV& args = KV()) {
    Request r;
    r.op = "cmd";
    r.key = k;
    r.args = args;
    return c.ExecuteSync(r);
  }
  Result Cfg(const std::string& k, const std::string& v) {
    Request r;
    r.op = "cfg";
    r.key = k;
    r.value = v;
    return c.ExecuteSync(r);
  }
  Result Profile(const char* name) {
    KV a;
    a.Set("name", name);
    return Cmd("profile.apply", a);
  }
  KV S() { return c.Snapshot(); }
  uint8_t Ec(uint16_t a) { return w.plat.ec[a]; }
  sim::FakeOemWorld& Svc() { return *w.oemWorld; }
  // How many control messages the service has received that contain `needle`.
  int Sent(const std::string& needle) {
    int n = 0;
    for (const std::string& m : Svc().Received())
      if (m.find(needle) != std::string::npos) n++;
    return n;
  }
  // advance the virtual clock and poll like the worker thread would
  void Run(int seconds, int stepMs = 1000) {
    for (int t = 0; t < seconds * 1000; t += stepMs) {
      w.clock.Advance(stepMs);
      c.PollOnce(false);
    }
  }
};

KV Consent(const char* name) {
  KV a;
  a.Set(std::string("consent.") + name, "1");
  return a;
}

std::string TempDir() {
  char tmpl[] = "/tmp/bctestXXXXXX";
  char* d = mkdtemp(tmpl);
  return d ? std::string(d) : std::string();
}

void RemoveDir(const std::string& dir) {
  const std::string cmd = "rm -rf " + dir;
  int rc = std::system(cmd.c_str());
  (void)rc;
}

}  // namespace

// ------------------------------------------------------------------------------------------------ probing / classes
TEST(ctl_probe_exact_device) {
  Rig r;
  CHECK(r.linked);
  KV s = r.S();
  CHECK_EQ(s.Str("dev.class"), std::string("verified"));
  CHECK_EQ(s.Str("ec.link"), std::string("connected"));
  for (const char* cap : {"ec", "ec.pl", "ec.tcc", "ec.fan", "ec.batt", "ec.fnlock", "ec.acboot", "ec.usbpower", "fan.read", "oem", "perf.pl", "perf.tcc",
                          "perf.gpu", "perf.gpu.temp", "fan.ctl", "fan.curve", "batt.mode", "fnlock", "acboot", "usbpower", "light.kbd", "light.kbd.timer", "gpu.nvml",
                          "gpu.ctgp", "disp.bright"})
    CHECK_EQ(s.Str(std::string("cap.") + cap), std::string("ok"));
  CHECK_EQ(s.Int("dev.project"), 26LL);
  // the profile in use and its values: live from the EC, limits from the service
  CHECK_EQ(s.Int("perf.mode"), 2LL);
  CHECK_EQ(s.Str("profile.active"), std::string("Overboost"));
  CHECK_EQ(s.Int("perf.pl1"), 120LL);
  CHECK_EQ(s.Int("perf.pl2"), 120LL);
  CHECK_EQ(s.Int("perf.pl4"), 200LL);
  CHECK_EQ(s.Int("perf.pl1.min"), 10LL);
  CHECK_EQ(s.Int("perf.pl1.max"), 200LL);
  CHECK_EQ(s.Int("perf.pl4.max"), 250LL);
  CHECK(!s.Bool("perf.tcc.on"));
  CHECK_EQ(s.Int("perf.tcc"), 5LL);
  CHECK_EQ(s.Int("perf.tcc.max"), 25LL);
  CHECK_EQ(s.Int("perf.gpu.temp"), 87LL);
  CHECK(s.Bool("perf.oc"));
  CHECK_EQ(s.Int("ec.pl1"), 120LL);
  CHECK_EQ(s.Int("ec.pl4"), 200LL);
  CHECK_EQ(s.Int("ec.tcc"), 100LL);
  CHECK_EQ(s.Int("gpu.ctgp"), 140LL);
  CHECK_EQ(s.Str("fan.mode"), std::string("auto"));
  CHECK_EQ(s.Str("batt.mode"), std::string("balanced"));
  CHECK_EQ(s.Str("profile.list"), std::string("Balanced,Enthusiast,Overboost,Customized"));
  // keyboard lighting as the service reports it
  CHECK(s.Bool("light.kbd.power"));
  CHECK_EQ(s.Str("light.kbd.effect"), std::string("static"));
  CHECK_EQ(s.Int("light.kbd.blocks"), 8LL);
  CHECK_EQ(s.Int("light.kbd.bright"), 3LL);
  CHECK_EQ(s.Int("light.kbd.speed"), 2LL);
  CHECK_EQ(s.Str("light.kbd.color"), std::string("#ffffff"));
  // the link
  CHECK_EQ(s.Str("oem.link.state"), std::string("connected"));
  CHECK_EQ(s.Str("oem.link.class"), std::string("UWPClient"));
  CHECK(s.Bool("oem.up"));
  CHECK(s.Num("cpu.temp") > 20 || s.Has("cpu.temp.ec"));
  CHECK_EQ(s.Str("oem.services"), std::string("gcuService,GcuBridge"));
  CHECK_EQ(s.Int("ec.writes"), 0LL);   // probing never writes
}

TEST(ctl_probe_never_writes_to_ec) {
  Rig r;
  CHECK_EQ(r.w.plat.ecWrites, 0);
  r.Cmd("probe");
  CHECK_EQ(r.w.plat.ecWrites, 0);
  r.Run(5);
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_family_and_unknown_classes) {
  Rig fam(sim::Model::Family);
  CHECK_EQ(fam.S().Str("dev.class"), std::string("family"));
  CHECK_EQ(fam.S().Str("cap.gpu.ctgp"), std::string("off"));   // the GPU power engine only trusts the exact unit
  CHECK_EQ(fam.Set("gpu.ctgp", "125").code, std::string("unsupported"));
  CHECK_EQ(fam.w.plat.ecWrites, 0);
  Rig unk(sim::Model::Unknown);
  CHECK_EQ(unk.S().Str("dev.class"), std::string("unknown"));
  CHECK_EQ(unk.S().Str("cap.gpu.ctgp"), std::string("off"));
  CHECK_EQ(unk.Set("gpu.ctgp", "125").code, std::string("unsupported"));
  CHECK_EQ(unk.w.plat.ecWrites, 0);
}

TEST(ctl_absent_hardware_degrades) {
  sim::World w;
  w.ec.state = mgp::DriverState::Absent;
  w.plat.nvmlAvailable = false;
  Controller c(w.hw(), Opts());
  c.Start();
  WaitFor([&] { return w.oemLink->Connected() && w.oemLink->State().Has("oem.fan.OperatingMode"); });
  c.PollOnce(true);
  KV s = c.Snapshot();
  CHECK_EQ(s.Str("cap.ec"), std::string("absent"));
  CHECK_EQ(s.Str("cap.ec.pl"), std::string("absent"));
  CHECK_EQ(s.Str("cap.gpu.nvml"), std::string("absent"));
  CHECK_EQ(s.Str("cap.gpu.ctgp"), std::string("absent"));
  CHECK_EQ(s.Str("ec.link"), std::string("absent"));
  // the vendor's service and Windows keep working; without the EC a change is confirmed by the service's own report only
  CHECK_EQ(s.Str("cap.perf.pl"), std::string("ok"));
  CHECK_EQ(s.Int("perf.pl1"), 120LL);
  Request q;
  q.op = "cmd"; q.key = "profile.apply"; q.args.Set("name", "Customized");
  CHECK(c.ExecuteSync(q).ok);
  Request p;
  p.op = "set"; p.key = "perf.pl1"; p.value = "90";
  Result res = c.ExecuteSync(p);
  CHECK(res.ok);
  CHECK(res.msg.find("and the EC") == std::string::npos);
  p.key = "disp.bright"; p.value = "40";
  CHECK(c.ExecuteSync(p).ok);
  // the extra GPU power is refused, nothing is written
  p.key = "gpu.ctgp"; p.value = "125";
  CHECK(!c.ExecuteSync(p).ok);
  CHECK_EQ(w.plat.ecWrites, 0);
}

TEST(ctl_no_link_in_this_build) {
  sim::World w;
  Hardware hw = w.hw();
  hw.oemLink = nullptr;
  Controller c(hw, Opts());
  c.Start();
  KV s = c.Snapshot();
  CHECK_EQ(s.Str("cap.oem"), std::string("absent"));
  CHECK_EQ(s.Str("oem.link.state"), std::string("none"));
  CHECK(!s.Bool("oem.up"));
  Request p;
  p.op = "set"; p.key = "perf.pl1"; p.value = "90";
  CHECK_EQ(c.ExecuteSync(p).code, std::string("unsupported"));
  p.key = "fan.mode"; p.value = "boost";
  CHECK_EQ(c.ExecuteSync(p).code, std::string("unsupported"));
  p.key = "gpu.ctgp"; p.value = "125";   // the program's own engine does not need the service
  CHECK(c.ExecuteSync(p).ok);
  Request q;
  q.op = "cmd"; q.key = "profile.apply"; q.args.Set("name", "Balanced");
  CHECK_EQ(c.ExecuteSync(q).code, std::string("needs.service"));
}

// ------------------------------------------------------------------------------------------------ request validation
TEST(ctl_bad_requests) {
  Rig r;
  CHECK_EQ(r.Set("does.not.exist", "1").code, std::string("unknown"));
  CHECK_EQ(r.Set("ec.pl1", "100").code, std::string("unknown"));   // removed: the EC is not written directly any more
  CHECK_EQ(r.Set("perf.pl1", "banana").code, std::string("range"));
  CHECK_EQ(r.Set("perf.pl1", "4").code, std::string("range"));
  CHECK_EQ(r.Set("perf.pl1", "900").code, std::string("range"));
  CHECK_EQ(r.Set("gpu.ctgp", "141").code, std::string("range"));
  CHECK_EQ(r.Set("fan.mode", "fixed").code, std::string("range"));
  CHECK_EQ(r.Cfg("notcfg.key", "1").code, std::string("range"));
  CHECK_EQ(r.Cfg("cfg.bad key!", "1").code, std::string("range"));
  Request q;
  q.op = "frobnicate";
  CHECK_EQ(r.c.ExecuteSync(q).code, std::string("unknown"));
  CHECK_EQ(r.Cmd("nope").code, std::string("unknown"));
  CHECK_EQ(r.w.plat.ecWrites, 0);                                  // nothing reached the EC
  CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), 0);                // ... and nothing reached the service
}

// ------------------------------------------------------------------------------------------------ the profile in use
TEST(ctl_profile_switching_goes_through_the_service) {
  Rig r;
  CHECK_EQ(r.Profile("Balanced").ok, true);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
  CHECK_EQ(r.S().Int("perf.mode"), 0LL);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(45));             // the service applied the profile's own values
  CHECK_EQ(r.S().Int("perf.pl1"), 45LL);                            // ... and the page shows them right away
  CHECK_EQ(r.Sent("OPERATING_OFFICE_MODE"), 1);
  CHECK(r.Profile("Enthusiast").ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(80));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  CHECK(r.Profile("overboost").ok);                                 // the name is not case sensitive
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(200));
  CHECK(r.Profile("Customized").ok);
  CHECK_EQ(r.S().Int("perf.mode"), 3LL);
  CHECK_EQ(r.Sent("OPERATING_CUSTOM_MODE"), 1);
  // already active: nothing is sent again
  const int before = static_cast<int>(r.Svc().Received().size());
  CHECK(r.Profile("Customized").ok);
  CHECK_EQ(static_cast<int>(r.Svc().Received().size()), before);
  // no such profile
  CHECK_EQ(r.Profile("Quiet").code, std::string("unknown"));
  CHECK_EQ(r.Profile("").code, std::string("unknown"));
  CHECK_EQ(r.w.plat.ecWrites, 0);                                   // the program wrote nothing itself
}

TEST(ctl_snapshot_follows_other_programs) {
  Rig r;
  CHECK_EQ(r.S().Int("perf.pl1"), 120LL);
  // 1. another program changes the EC behind everybody's back: the live value follows, the profile's own value stays visible
  r.w.plat.ec[reg::kPl1] = 77;
  r.w.plat.ec[reg::kPl2] = 99;
  r.w.plat.SetEcProfile(true, 25);
  r.Run(2);
  KV s = r.S();
  CHECK_EQ(s.Int("perf.pl1"), 77LL);
  CHECK_EQ(s.Int("perf.pl2"), 99LL);
  CHECK_EQ(s.Int("perf.pl1.profile"), 120LL);
  CHECK_EQ(s.Int("ec.pl1"), 77LL);
  CHECK_EQ(s.Int("gpu.ctgp"), 150LL);
  // 2. another client of the vendor's service (the vendor's own window, a hotkey) switches the profile: the page follows without a request
  oem::LinkConfig lc = r.Svc().LinkConfigFor();
  lc.slot = 7;
  oem::MqttOemLink other(lc);
  other.Start();
  CHECK(WaitFor([&] { return other.Connected(); }));
  CHECK(other.Publish(oem::msg::FanMode(oem::msg::Mode::Office, -1)));
  CHECK(WaitFor([&] { return r.w.oemLink->State().Str("oem.fan.OperatingMode") == "0"; }));
  r.Run(1);
  s = r.S();
  CHECK_EQ(s.Int("perf.mode"), 0LL);
  CHECK_EQ(s.Str("profile.active"), std::string("Balanced"));
  CHECK_EQ(s.Int("perf.pl1"), 45LL);
  // 3. and a value changed through the service by someone else
  oem::Msg m;
  CHECK(oem::msg::FanDetail({{"PL1", 50}}, &m));
  r.Svc().SetState([&] { sim::FakeServiceState st = r.Svc().State(); st.detailAnyMode = true; return st; }());
  CHECK(other.Publish(m));
  CHECK(WaitFor([&] { return r.w.oemLink->State().Str("oem.fan.CPU_PL1") == "50"; }));
  r.Run(1);
  CHECK_EQ(r.S().Int("perf.pl1"), 50LL);
  other.Stop();
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

// ------------------------------------------------------------------------------------------------ profile values (PL1 / PL2 / PL4 / offsets)
TEST(ctl_set_pl_in_the_customized_profile) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(80));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(100));
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(150));
  Result res = r.Set("perf.pl1", "90");
  CHECK(res.ok);
  CHECK(res.msg.find("EC") != std::string::npos);                    // confirmed by the service and the EC
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(90));
  CHECK_EQ(r.S().Int("perf.pl1"), 90LL);
  CHECK_EQ(r.S().Int("perf.pl1.profile"), 90LL);
  CHECK_EQ(r.Sent("\"PL1\":90"), 1);
  CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), 1);                  // only the value that changed travels
  CHECK(r.Set("perf.pl4", "170").ok);
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(170));
  CHECK(r.Set("perf.pl2", "130").ok);
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(130));
  CHECK_EQ(r.S().Int("perf.pl2"), 130LL);
  CHECK_EQ(r.w.plat.ecWrites, 0);                                    // the service moved the EC, the program did not
  CHECK_EQ(r.w.plat.Pl1W(), 90.0);                                   // and the CPU obeys it
}

TEST(ctl_pl_ordering_and_service_limits_are_enforced_before_anything_is_sent) {
  Rig r;
  CHECK(r.Profile("Customized").ok);                                 // 80 / 100 / 150
  const int base = r.Sent("SET_OPERATING_MODE_DETAIL");
  CHECK_EQ(r.Set("perf.pl1", "130").code, std::string("range"));     // above PL2
  CHECK_EQ(r.Set("perf.pl2", "70").code, std::string("range"));      // below PL1
  CHECK_EQ(r.Set("perf.pl2", "160").code, std::string("range"));     // above PL4
  CHECK_EQ(r.Set("perf.pl4", "90").code, std::string("range"));      // below PL2
  CHECK_EQ(r.Set("perf.pl1", "9").code, std::string("range"));       // below the service's own minimum (10)
  CHECK_EQ(r.Set("perf.pl4", "251").code, std::string("range"));     // above the registry's outer limit
  CHECK_EQ(r.Set("perf.gpu.temp", "70").code, std::string("range")); // the service allows 75..87
  CHECK_EQ(r.Set("perf.gpu.temp", "95").code, std::string("range"));
  CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), base);
  // raising the upper value first makes room for the lower one (the order a page follows when it applies several)
  CHECK(r.Set("perf.pl4", "200").ok);
  CHECK(r.Set("perf.pl2", "150").ok);
  CHECK(r.Set("perf.pl1", "140").ok);
  CHECK_EQ(r.S().Int("perf.pl1"), 140LL);
  CHECK_EQ(r.S().Int("perf.pl2"), 150LL);
  CHECK_EQ(r.S().Int("perf.pl4"), 200LL);
}

TEST(ctl_pl4_follows_the_ec_scale_bit) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  r.w.plat.ec[reg::kCustomProfile] |= 0x80;                          // PL4 is stored as half the watts
  r.Svc().SetState(r.Svc().State());                                 // the service writes it again that way
  r.c.PollOnce(true);
  CHECK_EQ(r.S().Int("ec.pl4"), 150LL);
  CHECK_EQ(r.S().Int("perf.pl4"), 150LL);
  CHECK(r.Set("perf.pl4", "170").ok);
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(85));
  CHECK_EQ(r.S().Int("perf.pl4"), 170LL);
}

TEST(ctl_a_ready_made_mode_is_never_written_to_and_nothing_is_sent) {
  Rig r;                                                             // Overboost: the service keeps its own stored values for this mode
  const int base = r.Sent("SET_OPERATING_MODE_DETAIL");
  const struct { const char* key; const char* value; } kTry[] = {{"perf.pl1", "100"},   {"perf.pl2", "130"},       {"perf.pl4", "190"},     {"perf.tcc", "10"},
                                                                  {"perf.gpu.core", "50"}, {"perf.gpu.mem", "50"}, {"perf.gpu.temp", "80"}};
  for (const auto& t : kTry) {                                       // all of them inside their ranges, so only the mode can be the reason
    const Result res = r.Set(t.key, t.value);
    CHECK(!res.ok);
    CHECK_EQ(res.code, std::string("gate.mode"));
  }
  CHECK(!r.Set("perf.tcc.on", "1").ok);
  CHECK_EQ(r.Set("perf.tcc.on", "1").code, std::string("gate.mode"));
  CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), base);               // not a single message reached the service
  CHECK_EQ(r.Cmd("perf.restore").code, std::string("gate.mode"));    // "restore the defaults" resets the active mode, so the same rule applies
  CHECK_EQ(r.Sent("RESTORE_OPERATING_MODE_DETAIL"), 0);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  CHECK_EQ(r.S().Int("perf.pl1"), 120LL);                            // the page still shows what runs
  CHECK_EQ(r.S().Int("perf.pl1.profile"), 120LL);                    // and the mode's stored value is untouched
  // the same holds for the other two ready-made modes
  CHECK(r.Profile("Balanced").ok);
  CHECK_EQ(r.Set("perf.pl1", "40").code, std::string("gate.mode"));
  CHECK(r.Profile("Enthusiast").ok);
  CHECK_EQ(r.Set("perf.pl2", "90").code, std::string("gate.mode"));
  CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), base);
  // the Custom mode is where the values are edited
  CHECK(r.Profile("Customized").ok);
  CHECK(r.Set("perf.pl1", "90").ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(90));
  CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), base + 1);
}

TEST(ctl_a_profile_value_is_not_sent_while_the_service_has_not_said_which_mode_is_active) {
  sim::World w(sim::Model::Exact);
  w.oemWorld->SetSilent(true);                                       // connected, but the service never answers: no status, so no active mode known
  Controller c(w.hw(), Opts());
  c.Start();
  CHECK(WaitFor([&] { return w.oemLink && w.oemLink->Connected(); }));
  c.PollOnce(true);
  CHECK(!w.oemLink->State().Has("oem.fan.OperatingMode"));
  // the controls are locked (unsupported) long before a message could be built; whatever the reason, nothing may reach the service
  const char* keys[] = {"perf.pl1", "perf.pl2", "perf.pl4", "perf.tcc", "perf.gpu.core", "perf.gpu.mem", "perf.gpu.temp", "perf.tcc.on"};
  for (const char* k : keys) {
    Request req;
    req.op = "set";
    req.key = k;
    req.value = k == std::string("perf.tcc.on") ? "1" : "60";
    CHECK(!c.ExecuteSync(req).ok);
  }
  for (const std::string& m : w.oemWorld->Received()) CHECK(m.find("SET_OPERATING_MODE_DETAIL") == std::string::npos);   // not one reached the service
  c.Stop();
}

TEST(ctl_an_ec_that_does_not_follow_is_reported_in_the_customized_profile_too) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  r.Svc().SetEcWritesApply(false);                                   // the service accepts but the EC never changes
  Result res = r.Set("perf.pl1", "90");
  CHECK_EQ(res.code, std::string("failed.verify"));
  CHECK(res.msg.find("Customized") == std::string::npos);            // no hint here: the profile is the right one
  CHECK(res.msg.find("PL1") != std::string::npos);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(80));
}

TEST(ctl_a_silent_service_is_reported_and_nothing_is_called_applied) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  r.Svc().SetSilent(true);
  Result res = r.Set("perf.pl1", "90");
  CHECK_EQ(res.code, std::string("failed.service"));
  CHECK_EQ(r.S().Int("perf.pl1"), 80LL);
  r.Svc().SetSilent(false);
  CHECK(r.Set("perf.pl1", "90").ok);
}

TEST(ctl_cpu_temperature_offset_and_gpu_offsets) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  CHECK(r.Set("perf.tcc.on", "1").ok);
  CHECK(r.S().Bool("perf.tcc.on"));
  CHECK(r.Set("perf.tcc", "10").ok);
  CHECK_EQ(r.S().Int("perf.tcc"), 10LL);
  CHECK_EQ(r.Ec(reg::kCpuTempLimitRead) & 0x7F, 15);                // the EC holds 5 + offset
  CHECK_EQ(r.S().Int("ec.tcc"), 90LL);
  CHECK_EQ(r.Sent("\"CpuTccOffsetSwitch\":1"), 1);
  CHECK_EQ(r.Sent("\"CpuTccOffset\":10"), 1);
  CHECK_EQ(r.Set("perf.tcc", "26").code, std::string("range"));     // the service allows 25 at most
  // GPU clock offsets: the profile's overclocking switch is on, so the OC variants are the ones in use
  CHECK(r.S().Bool("perf.oc"));
  CHECK(r.Set("perf.gpu.core", "-100").ok);
  CHECK_EQ(r.Sent("\"GpuCoreClockOffsetOC\":-100"), 1);
  CHECK_EQ(r.w.plat.gpuCoreOffset, -100);
  CHECK_EQ(r.S().Int("perf.gpu.core"), -100LL);                     // the graphics driver reports it back
  CHECK(r.Set("perf.gpu.mem", "250").ok);
  CHECK_EQ(r.w.plat.gpuMemOffset, 250);
  CHECK(r.Set("perf.gpu.temp", "80").ok);
  CHECK_EQ(r.S().Int("perf.gpu.temp"), 80LL);
  // with the switch off the plain fields are used
  sim::FakeServiceState st = r.Svc().State();
  st.detail[3].oc = 0;
  r.Svc().SetState(st);
  r.Svc().ClearReceived();
  r.w.oemLink->RequestAll();
  CHECK(WaitFor([&] { return r.w.oemLink->State().Str("oem.fan.OverClockingSwitch") == "0"; }));
  r.Run(1);
  CHECK(!r.S().Bool("perf.oc"));
  CHECK(r.Set("perf.gpu.mem", "300").ok);
  CHECK_EQ(r.Sent("\"GpuMemoryClockOffset\":300"), 1);
  CHECK_EQ(r.Sent("OC\":"), 0);
  // what the service may never be asked to change is not reachable from here at all
  CHECK(r.S().Int("perf.gpu.core.max") > 0);
}

TEST(ctl_restore_defaults_asks_the_service) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  CHECK(r.Set("perf.pl1", "95").ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(95));
  CHECK(r.Cmd("perf.restore").ok);
  CHECK_EQ(r.Sent("RESTORE_OPERATING_MODE_DETAIL"), 1);
  CHECK(WaitFor([&] { return r.w.oemLink->State().Str("oem.fan.CPU_PL1") == "80"; }));
  r.Run(1);
  CHECK_EQ(r.S().Int("perf.pl1"), 80LL);
}

// ------------------------------------------------------------------------------------------------ the service is not there
TEST(ctl_an_unreachable_service_blocks_service_settings_and_recovers) {
  Rig r;
  r.Svc().SetServiceUp(false);
  CHECK(WaitFor([&] { return !r.w.oemLink->Connected(); }));
  r.Run(2);
  KV s = r.S();
  CHECK(!s.Bool("oem.up"));
  CHECK_EQ(s.Str("oem.link.state") == "connected", false);
  for (const char* cap : {"perf.pl", "perf.tcc", "perf.gpu", "fan.ctl", "fan.curve", "batt.mode", "fnlock", "light.kbd"})
    CHECK_EQ(s.Str(std::string("cap.") + cap), std::string("offline"));
  CHECK_EQ(s.Int("perf.pl1"), 120LL);                                // the live values from the EC are still there
  CHECK_EQ(r.Set("perf.pl1", "100").code, std::string("needs.service"));
  CHECK_EQ(r.Set("fan.mode", "boost").code, std::string("needs.service"));
  CHECK_EQ(r.Set("batt.mode", "eco").code, std::string("needs.service"));
  CHECK_EQ(r.Set("pref.fnlock", "1").code, std::string("needs.service"));
  CHECK_EQ(r.Set("light.kbd.effect", "wave").code, std::string("needs.service"));
  CHECK_EQ(r.Profile("Balanced").code, std::string("needs.service"));
  CHECK_EQ(r.Cmd("fan.restore").code, std::string("needs.service"));
  // what does not need the service keeps working
  CHECK(r.Set("disp.bright", "40").ok);
  CHECK(r.Set("gpu.ctgp", "125").ok);
  CHECK_NEAR(r.w.plat.CtgpW(), 125, 0.1);
  // the service comes back
  r.Svc().SetServiceUp(true);
  CHECK(WaitFor([&] { return r.w.oemLink->Connected(); }));
  r.Run(2);
  s = r.S();
  CHECK(s.Bool("oem.up"));
  CHECK_EQ(s.Str("cap.perf.pl"), std::string("ok"));
  CHECK(r.Set("fan.mode", "boost").ok);
}

TEST(ctl_link_facts_for_the_service_page_and_no_credentials) {
  Rig r;
  KV s = r.S();
  CHECK_EQ(s.Str("oem.link.endpoint"), std::string("127.0.0.1:13688"));
  CHECK_EQ(s.Int("oem.link.slot"), 18LL);
  CHECK(s.Int("oem.link.sessions") >= 1);
  CHECK(s.Int("oem.link.rx") > 0);
  CHECK(!s.Bool("oem.link.capture"));
  CHECK(r.Cmd("link.capture", [] { KV a; a.Set("on", "1"); return a; }()).ok);
  r.Run(1);
  CHECK(r.S().Bool("oem.link.capture"));
  CHECK(r.Cmd("link.capture").ok);
  r.Run(1);
  CHECK(!r.S().Bool("oem.link.capture"));
  CHECK(r.Cmd("link.reconnect").ok);
  // nothing that identifies the program to the broker is ever published
  const KV all = r.S();
  for (const auto& kv : all.All()) {
    CHECK(kv.second.find("_Pwd") == std::string::npos);
    CHECK(kv.first.find("pass") == std::string::npos);
  }
}

TEST(ctl_the_vendors_services_can_be_started_from_the_service_page) {
  Rig r;
  KV s = r.S();
  CHECK(s.Bool("oem.installed"));
  CHECK(s.Bool("oem.running"));
  // the vendor's services are stopped (another program or the user did that)
  r.w.oemProbe.running = false;
  r.Svc().SetServiceUp(false);
  CHECK(WaitFor([&] { return !r.w.oemLink->Connected(); }));
  r.c.PollOnce(true);
  s = r.S();
  CHECK(s.Bool("oem.installed"));
  CHECK(!s.Bool("oem.running"));
  CHECK(!s.Bool("oem.up"));
  const int before = r.w.oemProbe.started;
  CHECK(r.Cmd("oem.start").ok);
  CHECK_EQ(r.w.oemProbe.started, before + 1);
  r.Svc().SetServiceUp(true);   // the real services would be listening now
  CHECK(WaitFor([&] { return r.w.oemLink->Connected(); }));
  r.c.PollOnce(true);
  s = r.S();
  CHECK(s.Bool("oem.running"));
  CHECK(s.Bool("oem.up"));
  // nothing to start when the vendor's Control Center is not installed
  r.w.oemProbe.installed = false;
  r.w.oemProbe.running = false;
  const Result none = r.Cmd("oem.start");
  CHECK(!none.ok);
  CHECK_EQ(none.code, std::string("failed.hw"));
  // and the kill switch stops it like every other change
  r.w.oemProbe.installed = true;
  CHECK(r.Cfg("cfg.readonly", "1").ok);
  CHECK_EQ(r.Cmd("oem.start").code, std::string("gate.readonly"));
  CHECK_EQ(r.w.oemProbe.started, before + 1);
}

// ------------------------------------------------------------------------------------------------ gating
TEST(ctl_read_only_kill_switch_stops_everything_that_changes_the_machine) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  const int base = static_cast<int>(r.Svc().Received().size());
  CHECK(r.Cfg("cfg.readonly", "1").ok);
  CHECK(r.S().Bool("cfg.readonly"));
  CHECK_EQ(r.Set("perf.pl1", "90").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("perf.tcc.on", "1").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("gpu.ctgp", "125").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("fan.mode", "boost").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("fan.cpu.curve", "35:0,48:30,60:45,72:60,84:80,96:100").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("batt.mode", "eco").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("pref.fnlock", "1").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("light.kbd.effect", "wave").code, std::string("gate.readonly"));
  CHECK_EQ(r.Profile("Balanced").code, std::string("gate.readonly"));
  CHECK_EQ(r.Cmd("fan.restore").code, std::string("gate.readonly"));
  CHECK_EQ(r.Cmd("perf.restore").code, std::string("gate.readonly"));
  CHECK_EQ(r.Cmd("kb.save").code, std::string("gate.readonly"));
  CHECK_EQ(static_cast<int>(r.Svc().Received().size()), base);       // not one message left for the service
  CHECK_EQ(r.w.plat.ecWrites, 0);
  // a program that starts while the switch is on does not change the profile either
  r.Cfg("cfg.app.0.exe", "game.exe");
  r.Cfg("cfg.app.0.profile", "Overboost");
  r.w.power.exes = {"game.exe"};
  r.Run(8);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Customized"));
  CHECK_EQ(static_cast<int>(r.Svc().Received().size()), base);
  // Windows-level settings are not hardware writes
  CHECK(r.Set("disp.bright", "30").ok);
  r.Cfg("cfg.readonly", "0");
  CHECK(r.Set("perf.pl1", "90").ok);
}

TEST(ctl_the_program_writes_the_ec_for_the_gpu_power_only) {
  Rig r;
  CHECK(r.Profile("Customized").ok);
  CHECK(r.Set("perf.pl1", "90").ok);
  CHECK(r.Set("perf.tcc.on", "1").ok);
  CHECK(r.Set("fan.mode", "boost").ok);
  CHECK(r.Set("batt.mode", "eco").ok);
  CHECK(r.Set("pref.fnlock", "1").ok);
  CHECK(r.Set("light.kbd.effect", "wave").ok);
  CHECK_EQ(r.w.plat.ecWrites, 0);
  CHECK(r.w.plat.writeLog.empty());
  CHECK(r.Set("gpu.ctgp", "125").ok);
  CHECK(!r.w.plat.writeLog.empty());
  for (const auto& wl : r.w.plat.writeLog) CHECK(wl.first == reg::kGpuCtrl || wl.first == reg::kGpuCtgpOffset);
  CHECK(r.S().Int("ec.refused") == 0);
}

// ------------------------------------------------------------------------------------------------ GPU cTGP
TEST(ctl_ctgp_flow) {
  Rig r;
  CHECK(r.Set("gpu.ctgp", "125").ok);
  CHECK_NEAR(r.w.plat.CtgpW(), 125, 0.1);
  CHECK(r.Set("gpu.ctgp", "140").ok);
  CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
  CHECK_EQ(r.S().Int("gpu.ctgp"), 140LL);
  Result res = r.Set("gpu.ctgp", "150");
  CHECK_EQ(res.code, std::string("gate.consent"));
  CHECK_EQ(res.data.Str("consent"), std::string("ctgp_over_oem"));
  CHECK(r.S().Str("last.error").empty());                            // a question to the user is not an error
  CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
  res = r.Set("gpu.ctgp", "150", Consent("ctgp_over_oem"));
  CHECK(res.ok);
  CHECK_NEAR(r.w.plat.CtgpW(), 150, 0.1);
  res = r.Set("gpu.ctgp", "135");
  CHECK_EQ(res.data.Str("consent"), std::string("ctgp_unverified"));  // 130/135 W were never measured live
  CHECK(r.Set("gpu.ctgp", "135", Consent("ctgp_unverified")).ok);
  CHECK_NEAR(r.w.plat.CtgpW(), 135, 0.1);
}

TEST(ctl_ctgp_takes_the_shared_apply_lock) {
  Rig r;
  CHECK(r.Set("gpu.ctgp", "140").ok);
  CHECK_EQ(r.w.lock.acquired, 1);
  CHECK_EQ(r.w.lock.released, 1);
  CHECK_EQ(r.w.lock.depth, 0);
  // a consent refusal never reaches the lock
  Result res = r.Set("gpu.ctgp", "150");
  CHECK_EQ(res.code, std::string("gate.consent"));
  CHECK_EQ(r.w.lock.acquired, 1);
  // another program holds the lock: nothing is applied and the answer is "busy"
  r.w.lock.failNext = true;
  const double before = r.w.plat.CtgpW();
  res = r.Set("gpu.ctgp", "125");
  CHECK_EQ(res.code, std::string("busy"));
  CHECK_NEAR(r.w.plat.CtgpW(), before, 0.1);
  CHECK_EQ(r.w.lock.depth, 0);
}

TEST(ctl_ctgp_with_lagging_nvml) {
  Rig r;
  r.w.plat.nvLagSamples = 3;
  Result res = r.Set("gpu.ctgp", "145");
  CHECK(res.code == "ok" || res.code == "gate.consent");
  if (res.code == "gate.consent") res = r.Set("gpu.ctgp", "145", Consent("ctgp_over_oem"));
  CHECK(res.ok);
  CHECK_NEAR(r.w.plat.CtgpW(), 145, 0.1);
}

TEST(ctl_ctgp_reapplied_after_display_cycle) {
  Rig r;
  CHECK(r.Set("gpu.ctgp", "150", Consent("ctgp_over_oem")).ok);
  r.w.plat.DisplayCycle();                    // the firmware drops the offset above 15 (observed on the real unit)
  CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
  r.c.Notify("display_on");
  r.Run(6);
  CHECK_NEAR(r.w.plat.CtgpW(), 150, 0.1);     // Control Center Advanced put it back
  CHECK(r.w.log.Has("re-applying"));
}

TEST(ctl_ctgp_is_applied_again_after_a_profile_switch) {
  Rig r;
  CHECK(r.Set("gpu.ctgp", "150", Consent("ctgp_over_oem")).ok);
  CHECK(r.Profile("Balanced").ok);
  r.w.plat.DisplayCycle();                    // the vendor's profile puts its own value back
  CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
  r.Run(6);
  CHECK_NEAR(r.w.plat.CtgpW(), 150, 0.1);
  CHECK(r.w.log.Has("(profile)"));
  // nothing of the kind happens when no GPU power was chosen
  Rig q;
  CHECK(q.Profile("Balanced").ok);
  q.Run(8);
  CHECK(!q.w.log.Has("re-applying"));
}

TEST(ctl_reapply_after_resume_and_rate_limit) {
  Rig r;
  CHECK(r.Set("gpu.ctgp", "150", Consent("ctgp_over_oem")).ok);
  // a program that keeps resetting the value must not cause an endless fight
  int wrote = 0;
  for (int i = 0; i < 12; i++) {
    r.w.plat.DisplayCycle();
    const int before = r.w.plat.ecWrites;
    r.c.Notify("resume");
    r.Run(5);
    if (r.w.plat.ecWrites != before) wrote++;
  }
  CHECK(wrote >= 1 && wrote <= 6);
  CHECK(r.w.log.Has("re-apply suppressed"));
}

TEST(ctl_keep_off_means_no_reapply) {
  Rig r;
  r.Cfg("cfg.keep", "0");
  CHECK(r.Set("gpu.ctgp", "150", Consent("ctgp_over_oem")).ok);
  r.w.plat.DisplayCycle();
  r.c.Notify("resume");
  r.Run(6);
  CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
}

TEST(ctl_gpu_power_persists_across_restart) {
  std::string dir = TempDir();
  CHECK(!dir.empty());
  {
    Rig r(sim::Model::Exact, dir);
    CHECK(r.Set("gpu.ctgp", "150", Consent("ctgp_over_oem")).ok);
    r.Cfg("cfg.lang", "tr");
  }
  {
    Rig r(sim::Model::Exact, dir);    // a fresh machine: the firmware default (140 W)
    CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
    CHECK_EQ(r.S().Str("cfg.lang"), std::string("tr"));
    r.Run(12);                        // the start-up re-apply fires after ~8 s
    CHECK_NEAR(r.w.plat.CtgpW(), 150, 0.1);
  }
  RemoveDir(dir);
}

TEST(ctl_desired_file_with_garbage_is_ignored) {
  std::string dir = TempDir();
  if (FILE* f = fopen((dir + "/desired.kv").c_str(), "wb")) {
    // out-of-range, unknown and removed keys; nothing the service owns is ever restored from a file
    fputs("gpu.ctgp=999\nbogus.key=1\nperf.pl1=50\nec.pl1=9999\nfan.mode=boost\ngpu.ctgp=abc\n", f);
    fclose(f);
  }
  {
    Rig r(sim::Model::Exact, dir);
    r.Run(12);
    CHECK_NEAR(r.w.plat.CtgpW(), 140, 0.1);
    CHECK_EQ(r.w.plat.ecWrites, 0);
    CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), 0);
    CHECK_EQ(r.Sent("FAN_BOOST_ON"), 0);
  }
  if (FILE* f = fopen((dir + "/desired.kv").c_str(), "wb")) {
    fputs("perf.pl1=50\ngpu.ctgp=125\n", f);
    fclose(f);
  }
  {
    Rig r(sim::Model::Exact, dir);
    r.Run(12);
    CHECK_NEAR(r.w.plat.CtgpW(), 125, 0.1);                         // the valid one was applied
    CHECK_EQ(r.Sent("SET_OPERATING_MODE_DETAIL"), 0);
  }
  RemoveDir(dir);
}

// ------------------------------------------------------------------------------------------------ fans
TEST(ctl_fan_boost_and_custom_curves_through_the_service) {
  Rig r;
  CHECK(r.Set("fan.mode", "boost").ok);
  CHECK(r.Ec(reg::kFanMode) & 0x40);
  CHECK_EQ(r.S().Str("fan.mode"), std::string("boost"));
  CHECK(r.S().Bool("fan.boost"));
  CHECK_EQ(r.Sent("\"Action\":\"FAN_BOOST_ON\""), 1);
  r.Run(30);
  CHECK(r.S().Int("fan.cpu.duty") >= 90);
  // boost -> custom curve: boost goes off first, then the service is told to follow the table
  CHECK(r.Set("fan.mode", "custom").ok);
  CHECK(!(r.Ec(reg::kFanMode) & 0x40));
  CHECK(r.Ec(reg::kFanCurveEnable) & 0x80);
  CHECK(r.Ec(reg::kFanCurveLatch) & 0x04);
  CHECK_EQ(r.S().Str("fan.mode"), std::string("custom"));
  CHECK_EQ(r.Sent("\"Action\":\"FAN_BOOST_OFF\""), 1);
  CHECK_EQ(r.Sent("\"FanControlRespective\":true"), 1);
  CHECK_EQ(r.Sent("\"Name\":\"M1T2\""), 1);                          // the table the service says is active
  // back to the automatic mode
  CHECK(r.Set("fan.mode", "auto").ok);
  CHECK(!(r.Ec(reg::kFanCurveEnable) & 0x80));
  CHECK_EQ(r.S().Str("fan.mode"), std::string("auto"));
  CHECK_EQ(r.Sent("\"FanControlRespective\":false"), 1);
  CHECK_EQ(r.Sent("FAN_BOOST_ON"), 1);                               // never sent again on the way
  // asking for what is already so sends nothing
  const int before = static_cast<int>(r.Svc().Received().size());
  CHECK(r.Set("fan.mode", "auto").ok);
  CHECK_EQ(static_cast<int>(r.Svc().Received().size()), before);
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_a_fan_held_at_a_fixed_speed_by_another_program_is_reported) {
  Rig r;
  r.w.plat.ec[reg::kFanFixedCpu] = 0x80 | 40;                         // some other tool pins the fans
  r.w.plat.ec[reg::kFanFixedGpu] = 0x80 | 40;
  r.Run(2);
  CHECK_EQ(r.S().Str("fan.mode"), std::string("fixed"));
  Result res = r.Set("fan.mode", "auto");
  CHECK_EQ(res.code, std::string("failed.verify"));
  CHECK(res.msg.find("fixed speed") != std::string::npos);
  CHECK_EQ(r.w.plat.ecWrites, 0);                                    // and this program did not touch the fan registers
}

TEST(ctl_fan_curves_go_to_the_table_the_service_uses) {
  Rig r;
  uint8_t gpuBefore[48];
  for (int i = 0; i < 16; i++) {
    gpuBefore[i] = r.Ec(reg::kFanTableGpuUp + i);
    gpuBefore[16 + i] = r.Ec(reg::kFanTableGpuDown + i);
    gpuBefore[32 + i] = r.Ec(reg::kFanTableGpuDuty + i);
  }
  const char* cpu = "35:0,48:30,60:45,72:60,84:80,96:100";
  CHECK(r.Set("fan.cpu.curve", cpu).ok);
  CHECK_EQ(r.Sent("SET_FAN_SPEED_CURVE_SETTING"), 1);
  CHECK_EQ(r.Sent("\"Name\":\"M1T2\""), 1);
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1), static_cast<uint8_t>(48));      // the table the fan follows is the one in the EC
  CHECK_EQ(r.Ec(reg::kFanTableCpuDuty + 1), static_cast<uint8_t>(60));
  CHECK_EQ(r.S().Str("fan.cpu.curve"), std::string(cpu));
  for (int i = 0; i < 16; i++) {                                           // the other fan keeps what it had
    CHECK_EQ(r.Ec(reg::kFanTableGpuUp + i), gpuBefore[i]);
    CHECK_EQ(r.Ec(reg::kFanTableGpuDown + i), gpuBefore[16 + i]);
    CHECK_EQ(r.Ec(reg::kFanTableGpuDuty + i), gpuBefore[32 + i]);
  }
  CHECK_EQ(r.S().Str("fan.mode"), std::string("auto"));                    // storing a curve does not switch the fan mode
  CHECK(r.Set("fan.gpu.curve", "40:0,50:35,60:50,70:65,80:85,90:100").ok);
  CHECK_EQ(r.Ec(reg::kFanTableGpuUp + 2), static_cast<uint8_t>(60));
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1), static_cast<uint8_t>(48));      // ... and the CPU table stayed
  // an unsafe curve never leaves the program
  const int sent = r.Sent("SET_FAN_SPEED_CURVE_SETTING");
  Result bad = r.Set("fan.cpu.curve", "35:0,50:20,100:30");
  CHECK_EQ(bad.code, std::string("range"));
  CHECK_EQ(r.Sent("SET_FAN_SPEED_CURVE_SETTING"), sent);
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1), static_cast<uint8_t>(48));
  // the vendor's default curves
  CHECK(r.Cmd("fan.restore").ok);
  CHECK_EQ(r.Sent("RESTORE_FAN_SPEED_CURVE_SETTING"), 1);
  CHECK(WaitFor([&] { return r.Ec(reg::kFanTableCpuUp + 1) != 48; }));
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_a_fan_table_the_ec_does_not_take_is_reported) {
  Rig r;
  r.Svc().SetEcWritesApply(false);
  Result res = r.Set("fan.cpu.curve", "35:0,48:30,60:45,72:60,84:80,96:100");
  CHECK_EQ(res.code, std::string("failed.verify"));
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1) != 48, true);
}

// ------------------------------------------------------------------------------------------------ battery / switches
TEST(ctl_charge_profile_through_the_service) {
  Rig r;
  CHECK(r.Set("batt.mode", "eco").ok);
  CHECK_EQ(r.Ec(reg::kBatteryMode) & 0x38, 0x28);
  CHECK_EQ(r.S().Str("batt.mode"), std::string("eco"));
  CHECK_EQ(r.Sent("\"Action\":\"HEALTHYMODE\""), 1);
  CHECK(r.Set("batt.mode", "highcap").ok);
  CHECK_EQ(r.Ec(reg::kBatteryMode) & 0x38, 0x08);
  CHECK_EQ(r.Sent("\"Action\":\"PERFORMANCEDMODE\""), 1);
  CHECK(r.Set("batt.mode", "balanced").ok);
  CHECK_EQ(r.Ec(reg::kBatteryMode) & 0x38, 0x18);
  CHECK_EQ(r.S().Str("batt.mode"), std::string("balanced"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_hardware_switches_are_confirmed_by_the_ec_bit) {
  Rig r;
  CHECK(r.Set("pref.fnlock", "1").ok);
  CHECK(r.Ec(reg::kFnLock) & 0x10);
  CHECK(r.S().Bool("pref.fnlock"));
  CHECK_EQ(r.Sent("\"Action\":\"FNKEY_LOCK\""), 1);
  // already so: nothing is sent (with the polarity unverified a repeat could flip it)
  CHECK(r.Set("pref.fnlock", "1").ok);
  CHECK_EQ(r.Sent("FNKEY"), 1);
  CHECK(r.Set("pref.fnlock", "0").ok);
  CHECK(!(r.Ec(reg::kFnLock) & 0x10));
  CHECK_EQ(r.Sent("\"Action\":\"FNKEY_UNLOCK\""), 1);
  CHECK(r.Set("pref.acboot", "1").ok);
  CHECK(r.Ec(reg::kAcAutoBoot) & 0x08);
  CHECK(r.Ec(reg::kAcAutoBoot) & 0x80);                              // the bit next to it was not disturbed
  CHECK_EQ(r.Sent("ACRECOVERY_TOGGLE_ON"), 1);
  CHECK(r.Set("pref.usbpower", "1").ok);
  CHECK(r.Ec(reg::kUsbPowerShare) & 0x10);
  CHECK_EQ(r.Sent("USB_CHARGER_ON"), 1);
  CHECK(r.Set("pref.usbpower", "0").ok);
  CHECK(!(r.Ec(reg::kUsbPowerShare) & 0x10));
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_switch_polarity_is_learned_from_the_ec) {
  Rig r;
  sim::FakeServiceState st = r.Svc().State();
  st.swInverted = true;                                              // on this (imaginary) model LOCK clears the bit
  r.Svc().SetState(st);
  CHECK(r.Set("pref.fnlock", "1").ok);                               // the first guess did nothing, the other action did it
  CHECK(r.Ec(reg::kFnLock) & 0x10);
  CHECK_EQ(r.Sent("\"Action\":\"FNKEY_LOCK\""), 1);
  CHECK_EQ(r.Sent("\"Action\":\"FNKEY_UNLOCK\""), 1);
  CHECK_EQ(r.S().Str("cfg.pol.fnlock"), std::string("1"));           // remembered
  // the next change uses the remembered polarity straight away
  CHECK(r.Set("pref.fnlock", "0").ok);
  CHECK(!(r.Ec(reg::kFnLock) & 0x10));
  CHECK_EQ(r.Sent("\"Action\":\"FNKEY_LOCK\""), 2);
  CHECK_EQ(r.Sent("\"Action\":\"FNKEY_UNLOCK\""), 1);
}

// ------------------------------------------------------------------------------------------------ keyboard lighting
TEST(ctl_keyboard_lighting_through_the_service) {
  Rig r;
  CHECK(r.Set("light.kbd.effect", "wave").ok);
  CHECK_EQ(r.S().Str("light.kbd.effect"), std::string("wave"));
  CHECK_EQ(r.Svc().State().kb.effect, 2);
  CHECK_EQ(r.Sent("\"function\":\"SetEffectALL\""), 1);
  CHECK(r.Set("light.kbd.bright", "4").ok);
  CHECK_EQ(r.Svc().State().kb.light, 4);
  CHECK_EQ(r.Svc().State().kb.effect, 2);                            // the rest of the lighting is sent along unchanged
  CHECK(r.Set("light.kbd.speed", "0").ok);
  CHECK_EQ(r.Svc().State().kb.speed, 0);
  CHECK(r.Set("light.kbd.dir", "left-right").ok);
  CHECK_EQ(r.Svc().State().kb.direction, 0);
  CHECK_EQ(r.S().Str("light.kbd.dir"), std::string("left-right"));
  CHECK(r.Set("light.kbd.color", "#FF8800").ok);                     // one colour for every block
  const sim::FakeServiceState st = r.Svc().State();
  for (int i = 0; i < st.kb.blocks; i++) {
    CHECK_EQ(st.kb.rgb[3 * i], 255);
    CHECK_EQ(st.kb.rgb[3 * i + 1], 0x88);
    CHECK_EQ(st.kb.rgb[3 * i + 2], 0);
  }
  CHECK_EQ(r.S().Str("light.kbd.color"), std::string("#ff8800"));
  CHECK(r.Set("light.kbd.colors", "#ff0000,#00ff00,#0000ff").ok);   // the first blocks only; the others keep their colour
  const sim::FakeServiceState st2 = r.Svc().State();
  CHECK_EQ(st2.kb.rgb[0], 255);
  CHECK_EQ(st2.kb.rgb[4], 255);
  CHECK_EQ(st2.kb.rgb[8], 255);
  CHECK_EQ(st2.kb.rgb[3 * 3 + 1], 0x88);
  CHECK_EQ(r.S().Str("light.kbd.colors"), std::string("#ff0000,#00ff00,#0000ff,#ff8800,#ff8800,#ff8800,#ff8800,#ff8800"));
  CHECK(r.Set("light.kbd.power", "0").ok);
  CHECK(!r.S().Bool("light.kbd.power"));
  CHECK_EQ(r.Sent("\"function\":\"SetPower\""), 1);
  CHECK(r.Set("light.kbd.power", "1").ok);
  CHECK(r.S().Bool("light.kbd.power"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_keyboard_with_a_different_number_of_zones) {
  Rig r(sim::Model::Exact, "", false);
  r.Svc().SetKeyboardBlocks(12);
  r.Link();
  r.w.oemLink->RequestAll();
  CHECK(WaitFor([&] { return r.w.oemLink->State().Str("oem.kb.color.ColorBlocks") == "12"; }));
  r.Run(1);
  CHECK_EQ(r.S().Int("light.kbd.blocks"), 12LL);
  CHECK(r.Set("light.kbd.color", "#112233").ok);
  CHECK_EQ(r.Sent("\"ColorBlocks\":12"), 1);
  const sim::FakeServiceState st = r.Svc().State();
  CHECK_EQ(static_cast<int>(st.kb.rgb.size()), 36);
  for (int i = 0; i < 12; i++) {
    CHECK_EQ(st.kb.rgb[3 * i], 0x11);
    CHECK_EQ(st.kb.rgb[3 * i + 2], 0x33);
  }
  // a value that is not a colour never leaves the program
  const int sent = r.Sent("SetEffectALL");
  CHECK_EQ(r.Set("light.kbd.color", "orange").code, std::string("range"));
  CHECK_EQ(r.Sent("SetEffectALL"), sent);
  // "save to BIOS": the same lighting again, with the flag that keeps it in the keyboard
  CHECK(r.Cmd("kb.save").ok);
  CHECK(WaitFor([&] { return r.Sent("\"nv_save\":true") == 1; }));    // sent without waiting for an answer: the stand-in service handles it on its own thread
}

TEST(ctl_the_keyboard_idle_timer_goes_through_the_service_and_waits_for_its_answer) {
  Rig r;
  CHECK_EQ(r.S().Int("light.kbd.timer"), 30LL);                      // what the service reports (the stand-in starts at 30 s)
  CHECK_EQ(r.S().Str("cap.light.kbd.timer"), std::string("ok"));
  CHECK(r.Set("light.kbd.timer", "300").ok);
  CHECK_EQ(r.Sent("\"Action\":\"SetKeyboardCloseTime\""), 1);
  CHECK_EQ(r.Sent("\"Mins\":5,\"Seconds\":0"), 1);                  // 300 s travel as minutes and seconds
  CHECK_EQ(r.Svc().State().closeMin, 5);
  CHECK_EQ(r.S().Int("light.kbd.timer"), 300LL);
  CHECK(r.Set("light.kbd.timer", "90").ok);                          // minutes and seconds both used
  CHECK_EQ(r.Sent("\"Mins\":1,\"Seconds\":30"), 1);
  CHECK_EQ(r.S().Int("light.kbd.timer"), 90LL);
  CHECK(r.Set("light.kbd.timer", "0").ok);                           // 0 = never
  CHECK_EQ(r.S().Int("light.kbd.timer"), 0LL);
  CHECK_EQ(r.w.plat.ecWrites, 0);                                    // nothing but the service is involved
  // values outside the range never leave the program
  const int sent = r.Sent("SetKeyboardCloseTime");
  CHECK_EQ(r.Set("light.kbd.timer", "-1").code, std::string("range"));
  CHECK_EQ(r.Set("light.kbd.timer", "3601").code, std::string("range"));
  CHECK_EQ(r.Set("light.kbd.timer", "soon").code, std::string("range"));
  CHECK_EQ(r.Sent("SetKeyboardCloseTime"), sent);
  // a value that another program changed shows up without any request
  r.Svc().ExternalKeyboard("timer", 600);
  CHECK(WaitFor([&] { r.c.PollOnce(true); return r.S().Int("light.kbd.timer") == 600LL; }));
  // the kill switch covers it like every other message
  CHECK(r.Cfg("cfg.readonly", "1").ok);
  CHECK_EQ(r.Set("light.kbd.timer", "10").code, std::string("gate.readonly"));
  CHECK_EQ(r.Sent("SetKeyboardCloseTime"), sent);
}

TEST(ctl_a_service_that_does_not_confirm_the_idle_timer_is_reported) {
  Rig r;
  r.Svc().SetSilent(true);
  const Result res = r.Set("light.kbd.timer", "120");
  CHECK_EQ(res.code, std::string("failed.service"));
  CHECK_EQ(r.S().Int("light.kbd.timer"), 30LL);                      // the shown value is still what the service reported
}

// ------------------------------------------------------------------------------------------------ Windows-level settings (unchanged from the vendor's app)
TEST(ctl_display_and_devices) {
  Rig r;
  CHECK(r.Set("disp.bright", "35").ok);
  CHECK_EQ(r.w.display.brightness, 35);
  CHECK(r.Set("disp.refresh", "60").ok);
  CHECK_EQ(r.w.display.refresh, 60);
  CHECK_EQ(r.Set("disp.refresh", "120").code, std::string("failed.hw"));
  CHECK(r.Set("disp.mode", "cinema").ok);
  CHECK_EQ(r.S().Str("disp.mode"), std::string("cinema"));
  CHECK(r.Set("disp.c.kelvin", "5500").ok);
  CHECK_EQ(r.w.display.color.kelvin, 5500);
  CHECK_EQ(r.S().Str("disp.mode"), std::string("custom"));
  r.w.display.failColor = true;
  CHECK_EQ(r.Set("disp.c.gamma", "120").code, std::string("failed.hw"));
  CHECK(r.Set("dev.wifi", "0").ok);
  CHECK_EQ(r.w.devices.Get(Dev::Wifi), 0);
  CHECK_EQ(r.S().Str("dev.wifi"), std::string("0"));
  r.w.devices.stuck = true;
  CHECK_EQ(r.Set("dev.bt", "0").code, std::string("reverted"));
}


TEST(ctl_display_saturation_is_a_colour_setting) {
  Rig r;
  CHECK_EQ(r.S().Str("cap.disp.sat"), std::string("ok"));
  CHECK_EQ(r.S().Int("disp.c.sat"), 100LL);
  CHECK(r.Set("disp.c.sat", "140").ok);
  CHECK_EQ(r.w.display.color.saturation, 140);
  CHECK_EQ(r.S().Int("disp.c.sat"), 140LL);
  CHECK_EQ(r.S().Str("disp.mode"), std::string("custom"));   // a hand-made value leaves the image profiles
  CHECK(r.Set("disp.mode", "vibrant").ok);                    // every profile carries its own saturation
  CHECK_EQ(r.w.display.color.saturation, 135);
  CHECK_EQ(r.S().Int("disp.c.sat"), 135LL);
  CHECK(r.Set("disp.mode", "standard").ok);
  CHECK_EQ(r.w.display.color.saturation, 100);
  CHECK(r.w.display.color.Neutral());
  CHECK_EQ(r.Set("disp.c.sat", "201").code, std::string("range"));
  CHECK_EQ(r.Set("disp.c.sat", "-1").code, std::string("range"));
  CHECK(r.Set("disp.c.sat", "0").ok);                         // fully grey is a valid choice
  CHECK_EQ(r.w.display.color.saturation, 0);
}

TEST(ctl_display_saturation_unavailable_without_the_colour_effect_api) {
  Rig r;
  r.w.display.saturationOk = false;   // Magnification.dll missing / refused
  CHECK(r.Cmd("probe").ok);           // the user pressed "re-probe hardware"
  CHECK_EQ(r.S().Str("cap.disp.sat"), std::string("unsupported"));
  CHECK(!r.S().Str("capwhy.disp.sat").empty());
  const int before = r.w.display.colorApplied;
  CHECK_EQ(r.Set("disp.c.sat", "120").code, std::string("unsupported"));
  CHECK_EQ(r.w.display.colorApplied, before);                 // nothing reached the panel
  CHECK_EQ(r.S().Int("disp.c.sat"), 100LL);
  CHECK(r.Set("disp.c.kelvin", "5000").ok);                   // the rest of the colour controls still work
  CHECK_EQ(r.w.display.color.kelvin, 5000);
  CHECK_EQ(r.S().Str("cap.disp.color"), std::string("ok"));
}

TEST(ctl_display_colours_come_back_after_sleep_and_wake) {
  Rig r;
  CHECK(r.Set("disp.c.kelvin", "5200").ok);
  CHECK(r.Set("disp.c.sat", "130").ok);
  r.w.display.color = ColorSettings();   // Windows drops the ramp and the colour effect across sleep
  int n = r.w.display.colorApplied;
  r.c.Notify("resume");
  r.Run(3);
  CHECK(r.w.display.colorApplied > n);
  CHECK_EQ(r.w.display.color.kelvin, 5200);
  CHECK_EQ(r.w.display.color.saturation, 130);
  r.w.display.color = ColorSettings();   // some drivers need a second look a few seconds later
  n = r.w.display.colorApplied;
  r.Run(6);
  CHECK(r.w.display.colorApplied > n);
  CHECK_EQ(r.w.display.color.kelvin, 5200);
  r.w.display.color = ColorSettings();   // the display waking up gets the same treatment
  r.c.Notify("display_on");
  r.Run(3);
  CHECK_EQ(r.w.display.color.kelvin, 5200);
  CHECK_EQ(r.w.display.color.saturation, 130);
  r.Run(6);
  n = r.w.display.colorApplied;          // and nothing else keeps pushing them
  r.c.Notify("ac_change");
  r.Run(10);
  CHECK_EQ(r.w.display.colorApplied, n);
}

TEST(ctl_display_neutral_colours_are_never_pushed_again) {
  Rig r;
  const int n0 = r.w.display.colorApplied;
  r.c.Notify("resume");
  r.Run(10);
  r.c.Notify("display_on");
  r.Run(10);
  CHECK_EQ(r.w.display.colorApplied, n0);   // an untouched machine keeps whatever night light / f.lux set
  CHECK(r.Set("disp.c.kelvin", "5000").ok);
  CHECK(r.Set("disp.mode", "standard").ok);
  const int n1 = r.w.display.colorApplied;
  r.c.Notify("resume");
  r.Run(10);
  CHECK_EQ(r.w.display.colorApplied, n1);   // the standard profile is neutral again
}

TEST(ctl_display_colours_return_at_start_and_after_a_refresh_change) {
  std::string dir = TempDir();
  CHECK(!dir.empty());
  {
    Rig r(sim::Model::Exact, dir);
    CHECK(r.Set("disp.c.kelvin", "4800").ok);
    CHECK(r.Set("disp.c.sat", "115").ok);
  }
  {
    Rig r(sim::Model::Exact, dir);   // sign-in: Windows starts with a clean ramp
    CHECK(r.w.display.color.Neutral());
    r.Run(4);
    CHECK_EQ(r.w.display.color.kelvin, 4800);
    CHECK_EQ(r.w.display.color.saturation, 115);
    r.Run(8);
    r.w.display.color = ColorSettings();
    CHECK(r.Set("disp.refresh", "60").ok);   // a mode change can drop the ramp as well
    r.Run(3);
    CHECK_EQ(r.w.display.color.kelvin, 4800);
    CHECK_EQ(r.S().Int("disp.refresh"), 60LL);
  }
  RemoveDir(dir);
}

// ------------------------------------------------------------------------------------------------ application profiles
TEST(ctl_application_profile_switching) {
  Rig r;
  r.Cfg("cfg.app.0.exe", "game.exe");
  r.Cfg("cfg.app.0.profile", "Overboost");
  CHECK(r.Profile("Balanced").ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(45));
  r.w.power.exes = {"explorer.exe", "game.exe"};
  r.Run(5);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  r.w.power.exes = {"explorer.exe"};
  r.Run(5);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(45));   // back to the previous profile
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_application_profile_commands) {
  Rig r;
  auto set = [&](const char* slot, const std::string& exe, const std::string& profile, const std::string& name = "") {
    KV a;
    if (*slot) a.Set("i", slot);
    a.Set("exe", exe);
    a.Set("profile", profile);
    a.Set("name", name);
    return r.Cmd("app.set", a);
  };
  auto slotCmd = [&](const char* cmd, const char* slot, const char* dir = nullptr) {
    KV a;
    a.Set("i", slot);
    if (dir) a.Set("dir", dir);
    return r.Cmd(cmd, a);
  };
  // adding appends and normalises the image name (trimmed, lower case, ".exe" added) and the profile name (the vendor's spelling)
  CHECK(set("", "  Game.EXE ", "overboost", "My game").ok);
  CHECK(set("", "blender", "Enthusiast").ok);
  KV s = r.S();
  CHECK_EQ(s.Str("cfg.app.0.exe"), std::string("game.exe"));
  CHECK_EQ(s.Str("cfg.app.0.profile"), std::string("Overboost"));
  CHECK_EQ(s.Str("cfg.app.0.name"), std::string("My game"));
  CHECK_EQ(s.Str("cfg.app.1.exe"), std::string("blender.exe"));
  CHECK_EQ(s.Str("cfg.app.1.name"), std::string(""));
  // refused: a path, a wildcard, an unknown profile, a duplicate, an over-long name, bad or empty slots
  CHECK(!set("", "C:\\Games\\game.exe", "Balanced").ok);
  CHECK(!set("", "ga*me.exe", "Balanced").ok);
  CHECK(!set("", "notexe.dll", "Balanced").ok);
  CHECK(!set("", "x.exe", "No such profile").ok);
  CHECK_EQ(set("", "x.exe", "Quiet").code, std::string("unknown"));   // the older program had more profiles: not here
  CHECK(!set("", "GAME.exe", "Balanced").ok);
  CHECK(!set("", "name.exe", "Balanced", std::string(49, 'x')).ok);
  CHECK(!set("", "", "Balanced").ok);
  CHECK(!set("7", "y.exe", "Balanced").ok);
  CHECK(!set("32", "y.exe", "Balanced").ok);
  CHECK(!set("abc", "y.exe", "Balanced").ok);
  CHECK(!slotCmd("app.delete", "9").ok);
  CHECK(!slotCmd("app.move", "0", "-1").ok);   // already first
  CHECK(!slotCmd("app.move", "1", "1").ok);    // already last
  // editing keeps the program name allowed for its own slot
  CHECK(set("0", "game.exe", "Balanced", "My game").ok);
  CHECK_EQ(r.S().Str("cfg.app.0.profile"), std::string("Balanced"));

  // the first running entry of the list wins; the previous profile comes back when it closes
  CHECK(r.Profile("Customized").ok);
  r.w.power.exes = {"explorer.exe", "blender.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("blender.exe"));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  CHECK_EQ(r.S().Str("app.before"), std::string("Customized"));
  r.w.power.exes = {"blender.exe", "game.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("game.exe"));   // slot 0 outranks slot 1 while both run
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
  CHECK(slotCmd("app.move", "1", "-1").ok);                     // blender moves above the game
  CHECK_EQ(r.S().Str("cfg.app.0.exe"), std::string("blender.exe"));
  CHECK_EQ(r.S().Str("cfg.app.1.exe"), std::string("game.exe"));
  CHECK_EQ(r.S().Str("cfg.app.1.name"), std::string("My game"));
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("blender.exe"));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  // editing the profile of the running match applies it right away, without waiting for the program to restart
  CHECK(set("0", "blender.exe", "Overboost").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  // switching the whole feature off hands control back at once
  CHECK(r.Cfg("cfg.app.auto", "0").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string(""));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Customized"));
  r.w.power.exes = {"explorer.exe"};
  r.Run(5);
  r.w.power.exes = {"game.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Customized"));   // matching is off: nothing changes
  CHECK(r.Cfg("cfg.app.auto", "1").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("game.exe"));
  // deleting the entry that matches ends its profile
  CHECK(slotCmd("app.delete", "1").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string(""));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Customized"));
  CHECK_EQ(r.S().Str("cfg.app.1.exe"), std::string(""));
  CHECK(!slotCmd("app.delete", "1").ok);

  // an entry with a profile this version does not have (a configuration from an older release) is skipped instead of blocking the
  // entries below it
  r.Cfg("cfg.app.1.exe", "night.exe");
  r.Cfg("cfg.app.1.profile", "Night");
  CHECK(set("", "late.exe", "Balanced").ok);
  CHECK_EQ(r.S().Str("cfg.app.2.exe"), std::string("late.exe"));
  r.w.power.exes = {"night.exe", "late.exe", "explorer.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("late.exe"));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
  CHECK_EQ(r.S().Str("cfg.app.1.profile"), std::string("Night"));   // still listed, so the page can flag it
  CHECK_EQ(r.w.plat.ecWrites, 0);
}

TEST(ctl_an_application_profile_the_service_cannot_apply_is_tried_again) {
  Rig r;
  CHECK(r.Profile("Balanced").ok);
  r.Cfg("cfg.app.0.exe", "game.exe");
  r.Cfg("cfg.app.0.profile", "Overboost");
  r.Svc().SetServiceUp(false);
  CHECK(WaitFor([&] { return !r.w.oemLink->Connected(); }));
  r.w.power.exes = {"game.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string(""));              // not applied, so not claimed either
  CHECK(r.w.log.Has("application profile not applied"));
  r.Svc().SetServiceUp(true);
  CHECK(WaitFor([&] { return r.w.oemLink->Connected(); }));
  r.Run(30);
  CHECK_EQ(r.S().Str("app.active"), std::string("game.exe"));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  CHECK_EQ(r.S().Str("app.before"), std::string("Balanced"));
}

TEST(ctl_plugging_the_charger_in_or_out_does_not_change_the_profile) {
  Rig r;                                   // the older program had power-source rules; this one leaves the profile alone
  CHECK(r.Profile("Enthusiast").ok);
  r.Cfg("cfg.prof.auto", "1");             // a leftover from an old configuration file must do nothing
  r.w.plat.ac = false;
  r.Run(8);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  r.w.plat.ac = true;
  r.c.Notify("ac_change");
  r.Run(8);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  CHECK_EQ(r.Sent("OPERATING_"), 1);       // only the one switch above
}

// ------------------------------------------------------------------------------------------------ battery history
namespace {
std::vector<int> BattHist(Rig& r) {
  std::vector<int> v;
  const std::string t = r.S().Str("batt.hist");
  size_t i = 0;
  while (i < t.size()) {
    size_t j = t.find(',', i);
    if (j == std::string::npos) j = t.size();
    v.push_back(atoi(t.substr(i, j - i).c_str()));
    i = j + 1;
  }
  return v;
}
}  // namespace

TEST(ctl_battery_history_samples_once_a_minute) {
  Rig r;
  r.Run(5);
  std::vector<int> h = BattHist(r);
  CHECK_EQ(h.size(), static_cast<size_t>(1));          // the first sample is taken as soon as the charge is known
  CHECK(h[0] >= 200 && h[0] <= 300);                   // the simulated charger is connected: charge + 200
  CHECK_EQ(r.S().Int("batt.hist.step"), 60LL);
  r.Run(5 * 60);
  h = BattHist(r);
  CHECK(h.size() >= 5 && h.size() <= 7);               // about one a minute, whatever the poll cadence
  r.w.plat.ac = false;
  r.Run(3 * 60);
  h = BattHist(r);
  CHECK(h.back() >= 0 && h.back() <= 100);             // unplugged: the plain percentage
  CHECK(h.front() >= 200);
}

TEST(ctl_battery_history_marks_the_minutes_the_machine_slept) {
  Rig r;
  r.Run(5);
  const size_t before = BattHist(r).size();
  r.w.clock.Advance(10 * 60 * 1000);                   // a ten minute sleep: no polls in between
  r.c.PollOnce(false);
  const std::vector<int> h = BattHist(r);
  CHECK_EQ(h.size(), before + 10);                     // nine empty minutes and the new sample
  for (size_t i = before; i + 1 < h.size(); i++) CHECK_EQ(h[i], -1);
  CHECK(h.back() >= 0);
}

TEST(ctl_battery_history_keeps_eight_hours) {
  Rig r;
  r.Run(2);
  r.w.clock.Advance(20ull * 3600 * 1000);              // far longer than the window
  r.c.PollOnce(false);
  const std::vector<int> h = BattHist(r);
  CHECK_EQ(h.size(), static_cast<size_t>(480));
  CHECK_EQ(h[0], -1);
  CHECK(h.back() >= 0);
}

// ------------------------------------------------------------------------------------------------ misc plumbing
TEST(ctl_threaded_mode_executes_and_stops) {
  sim::World w;
  Controller::Options o;
  o.mode = "sim";
  Controller c(w.hw(), o);   // worker thread, virtual clock never advances by itself: only Execute paths are used
  c.Start();
  Request q;
  q.op = "set";
  q.key = "disp.bright";
  q.value = "25";
  Result res = c.Execute(q);
  CHECK(res.ok);
  CHECK_EQ(w.display.brightness, 25);
  q.value = "30";
  uint64_t id = c.Submit(q);
  Result out;
  bool got = false;
  for (int i = 0; i < 200 && !got; i++) {
    got = c.TakeResult(id, &out);
    if (!got) usleep(10000);
  }
  CHECK(got && out.ok);
  CHECK_EQ(w.display.brightness, 30);
  c.Stop();
}

TEST(ctl_threaded_mode_service_requests_wait_for_the_answer) {
  sim::World w;
  Controller::Options o;
  o.mode = "sim";
  Controller c(w.hw(), o);
  c.Start();
  CHECK(WaitFor([&] { return w.oemLink->Connected() && w.oemLink->State().Has("oem.fan.OperatingMode"); }));
  Request q;
  q.op = "cmd";
  q.key = "profile.apply";
  q.args.Set("name", "Balanced");
  Result res = c.Execute(q);
  CHECK(res.ok);
  CHECK_EQ(w.plat.ec[reg::kPl1], static_cast<uint8_t>(45));
  c.Stop();
}

TEST(ctl_log_tail_and_error_state) {
  Rig r;
  r.Set("perf.pl1", "5");
  CHECK(r.S().Str("last.error").find("range") != std::string::npos);
  r.Set("gpu.ctgp", "125");
  CHECK(r.S().Str("log.tail").find("set gpu.ctgp=125 ok") != std::string::npos);
  CHECK(r.Profile("Customized").ok);
  r.Set("perf.pl1", "90");
  CHECK(r.S().Str("log.tail").find("set perf.pl1=90 ok") != std::string::npos);
}

// ------------------------------------------------------------------------------------------------ Device info: CPU counts, memory modules, drives
TEST(ctl_publishes_cpu_counts_and_memory_modules) {
  Rig r;
  const KV s = r.S();
  CHECK_EQ(s.Int("host.cores"), 8LL);
  CHECK_EQ(s.Int("host.threads"), 16LL);
  CHECK_EQ(s.Int("host.cpu.mhz"), 2304LL);
  CHECK_EQ(s.Int("host.mem.n"), 2LL);                    // the empty slot is not counted
  CHECK_EQ(s.Int("host.mem.0.mb"), 16384LL);
  CHECK_EQ(s.Str("host.mem.0.type"), std::string("DDR4"));
  CHECK_EQ(s.Int("host.mem.0.rated"), 3200LL);
  CHECK_EQ(s.Int("host.mem.0.run"), 3200LL);
  CHECK_EQ(s.Str("host.mem.0.slot"), std::string("ChannelA-DIMM0"));
  CHECK_EQ(s.Str("host.mem.1.maker"), std::string("Samsung"));   // the JEDEC id "80CE" is resolved
  CHECK_EQ(s.Str("host.mem.1.part"), std::string("M471A2G43AB2-CWE"));
  CHECK(!s.Has("host.mem.2.mb"));
  // serial numbers and asset tags from the table never reach the snapshot
  for (const auto& kv : s.All()) {
    CHECK(kv.second.find("SERIAL-FAKE") == std::string::npos);
    CHECK(kv.second.find("Asset-FAKE") == std::string::npos);
  }
}

TEST(ctl_publishes_drives_and_refreshes_only_while_visible) {
  Rig r;
  KV s = r.S();
  CHECK_EQ(s.Int("storage.n"), 2LL);
  CHECK_EQ(s.Str("storage.0.model"), std::string("WD Green SN3000 2TB"));
  CHECK_EQ(s.Str("storage.0.bus"), std::string("NVMe"));
  CHECK(s.Bool("storage.0.ssd"));
  CHECK_EQ(s.Str("storage.0.size"), std::string("2000398934016"));
  CHECK_EQ(s.Int("storage.0.temp"), 33LL);
  CHECK_EQ(s.Int("storage.0.cycles"), 927LL);
  CHECK_EQ(s.Int("storage.0.hours"), 116LL);             // 4 days 20 hours
  CHECK_EQ(s.Int("storage.0.wear"), 1LL);
  CHECK_EQ(s.Int("storage.0.warn"), 0LL);
  CHECK_EQ(s.Int("storage.0.written"), 3870LL);
  CHECK_EQ(s.Str("storage.1.model"), std::string("SAMSUNG MZVL21T0HCLR-00B00"));
  CHECK_EQ(s.Int("storage.1.cycles"), 3109LL);
  CHECK_EQ(s.Int("storage.1.hours"), 559LL * 24 + 23);

  // visible: refreshed every few seconds, not on every poll
  const int reads0 = r.w.storage.healthReads;
  r.w.storage.drives[0].tempC = 47;
  r.Run(3);
  CHECK_EQ(r.S().Int("storage.0.temp"), 33LL);
  CHECK_EQ(r.w.storage.healthReads, reads0);
  r.Run(7);
  CHECK_EQ(r.S().Int("storage.0.temp"), 47LL);
  CHECK(r.w.storage.healthReads > reads0);

  // hidden window: the drives are left alone, however long it takes
  r.c.SetInterest(false);
  r.w.storage.drives[0].tempC = 52;
  const int reads1 = r.w.storage.healthReads;
  r.Run(120);
  CHECK_EQ(r.w.storage.healthReads, reads1);
  CHECK_EQ(r.S().Int("storage.0.temp"), 47LL);
  r.c.SetInterest(true);
  r.Run(9);
  CHECK_EQ(r.S().Int("storage.0.temp"), 52LL);
}

TEST(ctl_drive_health_denied_or_failing_never_blanks_the_inventory) {
  Rig r;
  // rights missing: the inventory stays, the health values disappear (no stale numbers)
  r.w.storage.healthDenied = true;
  r.Run(10);
  KV s = r.S();
  CHECK_EQ(s.Int("storage.n"), 2LL);
  CHECK_EQ(s.Str("storage.0.model"), std::string("WD Green SN3000 2TB"));
  CHECK(!s.Has("storage.0.temp"));
  CHECK(!s.Has("storage.0.cycles"));
  CHECK(!s.Has("storage.0.hours"));
  CHECK(!s.Has("storage.0.wear"));
  // rights back
  r.w.storage.healthDenied = false;
  r.Run(10);
  CHECK_EQ(r.S().Int("storage.0.temp"), 33LL);
  // a failing read keeps the last good values instead of emptying the page
  r.w.storage.failRead = true;
  r.Run(20);
  CHECK_EQ(r.S().Int("storage.n"), 2LL);
  CHECK_EQ(r.S().Int("storage.1.temp"), 35LL);
}

TEST(ctl_first_drive_read_failing_publishes_nothing_and_is_not_hammered) {
  sim::World w;
  w.storage.failRead = true;
  Controller c(w.hw(), Opts(""));
  c.Start();
  CHECK(!c.Snapshot().Has("storage.n"));                 // the page shows "not available", not "0 drives"
  const int reads = w.storage.reads;
  CHECK(reads >= 1);
  for (int i = 0; i < 3; i++) { w.clock.Advance(1000); c.PollOnce(false); }
  CHECK_EQ(w.storage.reads, reads);                      // retried every few seconds, not on every poll
  w.storage.failRead = false;
  for (int i = 0; i < 9; i++) { w.clock.Advance(1000); c.PollOnce(false); }
  CHECK_EQ(c.Snapshot().Int("storage.n"), 2LL);
}
