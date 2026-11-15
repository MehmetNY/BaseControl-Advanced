#include "fake_oem.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "../common/registers.h"
#include "../common/util.h"
#include "../oem/json.h"

namespace bc {
namespace sim {

using oem::JsonOut;
using oem::JVal;
using oem::Msg;
using oem::Packet;
using oem::PacketReader;

namespace {

// One direction of an in-memory byte stream.
struct Pipe {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<uint8_t> q;
  bool closed = false;
  void Push(const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> g(mu);
    q.insert(q.end(), p, p + n);
    cv.notify_all();
  }
  bool TryPop(std::vector<uint8_t>* out) {
    std::lock_guard<std::mutex> g(mu);
    if (q.empty()) return false;
    out->assign(q.begin(), q.end());
    q.clear();
    return true;
  }
  int Pop(uint8_t* p, size_t cap, int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu);
    cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !q.empty() || closed; });
    if (!q.empty()) {
      size_t n = std::min(cap, q.size());
      for (size_t i = 0; i < n; i++) {
        p[i] = q.front();
        q.pop_front();
      }
      return static_cast<int>(n);
    }
    return closed ? -1 : 0;
  }
  void Close() {
    std::lock_guard<std::mutex> g(mu);
    closed = true;
    cv.notify_all();
  }
  bool Closed() {
    std::lock_guard<std::mutex> g(mu);
    return closed;
  }
};

struct Conn {
  Pipe c2s, s2c;
  PacketReader rd;      // server side
  bool authed = false, dead = false;
  std::string clientId;
  std::vector<std::string> filters;
};

bool IsControlTopic(const std::string& t) {
  return t == "Fan/Control" || t == "Keyboard/Ctrl" || t == "Setting/Control" || t == "BatteryProtection/Control" || t == "Customize/Control";
}

fan::TableImage ImageOf(const fan::Curve& c) { return fan::Pack(c); }

FakeServiceState Initial() {
  FakeServiceState s;
  s.mode = 2;   // Overboost, as the reference screenshots of the vendor UI show it
  s.profile[0] = s.profile[1] = s.profile[2] = 0;
  s.detail[0] = FakeDetail{1, 45, 64, 90, 0, 20, 0, 5, 0, 0, 87, 0};
  s.detail[1] = FakeDetail{1, 80, 100, 150, 0, 20, 0, 5, 0, 0, 87, 0};
  s.detail[2] = FakeDetail{1, 120, 120, 200, 0, 20, 0, 5, 0, 0, 87, 0};
  s.detail[3] = FakeDetail{1, 80, 100, 150, 0, 20, 0, 5, 0, 0, 87, 0};
  s.tableName = "M1T2";
  s.cpuTable = ImageOf(fan::DefaultCpu());
  s.gpuTable = ImageOf(fan::DefaultGpu());
  s.kb.rgb.assign(3 * s.kb.blocks, 255);
  return s;
}

const FakeDetail kDefaultDetail[4] = {FakeDetail{1, 45, 64, 90, 0, 20, 0, 5, 0, 0, 87, 0}, FakeDetail{1, 80, 100, 150, 0, 20, 0, 5, 0, 0, 87, 0},
                                      FakeDetail{1, 120, 120, 200, 0, 20, 0, 5, 0, 0, 87, 0}, FakeDetail{1, 80, 100, 150, 0, 20, 0, 5, 0, 0, 87, 0}};

}  // namespace

// ------------------------------------------------------------------------------------------------ implementation
struct FakeOemWorld::Impl {
  Platform* plat = nullptr;
  mutable std::mutex mu;
  std::condition_variable cv;
  std::thread th;
  bool stop = false, up = true, silent = false, ecApply = true;
  std::vector<std::string> accepted{"UWPClient"};
  std::vector<std::shared_ptr<Conn>> conns;
  std::vector<std::string> received;
  int accepts = 0, refusals = 0;
  std::string lastClientId;
  FakeServiceState st = Initial();

  // ---- broker -------------------------------------------------------------------------------------
  void Poke() {
    std::lock_guard<std::mutex> g(mu);
    cv.notify_all();
  }

  std::shared_ptr<Conn> NewConn() {
    std::lock_guard<std::mutex> g(mu);
    if (!up) return nullptr;
    auto c = std::make_shared<Conn>();
    conns.push_back(c);
    return c;
  }

  static void Send(Conn& c, const std::vector<uint8_t>& b) { c.s2c.Push(b.data(), b.size()); }

  static void Kill(Conn& c) {
    c.dead = true;
    c.s2c.Close();
    c.c2s.Close();
  }

