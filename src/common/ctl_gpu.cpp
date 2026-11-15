// Controller: GPU. cTGP (125..150 W) is delegated to the vendored MONSTER GPU POWER engine (verified on GM5TG7Y:
// identity guard, read-before-write, NVML confirmation, rollback); it is the only thing in this program that writes to the embedded
// controller. The graphics driver is also asked what clock offsets are in force, so the Performance page shows what really runs.
#include <cmath>

#include "../mgp/core/persist_txn.h"
#include "controller.h"
#include "i18n_mark.h"
#include "util.h"

namespace bc {

void Controller::ProbeGpu() {
  std::string why;
  bool nvml = hw_.gpu && hw_.gpu->Init(&why);
  SetCap("gpu.nvml", nvml ? "ok" : "absent", why);
  engineReady_ = false;
  if (!engine_) {
    SetCap("gpu.ctgp", "absent", N_("power engine unavailable (no EC/NVML/SMBIOS access)"));
    SetCap("oem.sync", "absent");
    return;
  }
  lastLive_ = engine_->ReadLive(false, false);
  engineReady_ = true;
  SetCap("oem.sync", hw_.oemFiles ? "ok" : "absent");
}

void Controller::PollGpu() {
  KV k;
  if (hw_.gpu) {
    GpuInfo g;
    if (hw_.gpu->Read(&g) && g.ok) {
      k.Set("gpu.name", g.name);
      k.SetI("gpu.temp", g.tempC);
      k.SetF("gpu.power", g.powerW, 1);
      k.SetF("gpu.limit", g.limitW, 0);
      k.SetI("gpu.clock", g.gfxMhz);
      k.SetI("gpu.memclock", g.memMhz);
      k.SetI("gpu.clock.max", g.maxGfxMhz);
      k.SetI("gpu.util", g.utilPct);
      k.SetI("gpu.vram.util", g.memUtilPct);
      k.SetF("gpu.vram.used", g.vramUsedMb, 0);
      k.SetF("gpu.vram.total", g.vramTotalMb, 0);
      if (g.fanPct >= 0) k.SetI("gpu.fan", g.fanPct);
      if (g.pstate >= 0) k.SetI("gpu.pstate", g.pstate);
      k.Set("gpu.throttle", Hex(g.throttleMask));
      k.SetB("gpu.thr.power", g.throttleMask & (kNvThrottleSwPower | kNvThrottleHwPowerBrake));
      k.SetB("gpu.thr.thermal", g.throttleMask & (kNvThrottleSwThermal | kNvThrottleHwThermal));
      k.SetB("gpu.thr.hwslow", g.throttleMask & kNvThrottleHwSlow);
      k.SetB("gpu.thr.idle", g.throttleMask & kNvThrottleIdle);
      if (g.haveOffsets) {
        k.SetI("nv.offset.core", g.coreOffsetMhz);
        k.SetI("nv.offset.mem", g.memOffsetMhz);
      }
    }
  }
  if (engine_ && engineReady_) {
    mgp::LiveState s = engine_->ReadLive(true, false);
    lastLive_ = s;
    const int w = s.activeProfile ? s.activeProfile : s.ecProfile;
    if (w) k.SetI("gpu.ctgp", w);
    if (s.ec.ok) {
      k.SetI("gpu.ec.ctrl", s.ec.ctrl);
      k.SetI("gpu.ec.offset", s.ec.offset);
    }
    k.SetB("gpu.ctgp.consistent", s.consistent);
    std::string state = "ok", why;
    if (s.identity.verdict != mgp::IdentityVerdict::Exact) {
      state = "off";
      why = Fmt(N_("not the verified GM5TG7Y (%s)"), s.identity.summary.c_str());
    } else if (!s.driverProbed || s.driver == mgp::DriverState::Absent) {
      state = "absent";
      why = N_("EC driver absent");
    } else if (!s.CanWrite()) {
      state = "blocked";
      why = s.blockerDetails.empty() ? N_("blocked") : s.blockerDetails[0];
    }
    SetCap("gpu.ctgp", state, why);
    if (engine_->FaultLatched()) k.Set("gpu.fault", "1");
  }
  std::lock_guard<std::mutex> g(snapMu_);
  sGpu_ = k;
}

namespace {
Result MapOutcome(Controller* self, const mgp::ApplyResult& r, std::string* msgOut) {
  (void)self;
  Result res;
  *msgOut = r.detail;
  switch (r.outcome) {
    case mgp::Outcome::Ok:
    case mgp::Outcome::AlreadyActive:
      res.ok = true;
      res.code = "ok";
      break;
    case mgp::Outcome::BlockedTemp: res.code = "gate.thermal"; break;
    case mgp::Outcome::BlockedAboveVbiosMax: res.code = "range"; break;
    case mgp::Outcome::BlockedConsent: res.code = "gate.consent"; break;
    case mgp::Outcome::BlockedNotReady:
    case mgp::Outcome::BlockedUnknownProfile: res.code = "unsupported"; break;
    case mgp::Outcome::FailedNoChange: res.code = "failed.hw"; break;
    case mgp::Outcome::FailedRolledBack: res.code = "reverted"; break;
    case mgp::Outcome::FailedRollbackUnverified: res.code = "failed.verify"; break;
  }
  res.msg = r.detail;
  return res;
}
}  // namespace

Result Controller::SetCtgp(int watts, const KV& args) {
  if (!engine_ || !engineReady_) return Fail("unsupported", N_("GPU power engine unavailable"));
  mgp::ApplyOptions o;
  o.consentOverOem = cfg_.Bool("cfg.consent.ctgp_over_oem", false);
  o.consentUnverified = cfg_.Bool("cfg.consent.ctgp_unverified", false);
  o.consentModel = cfg_.Bool("cfg.consent.ctgp_model", false);
  auto needConsent = [&](const char* what) {
    Result r = Fail("gate.consent", std::string("confirmation required: ") + what);
    r.data.Set("consent", what);
    return r;
  };
  if (watts > mgp::kOemMaxWatts && !o.consentOverOem) return needConsent("ctgp_over_oem");
  if ((watts == 130 || watts == 135) && !o.consentUnverified) return needConsent("ctgp_unverified");

  // another tool (MONSTER GPU POWER, its logon task) may be applying cTGP right now: take the shared lock for the whole sequence
  struct LockGuard {
    ICrossProcessLock* l;
    bool held = false;
    ~LockGuard() { if (held) l->Release(); }
  } lock{hw_.applyLock};
  if (hw_.applyLock) {
    if (!hw_.applyLock->Acquire(5000)) return Fail("busy", N_("another program is applying a GPU power change right now; try again"));
    lock.held = true;
  }

  const bool oemRunning = hw_.oemSvc && hw_.oemSvc->Probe().running;
  if (oemRunning && hw_.oemFiles && cfg_.Bool("cfg.oemSync", false)) {
    // keep the OEM profile file in step so the OEM service does not undo the change at the next display wake/boot
    mgp::oem::OemProfileStore store(*hw_.oemFiles, *hw_.log);
    mgp::PersistTxnInput in;
    in.watts = watts;
    in.opts = o;
    in.approvedSha = args.Str("approve.sha");
    in.foreignApproved = args.Bool("approve.foreign", false);
    in.own.ownedSha = cfg_.Str("cfg.oem.ownedSha");
    in.own.ownedWatts = static_cast<int>(cfg_.Int("cfg.oem.ownedWatts", 0));
    mgp::PersistTxnResult t = mgp::RunPersistTransaction(*engine_, store, *hw_.log, in);
    mgp::TxnClass c = mgp::ClassifyTxn(t);
    Result res;
    switch (c) {
      case mgp::TxnClass::NeedsApproval:
        res = Fail("needs.approval", "the OEM profile file holds a value Control Center Advanced did not write");
        res.data.Set("sha", t.pre.sha);
        res.data.SetI("oem.target", t.pre.target);
        return res;
      case mgp::TxnClass::Aborted: return Fail("busy", N_("the OEM profile changed after it was approved; nothing was applied"));
      case mgp::TxnClass::OemUnavailable: return Fail("gate.oem", std::string("OEM profile module locked: ") + mgp::oem::LockName(t.pre.lock));
      case mgp::TxnClass::LiveFailed: {
        std::string m;
        res = MapOutcome(this, t.live, &m);
        if (!res.ok && res.code == "internal") res.code = "failed.hw";
        lastError_ = res.code + ": " + m;
        return res;
      }
      case mgp::TxnClass::BothOk:
      case mgp::TxnClass::BothOkAlready: {
        mgp::OemOwnership own = mgp::OwnershipFromTxn(store.Inspect(), t);
        cfg_.Set("cfg.oem.ownedSha", own.ownedSha);
        cfg_.SetI("cfg.oem.ownedWatts", own.ownedWatts);
        SaveConfig();
        return Ok("applied live and written to the OEM profile");
      }
      case mgp::TxnClass::OemFailedLiveRestored: return Fail("reverted", N_("OEM profile write failed; the live change was undone"));
      case mgp::TxnClass::OemFailedRestoreUnverified: return Fail("failed.verify", N_("OEM profile write failed and the live change could not be verified as undone"));
      case mgp::TxnClass::OemFailedLiveUnchanged: return Fail("failed.hw", N_("OEM profile write failed"));
    }
    return Fail("internal", "unclassified transaction");
  }
  mgp::ApplyResult r = engine_->Apply(watts, o);
  std::string m;
  Result res = MapOutcome(this, r, &m);
  if (!res.ok && res.code != "gate.consent") {   // a consent that is still to be given is a question, not a failure
    lastError_ = res.code + ": " + m;
    L(mgp::LogLevel::Warn, "cTGP apply failed: " + lastError_);
  }
  if (res.ok && oemRunning) res.msg = "applied live; the vendor's service is running and may restore its own value after a profile change (it is applied again then)";
  return res;
}

}  // namespace bc
