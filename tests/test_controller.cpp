// Controller flows against the simulated machine: probing, gating, verified writes, rollback, live values, profiles, persistence.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/common/controller.h"
#include "../src/common/registers.h"
#include "../src/sim/sim.h"
#include "mini_test.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace bc;

namespace {

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
  explicit Rig(sim::Model m = sim::Model::Exact, const std::string& dir = "") : w(m), c(w.hw(), Opts(dir)) { c.Start(); }

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
  KV S() { return c.Snapshot(); }
  uint8_t Ec(uint16_t a) { return w.plat.ec[a]; }
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

}  // namespace

// ------------------------------------------------------------------------------------------------ probing / classes
TEST(ctl_probe_exact_device) {
  Rig r;
  KV s = r.S();
  CHECK_EQ(s.Str("dev.class"), std::string("verified"));
  CHECK_EQ(s.Str("ec.link"), std::string("connected"));
  for (const char* cap : {"ec", "pl.ec", "tcc", "fan.read", "fan.ctl", "fan.curve", "batt.mode", "fnlock", "msr", "msr.pkg", "msr.icc",
                          "msr.volt", "msr.pf", "gpu.nvml", "gpu.ctgp", "os.cpu", "disp.bright", "light.kbd"})
    CHECK_EQ(s.Str(std::string("cap.") + cap), std::string("ok"));
  CHECK_EQ(s.Str("cap.light.bar"), std::string("absent"));
  CHECK_EQ(s.Str("cap.gpu.db"), std::string("unsupported"));
  CHECK_EQ(s.Int("dev.project"), 26LL);
  CHECK_EQ(s.Int("ec.pl1"), 120LL);
  CHECK_EQ(s.Int("ec.pl2"), 120LL);
  CHECK_EQ(s.Int("ec.pl4"), 200LL);
  CHECK_EQ(s.Int("ec.tcc"), 100LL);
  CHECK_EQ(s.Int("gpu.ctgp"), 140LL);
  CHECK_EQ(s.Str("fan.mode"), std::string("auto"));
  CHECK_EQ(s.Str("batt.mode"), std::string("balanced"));
  CHECK(s.Int("cpu.temp") > 20);
  CHECK(s.Num("cpu.pkg.w") >= 0);
  CHECK_EQ(s.Str("oem.services"), std::string("gcuService,GcuBridge"));
  CHECK_EQ(s.Int("ec.writes"), 0LL);  // probing never writes
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
  CHECK_EQ(fam.S().Str("cap.gpu.ctgp"), std::string("off"));  // MGP engine only trusts the exact unit
  Rig unk(sim::Model::Unknown);
  CHECK_EQ(unk.S().Str("dev.class"), std::string("unknown"));
}

TEST(ctl_absent_hardware_degrades) {
  sim::World w;
  w.ec.state = mgp::DriverState::Absent;
  w.plat.msrAvailable = false;
  w.plat.nvmlAvailable = false;
  Controller c(w.hw(), Opts());
  c.Start();
  KV s = c.Snapshot();
  CHECK_EQ(s.Str("cap.ec"), std::string("absent"));
  CHECK_EQ(s.Str("cap.pl.ec"), std::string("absent"));
  CHECK_EQ(s.Str("cap.msr"), std::string("absent"));
  CHECK_EQ(s.Str("cap.gpu.nvml"), std::string("absent"));
  CHECK_EQ(s.Str("ec.link"), std::string("absent"));
  // Windows-level features keep working
  CHECK_EQ(s.Str("cap.os.cpu"), std::string("ok"));
  Request q; q.op = "set"; q.key = "ec.pl1"; q.value = "100";
  CHECK(!c.ExecuteSync(q).ok);
  q.key = "os.epp"; q.value = "20";
  CHECK(c.ExecuteSync(q).ok);
}

// ------------------------------------------------------------------------------------------------ request validation
TEST(ctl_bad_requests) {
  Rig r;
  CHECK_EQ(r.Set("does.not.exist", "1").code, std::string("unknown"));
  CHECK_EQ(r.Set("ec.pl1", "banana").code, std::string("range"));
  CHECK_EQ(r.Set("ec.pl1", "5").code, std::string("range"));
  CHECK_EQ(r.Set("ec.pl1", "900").code, std::string("range"));
  CHECK_EQ(r.Set("gpu.ctgp", "141").code, std::string("range"));
  CHECK_EQ(r.Cfg("notcfg.key", "1").code, std::string("range"));
  CHECK_EQ(r.Cfg("cfg.bad key!", "1").code, std::string("range"));
  Request q; q.op = "frobnicate";
  CHECK_EQ(r.c.ExecuteSync(q).code, std::string("unknown"));
  CHECK_EQ(r.Cmd("nope").code, std::string("unknown"));
  CHECK_EQ(r.w.plat.ecWrites, 0);  // nothing reached the EC
}

// ------------------------------------------------------------------------------------------------ CPU power through the EC
TEST(ctl_set_pl_within_oem_range) {
  Rig r;
  Result res = r.Set("ec.pl1", "100");
  CHECK(res.ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(100));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(120));
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(200));
  CHECK_EQ(r.S().Int("ec.pl1"), 100LL);
  CHECK_EQ(r.S().Int("msr.pl1"), 100LL);        // the CPU followed
  CHECK(res.data.Bool("cpu.applied"));
  CHECK_EQ(r.Ec(reg::kCpuTempLimitRead) & 0x7F, 5);  // the TCC offset is re-asserted unchanged
}

TEST(ctl_pl_ordering_enforced) {
  Rig r;
  CHECK_EQ(r.Set("ec.pl1", "130").code, std::string("range"));          // PL1 above the current PL2
  CHECK_EQ(r.Set("ec.pl2", "130").code, std::string("gate.consent"));   // above the OEM max needs consent
  r.Set("ec.pl1", "100");
  CHECK_EQ(r.Set("ec.pl2", "90").code, std::string("range"));          // PL2 < PL1
  CHECK_EQ(r.Set("ec.pl4", "80").code, std::string("range"));          // PL4 < PL2
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(120));
}

TEST(ctl_pl_above_oem_needs_consent) {
  Rig r;
  Result res = r.Set("ec.pl2", "150");
  CHECK(!res.ok);
  CHECK_EQ(res.code, std::string("gate.consent"));
  CHECK_EQ(res.data.Str("consent"), std::string("ec_pl_high"));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(120));
  res = r.Set("ec.pl2", "150", Consent("ec_pl_high"));
  CHECK(res.ok);
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(150));
  CHECK_EQ(r.S().Str("cfg.consent.ec_pl_high"), std::string("1"));
  CHECK(r.Cmd("consent.reset").ok);
  CHECK_EQ(r.S().Str("cfg.consent.ec_pl_high", "0"), std::string("0"));
  CHECK_EQ(r.Set("ec.pl2", "160").code, std::string("gate.consent"));   // consent is gone again
}