  static bool ParseConnect(const Packet& p, std::string* id, std::string* user, std::string* pass) {
    const std::vector<uint8_t>& b = p.body;
    size_t i = 0;
    auto str = [&](std::string* out) {
      if (i + 2 > b.size()) return false;
      const size_t n = (static_cast<size_t>(b[i]) << 8) | b[i + 1];
      i += 2;
      if (i + n > b.size()) return false;
      out->assign(reinterpret_cast<const char*>(&b[i]), n);
      i += n;
      return true;
    };
    std::string proto;
    if (!str(&proto) || proto != "MQTT") return false;
    if (i + 4 > b.size()) return false;
    const uint8_t flags = b[i + 1];
    i += 4;   // level, flags, keep-alive
    if (!str(id)) return false;
    if (flags & 0x04) {   // will
      std::string t, m;
      if (!str(&t) || !str(&m)) return false;
    }
    user->clear();
    pass->clear();
    if ((flags & 0x80) && !str(user)) return false;
    if ((flags & 0x40) && !str(pass)) return false;
    return true;
  }

  bool Accepts(const std::string& id, const std::string& user, const std::string& pass) {
    const size_t us = id.rfind('_');
    if (us == std::string::npos) return false;
    const std::string prefix = id.substr(0, us);
    long long slot = 0;
    if (!ParseInt(id.substr(us + 1), slot) || slot < 0 || slot > 19) return false;
    bool known = false;
    for (const std::string& a : accepted) known = known || a == prefix;
    if (!known) return false;
    const oem::Identity want = oem::MakeIdentity(prefix, static_cast<int>(slot));
    return user == want.user && pass == want.pass;
  }

  void Deliver(const std::string& topic, const std::string& payload) {
    const std::vector<uint8_t> pkt = oem::EncodePublish(topic, payload, 0, false, 0);
    for (auto& c : conns) {
      if (c->dead || !c->authed) continue;
      for (const std::string& f : c->filters)
        if (oem::TopicMatches(f, topic)) {
          Send(*c, pkt);
          break;
        }
    }
  }

  void Handle(const std::shared_ptr<Conn>& cp, const Packet& p) {
    Conn& c = *cp;
    if (!c.authed && p.type != oem::kConnect) {
      Kill(c);
      return;
    }
    switch (p.type) {
      case oem::kConnect: {
        std::string id, user, pass;
        if (!ParseConnect(p, &id, &user, &pass)) {
          Kill(c);
          return;
        }
        lastClientId = id;
        if (!Accepts(id, user, pass)) {
          refusals++;
          Send(c, {0x20, 0x02, 0x00, 0x04});   // CONNACK: bad user name or password
          c.s2c.Close();
          c.dead = true;
          return;
        }
        for (auto& o : conns)
          if (o != cp && o->authed && o->clientId == id) Kill(*o);   // a second session with the same id drops the older one
        c.authed = true;
        c.clientId = id;
        accepts++;
        Send(c, {0x20, 0x02, 0x00, 0x00});
        break;
      }
      case oem::kSubscribe: {
        const std::vector<uint8_t>& b = p.body;
        if (b.size() < 2) {
          Kill(c);
          return;
        }
        std::vector<uint8_t> ack = {0x90, 0x00, b[0], b[1]};
        size_t i = 2;
        while (i + 3 <= b.size()) {
          const size_t n = (static_cast<size_t>(b[i]) << 8) | b[i + 1];
          i += 2;
          if (i + n + 1 > b.size()) break;
          c.filters.emplace_back(reinterpret_cast<const char*>(&b[i]), n);
          i += n + 1;
          ack.push_back(0x00);
        }
        ack[1] = static_cast<uint8_t>(ack.size() - 2);
        Send(c, ack);
        break;
      }
      case oem::kPublish: {
        oem::PublishMsg m;
        if (!oem::DecodePublish(p, &m)) {
          Kill(c);
          return;
        }
        Route(m.topic, m.payload);
        break;
      }
      case oem::kPingreq: Send(c, {0xD0, 0x00}); break;
      case oem::kDisconnect: Kill(c); break;
      default: break;
    }
  }

  void Route(const std::string& topic, const std::string& payload) {
    const bool control = IsControlTopic(topic);
    if (control) {
      received.push_back(topic + " " + payload);
      if (received.size() > 600) received.erase(received.begin(), received.begin() + 100);
    }
    Deliver(topic, payload);   // like the real broker, capture subscribers see everything
    if (control && !silent) {
      for (const Msg& m : Service(topic, payload)) Deliver(m.topic, m.payload);
    }
  }