TEST(ctl_pl_write_failure_rolls_back) {
  Rig r;
  r.w.plat.ecFailWriteNumber = r.w.plat.ecWrites + 2;  // the PL2 write fails after PL1 was changed
  Result res = r.Set("ec.pl1", "100");
  CHECK(!res.ok);
  CHECK_EQ(res.code, std::string("reverted"));
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));  // PL1 restored
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(120));
}

TEST(ctl_pl_ignored_by_ec_is_reported) {
  Rig r;
  r.w.plat.ecDropWrites = true;
  Result res = r.Set("ec.pl1", "100");
  CHECK(!res.ok);
  CHECK_EQ(res.code, std::string("reverted"));
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  CHECK_EQ(r.S().Int("ec.pl1"), 120LL);
}

TEST(ctl_pl_msr_not_following_is_diagnosed) {
  Rig r;
  r.w.plat.msrFollowsEc = false;
  Result res = r.Set("ec.pl1", "100");
  CHECK(res.ok);                               // the EC accepted it...
  CHECK(!res.data.Bool("cpu.applied", true));  // ...but the package limit did not follow, and we say so
  CHECK_EQ(r.S().Int("msr.pl1"), 120LL);
  CHECK(r.w.log.Has("does not follow"));
}

TEST(ctl_ec_cold_needs_activation) {
  Rig r;
  r.w.plat.MakeEcCold();
  Result res = r.Set("ec.pl1", "100");
  CHECK_EQ(res.code, std::string("needs.activation"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
  // taking over while the OEM service runs is refused
  r.Cfg("cfg.takeover", "1");
  res = r.Set("ec.pl1", "100");
  CHECK_EQ(res.code, std::string("gate.oem"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
  // stopping the OEM service first (reversible), then the bring-up works
  r.Cfg("cfg.takeover", "0");
  CHECK(r.Cmd("oem.takeover").ok);
  CHECK(!r.w.oem.Probe().running);
  CHECK_EQ(r.S().Str("cfg.takeover"), std::string("1"));
  res = r.Set("ec.pl1", "100");
  CHECK(res.ok);
  CHECK(r.w.plat.EcActive());
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(100));
  CHECK(r.Cmd("oem.restore").ok);
  CHECK(r.w.oem.Probe().running);
}

TEST(ctl_activation_failure_is_reported) {
  Rig r;
  r.w.plat.MakeEcCold();
  r.Cfg("cfg.takeover", "1");
  r.w.oem.running = false;
  r.w.plat.ecDropWrites = true;  // the EC ignores the activation byte
  Result res = r.Set("ec.pl1", "100");
  CHECK(!res.ok);
  CHECK(res.code == "failed.verify" || res.code == "failed.hw");
}

TEST(ctl_tcc_target) {
  Rig r;
  Result res = r.Set("ec.tcc", "90");
  CHECK(res.ok);
  CHECK_EQ(r.Ec(reg::kCpuTempLimitRead) & 0x7F, 15);
  CHECK_EQ(r.S().Int("ec.tcc"), 90LL);
  CHECK_EQ(r.Set("ec.tcc", "69").code, std::string("range"));
}

TEST(ctl_thermal_gate_blocks_raising_only) {
  Rig r;
  r.w.plat.SetLoad(1, 0);
  r.w.plat.cpuT = 96;
  r.w.clock.Advance(100);
  r.c.PollOnce(true);
  CHECK(r.S().Int("cpu.temp.ec") >= 92);
  Result res = r.Set("ec.pl2", "130", Consent("ec_pl_high"));
  CHECK_EQ(res.code, std::string("gate.thermal"));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(120));
  // lowering is always allowed
  CHECK(r.Set("ec.pl1", "90").ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(90));
}

// ------------------------------------------------------------------------------------------------ live values (sliders follow the hardware)
TEST(ctl_snapshot_follows_external_changes) {
  Rig r;
  CHECK_EQ(r.S().Int("ec.pl1"), 120LL);
  // another program (or the OEM service) changes the limits behind our back
  r.w.plat.ec[reg::kPl1] = 77;
  r.w.plat.ec[reg::kPl2] = 99;
  r.w.plat.SetEcProfile(true, 25);
  r.Run(2);
  KV s = r.S();
  CHECK_EQ(s.Int("ec.pl1"), 77LL);
  CHECK_EQ(s.Int("ec.pl2"), 99LL);
  CHECK_EQ(s.Int("gpu.ctgp"), 150LL);
  // and the MSR view follows what the firmware does
  r.w.plat.msr[0x610] = r.w.plat.msr[0x610];
  r.Run(2);
  CHECK(r.S().Has("msr.pl1"));
}

// ------------------------------------------------------------------------------------------------ gating
TEST(ctl_family_model_needs_experimental_toggle) {
  Rig r(sim::Model::Family);
  Result res = r.Set("ec.pl1", "100");
  CHECK_EQ(res.code, std::string("gate.experimental"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
  r.Cfg("cfg.experimental", "1");
  CHECK(r.Set("ec.pl1", "100").ok);
}

TEST(ctl_unknown_model_blocks_hardware_writes) {
  Rig r(sim::Model::Unknown);
  r.Cfg("cfg.experimental", "1");
  Result res = r.Set("ec.pl1", "100");
  CHECK_EQ(res.code, std::string("gate.model"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
  // Windows-only settings are not hardware writes
  CHECK(r.Set("os.epp", "10").ok);
  CHECK(r.Set("disp.bright", "30").ok);
  r.Cfg("cfg.unknownModel", "1");
  CHECK(r.Set("ec.pl1", "100").code != "gate.model");
}

TEST(ctl_read_only_kill_switch) {
  Rig r;
  CHECK(r.Cfg("cfg.readonly", "1").ok);
  CHECK_EQ(r.Set("ec.pl1", "100").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("gpu.ctgp", "125").code, std::string("gate.readonly"));
  CHECK_EQ(r.Set("fan.mode", "boost").code, std::string("gate.readonly"));
  CHECK_EQ(r.w.plat.ecWrites, 0);
  CHECK(r.Set("os.max", "90").ok);  // Windows policy is not a hardware write
  r.Cfg("cfg.readonly", "0");
  CHECK(r.Set("ec.pl1", "100").ok);
}

TEST(ctl_experimental_features_are_gated) {
  Rig r;
  CHECK_EQ(r.Set("msr.pl1", "90").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("msr.icc", "150").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("msr.volt.core", "-50").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("nv.offset.core", "50").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("fan.fixed.cpu", "50").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("fan.ramp", "5").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("fan.mode", "fixed").code, std::string("gate.experimental"));
  CHECK_EQ(r.Set("gpu.db", "1").code, std::string("unsupported"));      // capability missing on this model
  r.Cfg("cfg.experimental", "1");
  CHECK_EQ(r.Set("gpu.db", "1").code, std::string("unsupported"));      // ...and the toggle does not change that
  CHECK_EQ(r.w.plat.ecWrites, 0);
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
  CHECK_NEAR(r.w.plat.CtgpW(), 150, 0.1);     // BaseControl put it back
  CHECK(r.w.log.Has("re-applying"));
}

// ------------------------------------------------------------------------------------------------ fans
TEST(ctl_fan_modes) {
  Rig r;
  CHECK(r.Set("fan.mode", "boost").ok);
  CHECK(r.Ec(reg::kFanMode) & 0x40);
  CHECK_EQ(r.S().Str("fan.mode"), std::string("boost"));
  r.Run(30);
  CHECK(r.S().Int("fan.cpu.duty") >= 90);
  CHECK(r.Set("fan.mode", "auto").ok);
  CHECK(!(r.Ec(reg::kFanMode) & 0x40));
  CHECK_EQ(r.S().Str("fan.mode"), std::string("auto"));
}

TEST(ctl_fan_custom_curve) {
  Rig r;
  CHECK(r.Set("fan.cpu.curve", "35:0,48:30,60:45,72:60,84:80,96:100").ok);  // saved only (mode is auto)
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1), static_cast<uint8_t>(50));        // factory table untouched
  CHECK(r.Set("fan.mode", "custom").ok);
  CHECK(r.Ec(reg::kFanCurveEnable) & 0x80);
  CHECK(r.Ec(reg::kFanCurveLatch) & 0x04);
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1), static_cast<uint8_t>(48));
  CHECK_EQ(r.Ec(reg::kFanTableCpuDuty + 1), static_cast<uint8_t>(60));
  CHECK_EQ(r.S().Str("fan.mode"), std::string("custom"));
  CHECK_EQ(r.S().Str("fan.cpu.curve"), std::string("35:0,48:30,60:45,72:60,84:80,96:100"));
  // editing in custom mode writes straight away
  CHECK(r.Set("fan.gpu.curve", "40:0,50:35,60:50,70:65,80:85,90:100").ok);
  CHECK_EQ(r.Ec(reg::kFanTableGpuUp + 2), static_cast<uint8_t>(60));
  // an unsafe curve never reaches the EC
  Result bad = r.Set("fan.cpu.curve", "35:0,50:20,100:30");
  CHECK_EQ(bad.code, std::string("range"));
  CHECK_EQ(r.Ec(reg::kFanTableCpuUp + 1), static_cast<uint8_t>(48));
}

TEST(ctl_fan_table_readback_failure_restores) {
  Rig r;
  r.Set("fan.cpu.curve", "35:0,48:30,60:45,72:60,84:80,96:100");
  uint8_t before[48];
  for (int i = 0; i < 48; i++) before[i] = r.Ec(reg::kFanTableCpuUp + i);
  r.w.plat.ecDropWrites = true;
  Result res = r.Set("fan.mode", "custom");
  CHECK(!res.ok);
  for (int i = 0; i < 48; i++) CHECK_EQ(r.Ec(reg::kFanTableCpuUp + i), before[i]);
}

TEST(ctl_fixed_fan_and_watchdog) {
  Rig r;
  r.Cfg("cfg.experimental", "1");
  CHECK_EQ(r.Set("fan.mode", "fixed").code, std::string("gate.consent"));
  CHECK(r.Set("fan.fixed.cpu", "20").ok);
  CHECK(r.Set("fan.fixed.gpu", "20").ok);
  CHECK(r.Set("fan.mode", "fixed", Consent("fan_fixed")).ok);
  CHECK_EQ(r.Ec(reg::kFanFixedCpu), static_cast<uint8_t>(0x80 | 20));
  r.w.plat.SetLoad(1, 1);
  r.Run(240);
  // the temperature climbed with a 20 % fan, the watchdog must have forced boost
  CHECK_EQ(r.S().Str("fan.guard"), std::string("tripped"));
  CHECK(r.Ec(reg::kFanMode) & 0x40);
  CHECK_EQ(r.Ec(reg::kFanFixedCpu), static_cast<uint8_t>(0));
  CHECK_EQ(r.S().Str("fan.mode"), std::string("boost"));
  CHECK(r.w.log.Has("fan watchdog"));
  CHECK(r.Set("fan.mode", "auto").ok);
  CHECK_EQ(r.S().Str("fan.guard"), std::string("ok"));
}

// The watchdog must not get slower when the window is hidden (the full EC poll then runs every 5 s): while a custom or fixed fan
// setting is active the temperatures it judges are read every second.
namespace {
int SecondsFromHotToTripped(bool visible) {
  Rig r;
  r.Cfg("cfg.experimental", "1");
  r.Set("fan.fixed.cpu", "20");
  r.Set("fan.fixed.gpu", "20");
  r.Set("fan.mode", "fixed", Consent("fan_fixed"));
  r.c.SetInterest(visible);
  r.w.plat.SetLoad(1, 1);
  int hotAt = -1, trippedAt = -1;
  for (int t = 0; t < 900 && trippedAt < 0; t++) {
    r.Run(1);
    if (hotAt < 0 && (r.w.plat.cpuT >= 97 || r.w.plat.gpuT >= 92)) hotAt = t;
    if (r.S().Str("fan.guard") == "tripped") trippedAt = t;
  }
  if (hotAt < 0 || trippedAt < 0) return -1;
  return trippedAt - hotAt;
}

int EcReadsOver20Seconds(const char* fanMode) {
  Rig r;
  r.Cfg("cfg.experimental", "1");
  if (std::string(fanMode) != "auto") {
    r.Set("fan.fixed.cpu", "40");
    r.Set("fan.fixed.gpu", "40");
    r.Set("fan.mode", fanMode, Consent("fan_fixed"));
  }
  r.c.SetInterest(false);
  r.Run(10);
  const int before = r.w.plat.ecReads;
  r.Run(20);
  return r.w.plat.ecReads - before;
}
}  // namespace

TEST(ctl_fan_watchdog_is_as_quick_with_a_hidden_window) {
  const int shown = SecondsFromHotToTripped(true);
  const int hidden = SecondsFromHotToTripped(false);
  CHECK(shown >= 0);
  CHECK(hidden >= 0);
  CHECK(shown <= 4);
  CHECK(hidden <= 4);
}

TEST(ctl_hidden_window_polls_two_extra_registers_only_while_a_fan_setting_is_active) {
  const int autoReads = EcReadsOver20Seconds("auto");
  const int fixedReads = EcReadsOver20Seconds("fixed");
  CHECK(fixedReads > autoReads);              // the watchdog's temperature reads are there ...
  CHECK(fixedReads - autoReads <= 2 * 20);    // ... and they are the two registers once a second, nothing more
}

TEST(ctl_stop_returns_fixed_fans_to_auto) {
  Rig r;
  r.Cfg("cfg.experimental", "1");
  CHECK(r.Set("fan.fixed.cpu", "40").ok);
  CHECK(r.Set("fan.fixed.gpu", "40").ok);
  CHECK(r.Set("fan.mode", "fixed", Consent("fan_fixed")).ok);
  CHECK_EQ(r.Ec(reg::kFanFixedCpu), static_cast<uint8_t>(0x80 | 40));
  r.c.Stop();  // no watchdog from here on: the fans must not stay at a fixed duty
  CHECK_EQ(r.Ec(reg::kFanFixedCpu) & 0x80, 0);
  CHECK_EQ(r.Ec(reg::kFanFixedGpu) & 0x80, 0);
  CHECK(r.w.log.Has("returning the fans"));
}

// ------------------------------------------------------------------------------------------------ battery / preferences
TEST(ctl_battery_and_preferences) {
  Rig r;
  CHECK(r.Set("batt.mode", "eco").ok);
  CHECK_EQ(r.Ec(reg::kBatteryMode) & 0x38, 0x28);
  CHECK_EQ(r.S().Str("batt.mode"), std::string("eco"));
  CHECK(r.Set("pref.fnlock", "1").ok);
  CHECK(r.Ec(reg::kFnLock) & 0x10);
  CHECK(r.Set("pref.acboot", "1").ok);
  CHECK(r.Ec(reg::kAcAutoBoot) & 0x08);
  CHECK(r.Ec(reg::kAcAutoBoot) & 0x80);  // the PECI bit next to it was not disturbed
  // "powered USB" and "boot on AC" are mutually exclusive
  CHECK(r.Set("pref.usbpower", "1").ok);
  CHECK(r.Ec(reg::kUsbPowerShare) & 0x10);
  CHECK(!(r.Ec(reg::kAcAutoBoot) & 0x08));
  CHECK(r.Set("pref.acboot", "1").ok);
  CHECK(!(r.Ec(reg::kUsbPowerShare) & 0x10));
  CHECK(r.Ec(reg::kAcAutoBoot) & 0x08);
}

// ------------------------------------------------------------------------------------------------ MSR (advanced CPU)
TEST(ctl_msr_gating_and_flow) {
  Rig r;
  CHECK_EQ(r.Set("msr.pl1", "90").code, std::string("gate.experimental"));
  r.Cfg("cfg.experimental", "1");
  Result res = r.Set("msr.pl1", "90");
  CHECK_EQ(res.code, std::string("gate.consent"));
  CHECK_EQ(res.data.Str("consent"), std::string("msr_write"));
  CHECK(r.Set("msr.pl1", "90", Consent("msr_write")).ok);
  CHECK_EQ(r.S().Int("msr.pl1"), 90LL);
  CHECK_EQ(r.S().Int("msr.pl2"), 120LL);
  CHECK(r.Set("msr.tau1", "56").ok);
  CHECK_NEAR(r.S().Num("msr.tau1"), 56, 3);
  CHECK(r.Set("msr.pl2", "140").ok);
  CHECK_EQ(r.Set("msr.pl1", "200").code, std::string("range"));  // PL1 above PL2
  CHECK(r.Set("msr.icc", "150").ok);
  CHECK_EQ(r.S().Int("msr.icc"), 150LL);
  CHECK(r.Set("msr.pf.l2", "0").ok);
  CHECK_EQ(r.S().Str("msr.pf.l2"), std::string("0"));
}

TEST(ctl_msr_firmware_owned_value_reverts) {
  Rig r;
  r.Cfg("cfg.experimental", "1");
  r.w.plat.msrFirmwareOwns = true;
  Result res = r.Set("msr.pl1", "90", Consent("msr_write"));
  CHECK_EQ(res.code, std::string("reverted"));
  CHECK_EQ(r.S().Int("msr.pl1"), 120LL);
}

TEST(ctl_msr_locked_and_closed_mailbox) {
  Rig r;
  r.w.plat.msrPkgLocked = true;
  r.w.plat.ocMailboxOpen = false;
  r.c.ExecuteSync([] { Request q; q.op = "cmd"; q.key = "probe"; return q; }());
  r.Cfg("cfg.experimental", "1");
  CHECK_EQ(r.S().Str("cap.msr.pkg"), std::string("locked"));
  CHECK_EQ(r.S().Str("cap.msr.volt"), std::string("locked"));
  CHECK_EQ(r.Set("msr.pl1", "90", Consent("msr_write")).code, std::string("unsupported"));
  CHECK_EQ(r.Set("msr.volt.core", "-50", Consent("msr_write")).code, std::string("unsupported"));
}

TEST(ctl_msr_undervolt_roundtrip) {
  Rig r;
  r.Cfg("cfg.experimental", "1");
  CHECK(r.Set("msr.volt.core", "-70", Consent("msr_write")).ok);
  CHECK(std::abs(static_cast<int>(r.S().Int("msr.volt.core")) + 70) <= 1);
  CHECK_EQ(r.Set("msr.volt.core", "20").code, std::string("range"));
}

TEST(ctl_msr_platform_limit_and_bd_prochot) {
  Rig r;
  CHECK_EQ(r.S().Str("cap.msr.psys"), std::string("ok"));
  CHECK_EQ(r.S().Int("msr.psys.pl1"), 210LL);
  CHECK_EQ(r.S().Int("msr.psys.pl2"), 250LL);
  CHECK_EQ(r.S().Str("msr.bdp"), std::string("1"));
  CHECK_EQ(r.Set("msr.psys.pl1", "230").code, std::string("gate.experimental"));
  r.Cfg("cfg.experimental", "1");
  CHECK_EQ(r.Set("msr.psys.pl1", "230").code, std::string("gate.consent"));
  CHECK(r.Set("msr.psys.pl1", "230", Consent("msr_write")).ok);
  CHECK_EQ(r.S().Int("msr.psys.pl1"), 230LL);
  CHECK_EQ(r.S().Int("msr.psys.pl2"), 250LL);          // the neighbour is untouched
  CHECK_EQ(r.Set("msr.psys.pl1", "300").code, std::string("range"));   // PL1 above PL2
  CHECK(r.Set("msr.psys.pl2", "320").ok);
  CHECK(r.Set("msr.psys.pl1", "300").ok);
  CHECK(r.Set("msr.psys.pl2.en", "0").ok);
  CHECK_EQ(r.S().Str("msr.psys.pl2.en"), std::string("0"));
  // BD PROCHOT: off, then on again; the other bits of MSR 0x1FC survive
  CHECK(r.Set("msr.bdp", "0").ok);
  CHECK_EQ(r.S().Str("msr.bdp"), std::string("0"));
  CHECK_EQ(r.w.plat.msr[0x1FC], 0x2ULL);
  CHECK(r.Set("msr.bdp", "1").ok);
  CHECK_EQ(r.w.plat.msr[0x1FC], 0x3ULL);
}

TEST(ctl_msr_platform_limit_locked_or_absent) {
  Rig r;
  r.w.plat.msrPsysLocked = true;
  r.c.ExecuteSync([] { Request q; q.op = "cmd"; q.key = "probe"; return q; }());
  r.Cfg("cfg.experimental", "1");
  CHECK_EQ(r.S().Str("cap.msr.psys"), std::string("locked"));
  CHECK_EQ(r.S().Str("msr.psys.lock"), std::string("1"));
  CHECK_EQ(r.Set("msr.psys.pl1", "230", Consent("msr_write")).code, std::string("unsupported"));
  // BD PROCHOT is independent of the PSys lock
  CHECK(r.Set("msr.bdp", "0", Consent("msr_write")).ok);
}

// ------------------------------------------------------------------------------------------------ Windows policy / display / devices / light
TEST(ctl_os_cpu_policy) {
  Rig r;
  CHECK(r.Set("os.boost", "disabled").ok);
  CHECK_EQ(r.w.power.policy.boost, 0);
  CHECK_EQ(r.S().Str("os.boost"), std::string("disabled"));
  CHECK(r.Set("os.max", "80").ok);
  CHECK_EQ(r.Set("os.min", "90").code, std::string("range"));   // min above max
  CHECK_EQ(r.Set("os.max", "3").code, std::string("range"));    // below the registry minimum
  r.w.power.failSet = true;
  CHECK_EQ(r.Set("os.epp", "50").code, std::string("failed.hw"));
}

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
  CHECK(r.Cmd("probe").ok);   // the user pressed "re-probe hardware"
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
  std::string cmd = "rm -rf " + dir;
  int rc = std::system(cmd.c_str());
  (void)rc;
}

TEST(ctl_lighting) {
  Rig r;
  size_t n0 = r.w.light.sent.size();
  CHECK(r.Set("light.kbd.color", "#10aa40").ok);
  CHECK(r.w.light.sent.size() > n0);
  bool sawColor = false;
  for (auto& p : r.w.light.sent)
    if (p.second[1] == 0x14 && p.second[4] == 0x10 && p.second[5] == 0xAA && p.second[6] == 0x40) sawColor = true;
  CHECK(sawColor);
  CHECK_EQ(r.S().Str("light.kbd.color"), std::string("#10aa40"));
  CHECK_EQ(r.Set("light.kbd.color", "green").code, std::string("range"));
  CHECK_EQ(r.Set("light.bar.color", "#ffffff").code, std::string("unsupported"));  // this model has no light bar
  CHECK(r.Set("light.kbd.effect", "cycle").ok);
  size_t n1 = r.w.light.sent.size();
  r.Run(3, 100);  // the software effect streams colour reports
  CHECK(r.w.light.sent.size() > n1 + 5);
  CHECK(r.Set("light.kbd.power", "0").ok);
  CHECK_EQ(r.S().Str("light.kbd.power"), std::string("0"));
}

// What the keyboard was last told: "off" (the power-off sequence), or the brightness byte of the last level command.
namespace {
struct LastLight {
  bool off = false;
  int level = -1;   // HID brightness byte of the keyboard
};
LastLight KbdNow(Rig& r) {
  LastLight l;
  for (auto& p : r.w.light.sent) {
    if (p.first != LightZone::Keyboard) continue;
    if (p.second[1] == 0x1A && p.second[3] == 0x00) l.off = true;    // power-off sequence ends with 1A zone 00
    if (p.second[1] == 0x1A && p.second[3] == 0x01) l.off = false;   // power-on sequence starts with 1A zone 01
    if (p.second[1] == 0x08 && p.second[2] == 0x02) l.level = p.second[5];
  }
  return l;
}
KV IdleOn(int on) {
  KV a;
  a.SetI("on", on);
  return a;
}
}  // namespace

TEST(ctl_lighting_idle_state_is_temporary) {
  Rig r;
  CHECK(r.Set("light.kbd.color", "#10aa40").ok);
  CHECK(r.Set("light.kbd.bright", "3").ok);
  CHECK(r.Set("light.idle", "30").ok);
  CHECK_EQ(r.S().Int("light.idle"), 30);
  CHECK_EQ(r.S().Str("light.off.why"), std::string(""));
  CHECK(!KbdNow(r).off);
  // the UI's watcher reports idle: the keyboard goes dark, the stored setting does not change
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  CHECK(KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string("idle"));
  CHECK_EQ(r.S().Str("light.kbd.power"), std::string("1"));
  CHECK_EQ(r.S().Str("light.kbd.color"), std::string("#10aa40"));
  // a repeated report only keeps the state alive; it does not send anything again
  const size_t n = r.w.light.sent.size();
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  CHECK_EQ(r.w.light.sent.size(), n);
  // input: everything comes back, colour and brightness as they were
  CHECK(r.Cmd("light.idle.state", IdleOn(0)).ok);
  CHECK(!KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string(""));
  bool sawColor = false;
  for (size_t i = n; i < r.w.light.sent.size(); i++) {
    const auto& p = r.w.light.sent[i].second;
    if (p[1] == 0x14 && p[4] == 0x10 && p[5] == 0xAA && p[6] == 0x40) sawColor = true;
  }
  CHECK(sawColor);
  CHECK_EQ(KbdNow(r).level, 8 + 2 * 14);   // level 3
}

TEST(ctl_lighting_idle_ends_when_the_watcher_goes_silent) {
  Rig r;
  CHECK(r.Set("light.idle", "10").ok);
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  CHECK(KbdNow(r).off);
  r.Run(10);   // the report is repeated every few seconds while idle: here it is not
  CHECK(KbdNow(r).off);
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);   // heartbeat
  r.Run(15);
  CHECK(KbdNow(r).off);
  r.Run(10);   // 25 s without a report: the UI is gone, so the lights must not stay dark
  CHECK(!KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string(""));
}

TEST(ctl_lighting_idle_switched_off_wakes_the_lights) {
  Rig r;
  CHECK(r.Set("light.idle", "30").ok);
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  CHECK(KbdNow(r).off);
  CHECK(r.Set("light.idle", "0").ok);
  CHECK(!KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string(""));
  // a late report from a watcher that has not noticed yet changes nothing
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  CHECK(!KbdNow(r).off);
}

TEST(ctl_lighting_idle_needs_a_keyboard_controller) {
  Rig r;
  r.w.light.kbd = false;
  CHECK(r.Cmd("probe").ok);
  CHECK_EQ(r.Cmd("light.idle.state", IdleOn(1)).code, std::string("unsupported"));
  CHECK_EQ(r.Set("light.idle", "30").code, std::string("unsupported"));    // capability gate: nothing to time out
  CHECK_EQ(r.Set("light.battdim", "1").code, std::string("unsupported"));
}

TEST(ctl_lighting_dims_on_battery) {
  Rig r;
  CHECK(r.Set("light.kbd.bright", "4").ok);
  CHECK(r.Set("light.battdim", "1").ok);
  CHECK(r.Set("light.battlevel", "2").ok);
  r.Run(2);
  CHECK_EQ(KbdNow(r).level, 8 + 3 * 14);   // on the charger: the chosen level 4
  CHECK_EQ(r.S().Int("light.bright.eff"), 4);
  r.w.plat.ac = false;
  r.Run(3);
  CHECK_EQ(KbdNow(r).level, 8 + 1 * 14);   // unplugged: limited to level 2
  CHECK_EQ(r.S().Int("light.bright.eff"), 2);
  CHECK_EQ(r.S().Int("light.kbd.bright"), 4);   // the stored choice is untouched
  r.w.plat.ac = true;
  r.Run(3);
  CHECK_EQ(KbdNow(r).level, 8 + 3 * 14);
  // a level below the limit is never raised
  r.w.plat.ac = false;
  CHECK(r.Set("light.kbd.bright", "1").ok);
  r.Run(3);
  CHECK_EQ(KbdNow(r).level, 8);
  // switching the rule off while on battery restores the chosen level straight away
  CHECK(r.Set("light.kbd.bright", "4").ok);
  r.Run(2);
  CHECK_EQ(KbdNow(r).level, 8 + 1 * 14);
  CHECK(r.Set("light.battdim", "0").ok);
  CHECK_EQ(KbdNow(r).level, 8 + 3 * 14);
}