  void Run() {
    std::unique_lock<std::mutex> lk(mu);
    while (!stop) {
      std::vector<std::shared_ptr<Conn>> snapshot = conns;
      for (auto& c : snapshot) {
        if (c->dead) continue;
        std::vector<uint8_t> bytes;
        if (c->c2s.TryPop(&bytes)) {
          c->rd.Feed(bytes.data(), bytes.size());
          Packet p;
          PacketReader::Status s = PacketReader::NeedMore;
          while (!c->dead && (s = c->rd.Next(&p)) == PacketReader::Got) Handle(c, p);
          if (s == PacketReader::Bad) Kill(*c);
        } else if (c->c2s.Closed()) {
          Kill(*c);
        }
      }
      conns.erase(std::remove_if(conns.begin(), conns.end(), [](const std::shared_ptr<Conn>& c) { return c->dead; }), conns.end());
      cv.wait_for(lk, std::chrono::milliseconds(15));
    }
    for (auto& c : conns) Kill(*c);
    conns.clear();
  }

  // ---- the service ------------------------------------------------------------------------------------
  static const JVal* Get(const JVal& v, const char* k) { return v.FindI(k); }

  void ApplyToEc() {
    if (!plat || !ecApply) return;
    std::lock_guard<std::recursive_mutex> g(plat->mu);
    const FakeDetail& d = st.detail[st.mode];
    const FakeDetail& def = kDefaultDetail[st.mode];
    const FakeDetail& use = (st.mode == 3 || st.detailAnyMode) ? d : def;   // assumption: the user's values are applied by the custom profile
    plat->gpuCoreOffset = use.oc ? use.gCoreOc : use.gCore;
    plat->gpuMemOffset = use.oc ? use.gMemOc : use.gMem;
    plat->ec[reg::kPl1] = static_cast<uint8_t>(use.pl1);
    plat->ec[reg::kPl2] = static_cast<uint8_t>(use.pl2);
    const int pl4Scale = (plat->ec[reg::kCustomProfile] & 0x80) ? 2 : 1;   // bit 7 of 0x0727: the raw PL4 byte holds half the watts
    plat->ec[reg::kPl4] = static_cast<uint8_t>(use.pl4 / pl4Scale);
    plat->ec[reg::kCpuTempLimitRead] = static_cast<uint8_t>(use.tccSw ? 5 + use.tcc : 5);
    plat->ec[reg::kFanSwitchSpeed] = static_cast<uint8_t>((d.fanSwEn ? 0x80 : 0) | (d.fanSw / 100 & 0x7F));
    if (st.boost) plat->ec[reg::kFanMode] |= 0x40;
    else plat->ec[reg::kFanMode] &= static_cast<uint8_t>(~0x40);
    for (int i = 0; i < fan::kSlots; i++) {
      plat->ec[reg::kFanTableCpuUp + i] = st.cpuTable.up[i];
      plat->ec[reg::kFanTableCpuDown + i] = st.cpuTable.down[i];
      plat->ec[reg::kFanTableCpuDuty + i] = st.cpuTable.duty[i];
      plat->ec[reg::kFanTableGpuUp + i] = st.gpuTable.up[i];
      plat->ec[reg::kFanTableGpuDown + i] = st.gpuTable.down[i];
      plat->ec[reg::kFanTableGpuDuty + i] = st.gpuTable.duty[i];
    }
    if (st.fanCustom) {
      plat->ec[reg::kFanCurveEnable] |= 0x80;
      plat->ec[reg::kFanCurveLatch] |= 0x04;
    } else {
      plat->ec[reg::kFanCurveEnable] &= static_cast<uint8_t>(~0x80);
    }
    ApplyBattery();
    st.serviceEcWrites++;
  }

  // Charge profile: the EC keeps it in 0x07A6 bits 3..5 (assumption taken from the EC map: 0x08 high capacity, 0x18 balanced, 0x28 eco).
  void ApplyBattery() {
    if (!plat || !ecApply) return;
    std::lock_guard<std::recursive_mutex> g(plat->mu);
    const uint8_t code = st.battMode == 0 ? 0x08 : st.battMode == 2 ? 0x28 : 0x18;
    plat->ec[reg::kBatteryMode] = static_cast<uint8_t>((plat->ec[reg::kBatteryMode] & ~0x38) | code);
  }

  // A switch the service changed: the EC bit that shows it (natural polarity unless swInverted).
  void ApplySwitch(const std::string& stem, bool on) {
    if (!plat || !ecApply) return;
    struct Bit { const char* stem; uint16_t addr; uint8_t mask; };
    static const Bit kBits[] = {{"FNKEY", reg::kFnLock, 0x10}, {"ACRECOVERY_TOGGLE", reg::kAcAutoBoot, 0x08}, {"USB_CHARGER", reg::kUsbPowerShare, 0x10}};
    for (const Bit& b : kBits) {
      if (stem != b.stem) continue;
      std::lock_guard<std::recursive_mutex> g(plat->mu);
      const bool set = on != st.swInverted;
      if (set) plat->ec[b.addr] |= b.mask;
      else plat->ec[b.addr] &= static_cast<uint8_t>(~b.mask);
      st.serviceEcWrites++;
    }
  }

  Msg FanStatus() {
    const FakeDetail& d = st.detail[st.mode];
    JsonOut j;
    j.BeginObj();
    j.Int("OverClockingSwitch", d.oc).Int("OperatingMode", st.mode);
    j.Int("OfficeProfileIndex", st.profile[0]).Int("GamingProfileIndex", st.profile[1]).Int("TurboProfileIndex", st.profile[2]).Int("CustomProfileIndex", st.profile[3]);
    j.Int("FanBoostEnable", st.boost ? 1 : 0).Str("ProfileName", "Default");
    j.Int("FAN_FanSwitchSpeedEnabled", d.fanSwEn).Int("FAN_FanSwitchSpeed", d.fanSw).Str("FAN_TableName", st.tableName);
    j.Int("CPU_PL1", d.pl1).Int("CPU_PL1Maximum", 200).Int("CPU_PL1Minimum", 10);
    j.Int("CPU_PL2", d.pl2).Int("CPU_PL2Maximum", 200).Int("CPU_PL2Minimum", 10);
    j.Int("CPU_PL4", d.pl4).Int("CPU_PL4Maximum", 250).Int("CPU_PL4Minimum", 20);
    j.Int("CPU_TccOffsetSwitch", d.tccSw).Int("CPU_TccOffsetMaximum", 25).Int("CPU_TccOffset", d.tcc);
    j.Int("GPU_CoreClockOffset", d.gCore).Int("GPU_CoreClockOffsetOC", d.gCoreOc).Int("GPU_CoreClockOffsetMaximum", 200).Int("GPU_CoreClockOffsetMaximumHWOC", 300);
    j.Int("GPU_CoreClockOffsetMinimum", -200);
    j.Int("GPU_MemoryClockOffset", d.gMem).Int("GPU_MemoryClockOffsetOC", d.gMemOc).Int("GPU_MemoryClockOffsetMaximum", 1000);
    j.Int("GPU_MemoryClockOffsetMaximumHWOC", 1500).Int("GPU_MemoryClockOffsetMinimum", -500).Int("GPU_MemoryClockOffsetMinimumHWOC", -1000);
    j.Int("GPU_TargetTemperature", d.gTemp).Int("GPU_TargetTemperatureMaximum", 87).Int("GPU_TargetTemperatureMinimum", 75);
    j.Int("GPU_DynamicBoostSwitch", d.dynBoost);
    j.Int("OcSupport", 1).Int("IsAMDPlatform", 0).Int("IsNvGpu", 1).Int("TurboModeOption", 1).Int("FanBoostBtnSupport", 1);
    j.EndObj();
    return Msg{"Fan/Status", j.Take()};
  }

  static void TableObj(JsonOut& j, const char* key, const fan::TableImage& t) {
    j.BeginObj(key);
    for (int part = 0; part < 3; part++) {
      const char* names[3] = {"Duty", "UpT", "DownT"};
      const uint8_t* a = part == 0 ? t.duty : part == 1 ? t.up : t.down;
      j.BeginObj(names[part]);
      for (int i = 0; i < fan::kSlots; i++) j.Int(Fmt("T%d", i).c_str(), a[i]);
      j.EndObj();
    }
    j.EndObj();
  }

  Msg FanTable() {
    JsonOut j;
    j.BeginObj().Str("Name", st.tableName).Str("Type", "Default").Bool("FanControlRespective", st.fanCustom);
    TableObj(j, "CPU", st.cpuTable);
    TableObj(j, "GPU", st.gpuTable);
    j.EndObj();
    return Msg{"Fan/Table", j.Take()};
  }