TEST(ctl_lighting_follows_the_display_and_sleep) {
  Rig r;
  CHECK(r.Set("light.kbd.color", "#ff8000").ok);
  CHECK(r.S().Bool("light.sleepoff"));   // on by default
  r.c.Notify("display_off");
  r.Run(2);
  CHECK(KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string("display"));
  CHECK_EQ(r.S().Str("light.kbd.power"), std::string("1"));
  r.c.Notify("display_on");
  r.Run(2);   // the lights come back a moment after the wake, then once more a few seconds later
  CHECK(!KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string(""));
  // suspend / resume
  r.c.Notify("suspend");
  r.Run(2);
  CHECK(KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string("sleep"));
  r.w.light.sent.clear();
  r.c.Notify("resume");
  r.Run(2);
  CHECK(!KbdNow(r).off);
  const size_t first = r.w.light.sent.size();
  CHECK(first > 0);
  r.Run(8);
  CHECK(r.w.light.sent.size() > first);   // the second look after a few seconds
  CHECK(!KbdNow(r).off);
  // with the option off the lights ignore the display and sleep, but resume still restores them
  CHECK(r.Set("light.sleepoff", "0").ok);
  r.c.Notify("display_off");
  r.Run(2);
  CHECK(!KbdNow(r).off);
  CHECK_EQ(r.S().Str("light.off.why"), std::string(""));
}

TEST(ctl_lighting_user_off_wins_over_every_reason) {
  Rig r;
  CHECK(r.Set("light.kbd.power", "0").ok);
  CHECK(r.Set("light.idle", "30").ok);
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  CHECK(r.Cmd("light.idle.state", IdleOn(0)).ok);
  r.c.Notify("resume");
  r.Run(3);
  CHECK(KbdNow(r).off);   // the user turned the light off: nothing switches it on again
  CHECK_EQ(r.S().Str("light.kbd.power"), std::string("0"));
}

TEST(ctl_lighting_effects_pause_while_the_lights_are_off) {
  Rig r;
  CHECK(r.Set("light.kbd.effect", "cycle").ok);
  CHECK(r.Set("light.idle", "30").ok);
  r.Run(2, 100);
  const size_t streaming = r.w.light.sent.size();
  CHECK(r.Cmd("light.idle.state", IdleOn(1)).ok);
  const size_t atIdle = r.w.light.sent.size();
  r.Run(3, 100);
  CHECK_EQ(r.w.light.sent.size(), atIdle);   // not a single colour report while dark
  CHECK(streaming > 0);
  CHECK(r.Cmd("light.idle.state", IdleOn(0)).ok);
  r.Run(2, 100);
  CHECK(r.w.light.sent.size() > atIdle + 5);  // the effect runs again
}