  static bool ReadTable(const JVal* o, fan::TableImage* t) {
    if (!o || !o->IsObj()) return false;
    for (int part = 0; part < 3; part++) {
      const char* names[3] = {"Duty", "UpT", "DownT"};
      const JVal* p = o->FindI(names[part]);
      if (!p || !p->IsObj()) return false;
      uint8_t* a = part == 0 ? t->duty : part == 1 ? t->up : t->down;
      for (int i = 0; i < fan::kSlots; i++) {
        const JVal* e = p->FindI(Fmt("T%d", i));
        if (!e || !e->Numeric()) return false;
        a[i] = static_cast<uint8_t>(std::max<long long>(0, std::min<long long>(255, e->AsInt())));
      }
    }
    return true;
  }

  std::vector<Msg> FanControl(const JVal& v, const std::string& action) {
    std::vector<Msg> out;
    if (action == "GETSTATUS" || action == "DEFAULT") {
      out.push_back(FanStatus());
    } else if (action == "FAN_BOOST_ON" || action == "FAN_BOOST_OFF") {
      st.boost = action == "FAN_BOOST_ON";
      ApplyToEc();
      out.push_back(FanStatus());
    } else if (action == "OPERATING_OFFICE_MODE" || action == "OPERATING_GAMING_MODE" || action == "OPERATING_TURBO_MODE" ||
               action == "OPERATING_CUSTOM_MODE") {
      const int m = action == "OPERATING_OFFICE_MODE" ? 0 : action == "OPERATING_GAMING_MODE" ? 1 : action == "OPERATING_TURBO_MODE" ? 2 : 3;
      st.mode = m;
      if (const JVal* pi = Get(v, "ProfileIndex")) st.profile[m] = static_cast<int>(pi->AsInt());
      st.tableName = m == 0 ? "M2T1" : m == 1 ? "M1T1" : m == 2 ? "M1T2" : "M4T1";
      ApplyToEc();
      out.push_back(FanStatus());
      out.push_back(FanTable());
    } else if (action == "SET_OPERATING_MODE_DETAIL") {
      FakeDetail& d = st.detail[st.mode];
      struct Field { const char* key; int* dst; };
      const Field fields[] = {{"OverClockingSwitch", &d.oc}, {"PL1", &d.pl1}, {"PL2", &d.pl2}, {"PL4", &d.pl4}, {"FanSwitchSpeed", &d.fanSw},
                              {"FanSwitchSpeedEnabled", &d.fanSwEn}, {"CpuTccOffsetSwitch", &d.tccSw}, {"CpuTccOffset", &d.tcc},
                              {"GpuCoreClockOffset", &d.gCore}, {"GpuMemoryClockOffset", &d.gMem}, {"GpuTargetTemperature", &d.gTemp},
                              {"GpuDynamicBoostSwitch", &d.dynBoost}, {"GpuCoreClockOffsetOC", &d.gCoreOc}, {"GpuMemoryClockOffsetOC", &d.gMemOc}};
      for (const Field& f : fields)
        if (const JVal* e = Get(v, f.key))
          if (e->Numeric()) *f.dst = static_cast<int>(e->AsInt());
      ApplyToEc();
      out.push_back(FanStatus());
    } else if (action == "RESTORE_OPERATING_MODE_DETAIL") {
      st.detail[st.mode] = kDefaultDetail[st.mode];
      ApplyToEc();
      out.push_back(FanStatus());
    } else if (action == "GET_FAN_SPEED_CURVE_SETTING") {
      out.push_back(FanTable());
    } else if (action == "SET_FAN_SPEED_CURVE_SETTING") {
      fan::TableImage c = st.cpuTable, g = st.gpuTable;
      const JVal* cj = Get(v, "CPU");
      const JVal* gj = Get(v, "GPU");
      if ((cj && !ReadTable(cj, &c)) || (gj && !ReadTable(gj, &g))) return out;   // malformed: ignored, like a service that cannot parse it
      st.cpuTable = c;
      st.gpuTable = g;
      if (const JVal* n = Get(v, "Name")) st.tableName = n->AsString();
      ApplyToEc();
      out.push_back(FanTable());
    } else if (action == "SET_FAN_CONTROL_RESPECTIVE") {
      if (const JVal* e = Get(v, "FanControlRespective")) st.fanCustom = e->AsBool();
      ApplyToEc();
      out.push_back(FanTable());
    } else if (action == "RESTORE_FAN_SPEED_CURVE_SETTING" || action == "RESTORE_FAN_SPEED_CURVE_SETTING_ALL") {
      st.cpuTable = ImageOf(fan::DefaultCpu());
      st.gpuTable = ImageOf(fan::DefaultGpu());
      st.fanCustom = false;
      ApplyToEc();
      out.push_back(FanTable());
    }
    return out;
  }

  Msg KbStatus() {
    const FakeKb& k = st.kb;
    JsonOut j;
    j.BeginObj().Int("solution", 2).Str("type", "MEZone_2nd_101").Int("powerStatus", k.power).Int("effect", k.effect).Int("light", k.light);
    j.Int("speed", k.speed).Int("direction", k.direction).Bool("nv_save", false);
    j.BeginObj("color").Bool("isCircular", false).Int("ColorBlocks", k.blocks).BeginArr("ColorBuffer");
    for (int i = 0; i < k.blocks; i++) {
      j.BeginObj().Int("ID", i).Int("R", k.rgb[3 * i]).Int("G", k.rgb[3 * i + 1]).Int("B", k.rgb[3 * i + 2]).EndObj();
    }
    j.EndArr().EndObj();
    j.Int("LogoSupport", 0).Int("HingeSupport", 0).Int("BaseSupport", 0);
    j.EndObj();
    return Msg{"Keyboard/Status", j.Take()};
  }

  std::vector<Msg> KbControl(const JVal& v, const std::string& action) {
    std::vector<Msg> out;
    const JVal* fn = Get(v, "function");
    const std::string function = fn ? fn->AsString() : "";
    if (action == "GETSTATUS") out.push_back(KbStatus());
    if (function == "SetPower") {
      if (const JVal* p = Get(v, "powerstatus")) st.kb.power = static_cast<int>(p->AsInt());
      out.push_back(KbStatus());
    } else if (function == "SetEffectALL" || function == "SetColor") {
      FakeKb& k = st.kb;
      if (const JVal* e = Get(v, "mode")) k.mode = static_cast<int>(e->AsInt());
      if (const JVal* e = Get(v, "effect")) k.effect = static_cast<int>(e->AsInt());
      if (const JVal* e = Get(v, "light")) k.light = static_cast<int>(e->AsInt());
      if (const JVal* e = Get(v, "speed")) k.speed = static_cast<int>(e->AsInt());
      if (const JVal* e = Get(v, "direction")) k.direction = static_cast<int>(e->AsInt());
      if (const JVal* c = Get(v, "color")) {
        if (const JVal* buf = c->FindI("ColorBuffer")) {
          if (buf->IsArr()) {
            // The firmware has a fixed number of blocks: entries for blocks it does not have are dropped, missing ones keep their colour.
            for (const JVal& e : buf->arr) {
              const JVal* id = e.FindI("ID");
              if (!id) continue;
              const long long i = id->AsInt();
              if (i < 0 || i >= k.blocks) continue;
              if (const JVal* r = e.FindI("R")) k.rgb[3 * i] = static_cast<int>(std::max<long long>(0, std::min<long long>(255, r->AsInt())));
              if (const JVal* g = e.FindI("G")) k.rgb[3 * i + 1] = static_cast<int>(std::max<long long>(0, std::min<long long>(255, g->AsInt())));
              if (const JVal* b = e.FindI("B")) k.rgb[3 * i + 2] = static_cast<int>(std::max<long long>(0, std::min<long long>(255, b->AsInt())));
            }
          }
        }
      }
      out.push_back(KbStatus());
    }
    return out;
  }

  Msg SettingStatus() {
    auto sw = [&](const char* stem) {
      auto it = st.sw.find(stem);
      return it == st.sw.end() ? 0 : it->second;
    };
    JsonOut j;
    j.BeginObj();
    j.Int("WIFIEnable", sw("WIFI")).Int("BTEnable", sw("BT")).Int("WebCamEnable", sw("WEBCAM")).Int("TochpadEnable", sw("TOUCHPAD"));
    j.Int("WinKey", sw("WINKEY")).Int("OSD", sw("OSD_HIDDEN")).Int("NumPad", sw("NUMPAD")).Int("FnKey", sw("FNKEY")).Int("TouchpadToggle", sw("TOUCHPAD_TOGGLE"));
    j.Int("UsbCharger", sw("USB_CHARGER")).Int("AcRecoverySwitch_Support", 1).Int("AcRecoverySwitch_Status", sw("ACRECOVERY_TOGGLE"));
    j.Int("FnWith1HotkeySwitch_Status", sw("FN_WITH1_HOTKEY_TOGGLE"));
    j.Int("DiscreteGpuDirectConnectionSwitch_Support", 1).Int("DiscreteGpuDirectConnectionSwitch_Status", sw("DGPU_DIRECT_CONNECT_TOGGLE"));
    j.Int("CloseTimer", st.closeMin * 60 + st.closeSec).Int("ScreenBrightness", 70);
    j.EndObj();
    return Msg{"Setting/Status", j.Take()};
  }