// ------------------------------------------------------------------------------------------------ profiles
TEST(ctl_builtin_profiles_apply) {
  Rig r;
  KV a;
  a.Set("name", "Balanced");
  Result res = r.Cmd("profile.apply", a);
  CHECK(res.ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(60));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(80));
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(120));
  CHECK_EQ(r.Ec(reg::kCpuTempLimitRead) & 0x7F, 10);  // 95 C
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
  a.Set("name", "Overboost");
  CHECK(r.Cmd("profile.apply", a).ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  CHECK_EQ(r.Ec(reg::kPl4), static_cast<uint8_t>(200));
  CHECK(r.Ec(reg::kFanMode) & 0x40);
  a.Set("name", "Quiet");
  CHECK(r.Cmd("profile.apply", a).ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(35));
  CHECK(!(r.Ec(reg::kFanMode) & 0x40));
  a.Set("name", "Nope");
  CHECK_EQ(r.Cmd("profile.apply", a).code, std::string("unknown"));
}

TEST(ctl_custom_profile_save_apply_delete) {
  Rig r;
  r.Set("ec.pl1", "95");
  r.Set("ec.pl2", "110");
  KV a;
  a.Set("name", "My game");
  CHECK(r.Cmd("profile.save", a).ok);
  CHECK(r.S().Str("profile.list").find("My game") != std::string::npos);
  r.Set("ec.pl1", "50");
  CHECK(r.Cmd("profile.apply", a).ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(95));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(110));
  KV bad;
  bad.Set("name", "evil=name|x");
  CHECK_EQ(r.Cmd("profile.save", bad).code, std::string("range"));
  bad.Set("name", "a,b");   // the comma separates the names in the published list
  CHECK_EQ(r.Cmd("profile.save", bad).code, std::string("range"));
  bad.Set("name", std::string("ctl\x01name"));
  CHECK_EQ(r.Cmd("profile.save", bad).code, std::string("range"));
  bad.Set("name", std::string("broken\xC3"));   // truncated UTF-8
  CHECK_EQ(r.Cmd("profile.save", bad).code, std::string("range"));
  KV tr;
  tr.Set("name", "G\xC3\xBCnd\xC3\xBCz \xC5\x9F""ark\xC4\xB1");   // Turkish letters are fine
  CHECK(r.Cmd("profile.save", tr).ok);
  CHECK(r.S().Str("profile.list").find("G\xC3\xBCnd\xC3\xBCz") != std::string::npos);
  CHECK(r.Cmd("profile.delete", tr).ok);
  KV builtin;
  builtin.Set("name", "Quiet");
  CHECK_EQ(r.Cmd("profile.delete", builtin).code, std::string("range"));
  CHECK_EQ(r.Cmd("profile.save", builtin).code, std::string("range"));   // a built-in name cannot be overwritten
  builtin.Set("name", "overboost");
  CHECK_EQ(r.Cmd("profile.save", builtin).code, std::string("range"));
  CHECK(r.Cmd("profile.delete", a).ok);
  CHECK(r.S().Str("profile.list").find("My game") == std::string::npos);
}