  std::vector<Msg> SettingControl(const JVal& v, const std::string& action) {
    std::vector<Msg> out;
    if (action == "GETSTATUS") {
      out.push_back(SettingStatus());
      return out;
    }
    if (action == "SetKeyboardCloseTime") {
      const JVal* m = Get(v, "Mins");
      const JVal* s = Get(v, "Seconds");
      st.closeMin = m ? static_cast<int>(m->AsInt()) : 0;
      st.closeSec = s ? static_cast<int>(s->AsInt()) : 0;
      out.push_back(SettingStatus());
      return out;
    }
    // "<STEM>_ON" / "<STEM>_OFF" / "<STEM>_LOCK" / "<STEM>_UNLOCK"
    static const char* suffix[] = {"_ON", "_OFF", "_LOCK", "_UNLOCK"};
    for (int i = 0; i < 4; i++) {
      const std::string sfx = suffix[i];
      if (action.size() > sfx.size() && action.compare(action.size() - sfx.size(), sfx.size(), sfx) == 0) {
        const std::string stem = action.substr(0, action.size() - sfx.size());
        st.sw[stem] = (i == 0 || i == 2) ? 1 : 0;
        ApplySwitch(stem, st.sw[stem] != 0);
        out.push_back(SettingStatus());
        break;
      }
    }
    return out;
  }

  Msg BatteryStatus() {
    JsonOut j;
    j.BeginObj().Int("BatteryPowerStatus", 1).Int("BatteryPercent", 76).Int("HealthProtectionStatus", st.battMode).Int("TypeCAdaptorPrioritySwitch", 0);
    j.Int("TypeCAdaptorPrioritySupport", 0).EndObj();
    return Msg{"System/BatteryProtection", j.Take()};
  }

  std::vector<Msg> Service(const std::string& topic, const std::string& payload) {
    JVal v;
    std::string err;
    std::vector<Msg> none;
    if (!oem::ParseJson(payload, &v, &err) || !v.IsObj()) return none;
    const JVal* a = v.FindI("Action");
    const std::string action = a ? a->AsString() : "";
    if (topic == "Fan/Control") return FanControl(v, action);
    if (topic == "Keyboard/Ctrl") return KbControl(v, action);
    if (topic == "Setting/Control") return SettingControl(v, action);
    if (topic == "BatteryProtection/Control") {
      const std::string act = Upper(action);
      std::vector<Msg> out;
      if (act == "PERFORMANCEDMODE") st.battMode = 0;
      else if (act == "BALANCEDMODE") st.battMode = 1;
      else if (act == "HEALTHYMODE") st.battMode = 2;
      else if (act != "REPORT") return out;
      ApplyBattery();
      out.push_back(BatteryStatus());
      return out;
    }
    if (topic == "Customize/Control" && (action == "GETSUPPORT" || action == "GETSETUPINFO")) {
      JsonOut j;
      j.BeginObj().Int("LightbarType", 0).Int("KeyboardType", 4).Int("ProjectID", 26).Int("SupportRamFan1p5", 1).Int("BatteryProtectionType", 1).Int("SupportColorCalibration", 0).EndObj();
      return {Msg{"Customize/SupportInfo", j.Take()}};
    }
    return none;
  }
};

// ------------------------------------------------------------------------------------------------ transport (client side)
namespace {
class MemTransport : public oem::ITransport {
 public:
  explicit MemTransport(FakeOemWorld::Impl* d) : d_(d) {}
  ~MemTransport() override { Close(); }
  bool Open(const std::string& host, int, int, std::string* why) override {
    if (host != "127.0.0.1" && host != "localhost") {
      if (why) *why = "only the loopback interface is allowed";
      return false;
    }
    c_ = d_->NewConn();
    if (!c_) {
      if (why) *why = "connection refused";
      return false;
    }
    return true;
  }
  bool SendAll(const uint8_t* p, size_t n) override {
    if (!c_ || c_->s2c.Closed() || c_->c2s.Closed()) return false;
    c_->c2s.Push(p, n);
    d_->Poke();
    return true;
  }
  int Recv(uint8_t* p, size_t cap, int timeoutMs) override { return c_ ? c_->s2c.Pop(p, cap, timeoutMs) : -1; }
  void Close() override {
    if (!c_) return;
    c_->c2s.Close();
    c_->s2c.Close();
    d_->Poke();
    c_.reset();
  }

 private:
  FakeOemWorld::Impl* d_;
  std::shared_ptr<Conn> c_;
};
}  // namespace