TEST(ctl_profile_with_unconsented_values_is_partial) {
  Rig r;
  r.Set("ec.pl2", "150", Consent("ec_pl_high"));
  KV a;
  a.Set("name", "Hot");
  CHECK(r.Cmd("profile.save", a).ok);
  CHECK(r.Cmd("consent.reset").ok);
  CHECK(r.Set("ec.pl1", "60").ok);   // lowering needs no consent even though PL2 stays at 150
  CHECK(r.Set("ec.pl2", "90").ok);
  Result res = r.Cmd("profile.apply", a);
  CHECK(!res.ok);                               // the high value needs consent again
  CHECK_EQ(res.data.Str("consent"), std::string("ec_pl_high"));
  CHECK_EQ(r.Ec(reg::kPl2), static_cast<uint8_t>(90));
}

// ------------------------------------------------------------------------------------------------ re-apply / persistence
TEST(ctl_reapply_after_resume_and_rate_limit) {
  Rig r;
  CHECK(r.Set("ec.pl1", "100").ok);
  r.w.plat.ec[reg::kPl1] = 120;  // firmware reset after resume
  r.c.Notify("resume");
  r.Run(5);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(100));
  // a program that keeps resetting the value must not cause an endless fight
  int wrote = 0;
  for (int i = 0; i < 12; i++) {
    r.w.plat.ec[reg::kPl1] = 120;
    int before = r.w.plat.ecWrites;
    r.c.Notify("resume");
    r.Run(5);
    if (r.w.plat.ecWrites != before) wrote++;
  }
  CHECK(wrote <= 6);
  CHECK(r.w.log.Has("re-apply suppressed"));
}

TEST(ctl_keep_off_means_no_reapply) {
  Rig r;
  r.Cfg("cfg.keep", "0");
  r.Set("ec.pl1", "100");
  r.w.plat.ec[reg::kPl1] = 120;
  r.c.Notify("resume");
  r.Run(5);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
}

TEST(ctl_persistence_across_restart) {
  std::string dir = TempDir();
  CHECK(!dir.empty());
  {
    Rig r(sim::Model::Exact, dir);
    CHECK(r.Set("ec.pl1", "95").ok);
    KV a;
    a.Set("name", "Persisted");
    CHECK(r.Cmd("profile.save", a).ok);
    r.Cfg("cfg.lang", "tr");
  }
  {
    Rig r(sim::Model::Exact, dir);   // fresh machine at the OEM default 120/120/200
    CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
    CHECK(r.S().Str("profile.list").find("Persisted") != std::string::npos);
    CHECK_EQ(r.S().Str("cfg.lang"), std::string("tr"));
    r.Run(12);                        // start-up re-apply fires after ~8 s
    CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(95));
  }
  std::string cmd = "rm -rf " + dir;
  int rc = std::system(cmd.c_str());
  (void)rc;
}

TEST(ctl_desired_file_with_garbage_is_ignored) {
  std::string dir = TempDir();
  if (FILE* f = fopen((dir + "/desired.kv").c_str(), "wb")) {
    fputs("ec.pl1=9999\nbogus.key=1\nec.pl2=abc\nec.tcc=90\n", f);
    fclose(f);
  }
  {
    Rig r(sim::Model::Exact, dir);
    r.Run(12);
    CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));  // out-of-range / unknown / malformed entries were dropped
    CHECK_EQ(r.Ec(reg::kCpuTempLimitRead) & 0x7F, 15);       // the valid one was applied
  }
  std::string cmd = "rm -rf " + dir;
  int rc = std::system(cmd.c_str());
  (void)rc;
}

// ------------------------------------------------------------------------------------------------ application profiles
TEST(ctl_application_profile_switching) {
  Rig r;
  r.Cfg("cfg.app.0.exe", "game.exe");
  r.Cfg("cfg.app.0.profile", "Overboost");
  KV a;
  a.Set("name", "Balanced");
  r.Cmd("profile.apply", a);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(60));
  r.w.power.exes = {"explorer.exe", "game.exe"};
  r.Run(5);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  r.w.power.exes = {"explorer.exe"};
  r.Run(5);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(60));  // back to the previous profile
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
  // adding appends and normalises the image name (trimmed, lower case, ".exe" added)
  CHECK(set("", "  Game.EXE ", "Overboost", "My game").ok);
  CHECK(set("", "blender", "Enthusiast").ok);
  KV s = r.S();
  CHECK_EQ(s.Str("cfg.app.0.exe"), std::string("game.exe"));
  CHECK_EQ(s.Str("cfg.app.0.profile"), std::string("Overboost"));
  CHECK_EQ(s.Str("cfg.app.0.name"), std::string("My game"));
  CHECK_EQ(s.Str("cfg.app.1.exe"), std::string("blender.exe"));
  CHECK_EQ(s.Str("cfg.app.1.name"), std::string(""));
  // refused: a path, a wildcard, an unknown profile, a duplicate, an over-long name, bad or empty slots
  CHECK(!set("", "C:\\Games\\game.exe", "Quiet").ok);
  CHECK(!set("", "ga*me.exe", "Quiet").ok);
  CHECK(!set("", "notexe.dll", "Quiet").ok);
  CHECK(!set("", "x.exe", "No such profile").ok);
  CHECK(!set("", "GAME.exe", "Quiet").ok);
  CHECK(!set("", "name.exe", "Quiet", std::string(49, 'x')).ok);
  CHECK(!set("", "", "Quiet").ok);
  CHECK(!set("7", "y.exe", "Quiet").ok);
  CHECK(!set("32", "y.exe", "Quiet").ok);
  CHECK(!set("abc", "y.exe", "Quiet").ok);
  CHECK(!slotCmd("app.delete", "9").ok);
  CHECK(!slotCmd("app.move", "0", "-1").ok);   // already first
  CHECK(!slotCmd("app.move", "1", "1").ok);    // already last
  // editing keeps the program name allowed for its own slot
  CHECK(set("0", "game.exe", "Balanced", "My game").ok);
  CHECK_EQ(r.S().Str("cfg.app.0.profile"), std::string("Balanced"));

  // the first running entry of the list wins; the previous profile comes back when it closes
  KV b;
  b.Set("name", "Quiet");
  CHECK(r.Cmd("profile.apply", b).ok);
  r.w.power.exes = {"explorer.exe", "blender.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("blender.exe"));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  CHECK_EQ(r.S().Str("app.before"), std::string("Quiet"));
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
  CHECK_EQ(r.S().Str("profile.active"), std::string("Quiet"));
  r.w.power.exes = {"explorer.exe"};
  r.Run(5);
  r.w.power.exes = {"game.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Quiet"));   // matching is off: nothing changes
  CHECK(r.Cfg("cfg.app.auto", "1").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string("game.exe"));
  // deleting the entry that matches ends its profile
  CHECK(slotCmd("app.delete", "1").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string(""));
  CHECK_EQ(r.S().Str("profile.active"), std::string("Quiet"));
  CHECK_EQ(r.S().Str("cfg.app.1.exe"), std::string(""));
  CHECK(!slotCmd("app.delete", "1").ok);

  // an entry whose profile was deleted is skipped instead of blocking the entries below it
  CHECK(r.Cmd("profile.apply", b).ok);
  KV n;
  n.Set("name", "Night");
  CHECK(r.Cmd("profile.save", n).ok);
  CHECK(set("", "night.exe", "Night").ok);
  CHECK(r.Cmd("profile.delete", n).ok);
  r.w.power.exes = {"night.exe", "explorer.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("app.active"), std::string(""));
  CHECK_EQ(r.S().Str("cfg.app.1.profile"), std::string("Night"));   // still listed, so the page can flag it
}

TEST(ctl_power_source_profile_switching) {
  Rig r;
  KV a;
  a.Set("name", "Overboost");
  CHECK(r.Cmd("profile.apply", a).ok);
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  // switched off: unplugging changes nothing
  r.w.plat.ac = false;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  r.w.plat.ac = true;
  r.Run(5);
  // switched on: the first look only records the state ...
  CHECK(r.Cfg("cfg.prof.auto", "1").ok);
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  // ... unplugging then picks the battery profile (default Quiet) ...
  r.w.plat.ac = false;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Quiet"));
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(35));
  // ... and plugging back in restores what was running before
  r.w.plat.ac = true;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  // explicit profiles for both sources
  CHECK(r.Cfg("cfg.prof.batt", "Balanced").ok);
  CHECK(r.Cfg("cfg.prof.ac", "Enthusiast").ok);
  r.w.plat.ac = false;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
  r.w.plat.ac = true;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Enthusiast"));
  // a profile chosen by hand stays: the rule reacts to a change, not to a state
  KV q;
  q.Set("name", "Quiet");
  CHECK(r.Cmd("profile.apply", q).ok);
  r.Run(10);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Quiet"));
  // an unknown profile name is ignored instead of breaking anything
  CHECK(r.Cfg("cfg.prof.batt", "Does not exist").ok);
  r.w.plat.ac = false;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Quiet"));
}

TEST(ctl_power_source_yields_to_application_profile) {
  Rig r;
  CHECK(r.Cfg("cfg.prof.auto", "1").ok);
  CHECK(r.Cfg("cfg.app.0.exe", "game.exe").ok);
  CHECK(r.Cfg("cfg.app.0.profile", "Overboost").ok);
  KV a;
  a.Set("name", "Balanced");
  CHECK(r.Cmd("profile.apply", a).ok);
  r.Run(5);
  r.w.power.exes = {"explorer.exe", "game.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  // unplugging while the game runs must not override the game's profile
  r.w.plat.ac = false;
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Overboost"));
  CHECK_EQ(r.Ec(reg::kPl1), static_cast<uint8_t>(120));
  // closing the game returns to the profile that was active before it
  r.w.power.exes = {"explorer.exe"};
  r.Run(5);
  CHECK_EQ(r.S().Str("profile.active"), std::string("Balanced"));
}

TEST(ctl_profile_summaries_are_published) {
  Rig r;
  r.Set("ec.pl1", "95");
  r.Set("ec.pl2", "110");
  KV a;
  a.Set("name", "My game");
  CHECK(r.Cmd("profile.save", a).ok);
  KV s = r.S();
  const std::string v = s.Str("profile.v.My game");
  CHECK(v.find("ec.pl1=95") != std::string::npos);
  CHECK(v.find("ec.pl2=110") != std::string::npos);
  CHECK(!s.Str("profile.v.Quiet").empty());
  CHECK(!s.Str("profile.v.Overboost").empty());
  CHECK(r.Cmd("profile.delete", a).ok);
  CHECK(!r.S().Has("profile.v.My game"));
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
  Request q; q.op = "set"; q.key = "os.epp"; q.value = "25";
  Result res = c.Execute(q);
  CHECK(res.ok);
  uint64_t id = c.Submit(q);
  Result out;
  bool got = false;
  for (int i = 0; i < 200 && !got; i++) {
    got = c.TakeResult(id, &out);
    if (!got) usleep(10000);
  }
  CHECK(got && out.ok);
  c.Stop();
}

TEST(ctl_log_tail_and_error_state) {
  Rig r;
  r.Set("ec.pl1", "5");
  CHECK(r.S().Str("last.error").find("range") != std::string::npos);
  r.Set("ec.pl1", "100");
  CHECK(r.S().Str("log.tail").find("set ec.pl1=100 ok") != std::string::npos);
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