// ------------------------------------------------------------------------------------------------ public API
FakeOemWorld::FakeOemWorld(Platform* plat) : d_(new Impl) {
  d_->plat = plat;
  {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->ApplyToEc();
  }
  d_->th = std::thread([this] { d_->Run(); });
}

FakeOemWorld::~FakeOemWorld() {
  {
    std::lock_guard<std::mutex> g(d_->mu);
    d_->stop = true;
    d_->cv.notify_all();
  }
  if (d_->th.joinable()) d_->th.join();
}

std::unique_ptr<oem::ITransport> FakeOemWorld::NewTransport() { return std::unique_ptr<oem::ITransport>(new MemTransport(d_.get())); }

oem::LinkConfig FakeOemWorld::LinkConfigFor(const std::vector<std::string>& prefixes) {
  oem::LinkConfig c;
  c.prefixes = prefixes.empty() ? oem::UiPrefixCandidates(0, 0) : prefixes;
  c.reconnectMinMs = 30;
  c.reconnectMaxMs = 120;
  c.transport = [this] { return NewTransport(); };
  return c;
}

void FakeOemWorld::SetServiceUp(bool up) {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->up = up;
  if (!up)
    for (auto& c : d_->conns) Impl::Kill(*c);
  d_->cv.notify_all();
}

void FakeOemWorld::SetAcceptedPrefixes(const std::vector<std::string>& p) {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->accepted = p;
}

void FakeOemWorld::DropConnections() {
  std::lock_guard<std::mutex> g(d_->mu);
  for (auto& c : d_->conns) Impl::Kill(*c);
  d_->cv.notify_all();
}

void FakeOemWorld::SetSilent(bool on) {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->silent = on;
}

void FakeOemWorld::SetEcWritesApply(bool on) {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->ecApply = on;
}

void FakeOemWorld::SetKeyboardBlocks(int n) {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->st.kb.blocks = std::max(1, std::min(16, n));
  d_->st.kb.rgb.assign(3 * static_cast<size_t>(d_->st.kb.blocks), 255);
}

void FakeOemWorld::ExternalKeyboard(const std::string& what, long long value) {
  std::lock_guard<std::mutex> g(d_->mu);
  FakeKb& k = d_->st.kb;
  if (what == "power") k.power = value != 0;
  else if (what == "bright") k.light = static_cast<int>(std::max(0LL, std::min(4LL, value)));
  else if (what == "speed") k.speed = static_cast<int>(std::max(0LL, std::min(4LL, value)));
  else if (what == "effect") k.effect = static_cast<int>(value);
  else if (what == "direction") k.direction = static_cast<int>(value);
  else if (what == "timer") {   // the idle timer lives in Setting/Status, not in the keyboard status
    const int sec = static_cast<int>(std::max(0LL, std::min(3600LL, value)));
    d_->st.closeMin = sec / 60;
    d_->st.closeSec = sec % 60;
    const oem::Msg m = d_->SettingStatus();
    d_->Deliver(m.topic, m.payload);
    return;
  } else if (what == "color") {
    for (int i = 0; i < k.blocks; i++) {
      k.rgb[3 * i] = static_cast<int>((value >> 16) & 255);
      k.rgb[3 * i + 1] = static_cast<int>((value >> 8) & 255);
      k.rgb[3 * i + 2] = static_cast<int>(value & 255);
    }
  } else {
    return;
  }
  const oem::Msg m = d_->KbStatus();
  d_->Deliver(m.topic, m.payload);
}

std::vector<std::string> FakeOemWorld::Received() const {
  std::lock_guard<std::mutex> g(d_->mu);
  return d_->received;
}

void FakeOemWorld::ClearReceived() {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->received.clear();
}

int FakeOemWorld::Connections() const {
  std::lock_guard<std::mutex> g(d_->mu);
  int n = 0;
  for (auto& c : d_->conns)
    if (c->authed && !c->dead) n++;
  return n;
}

int FakeOemWorld::Accepted() const {
  std::lock_guard<std::mutex> g(d_->mu);
  return d_->accepts;
}

int FakeOemWorld::Refused() const {
  std::lock_guard<std::mutex> g(d_->mu);
  return d_->refusals;
}

std::string FakeOemWorld::LastClientId() const {
  std::lock_guard<std::mutex> g(d_->mu);
  return d_->lastClientId;
}

FakeServiceState FakeOemWorld::State() const {
  std::lock_guard<std::mutex> g(d_->mu);
  return d_->st;
}

void FakeOemWorld::SetState(const FakeServiceState& s) {
  std::lock_guard<std::mutex> g(d_->mu);
  d_->st = s;
  d_->ApplyToEc();
}

}  // namespace sim
}  // namespace bc
