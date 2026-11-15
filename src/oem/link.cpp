#include "link.h"

#include <algorithm>
#include <chrono>

#include "../common/util.h"
#include "json.h"

namespace bc {
namespace oem {

namespace {
uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::string Clip(const std::string& s, size_t n) {
  std::string o;
  for (char c : s) {
    if (o.size() >= n) { o += "..."; break; }
    o += (static_cast<unsigned char>(c) < 0x20 && c != '\t') ? ' ' : c;
  }
  return o;
}
}  // namespace

bool IngestStatus(const std::string& topic, const std::string& payload, std::map<std::string, KV>* states, std::string* err) {
  const std::string stem = StateStem(topic);
  if (stem.empty()) return false;
  JVal v;
  std::string why;
  if (!ParseJson(payload, &v, &why)) {
    if (err) *err = topic + ": " + why;
    return false;
  }
  KV out;
  Flatten(v, stem, &out);   // keys come out as "oem.fan.CPU_PL1"; a bare scalar lands on the stem itself
  KV& cur = (*states)[stem];
  // The service sends whole objects, so the new message replaces the old one; equal content is not a change.
  if (cur.All() == out.All()) return false;
  cur = out;
  return true;
}

MqttOemLink::MqttOemLink(const LinkConfig& cfg) : cfg_(cfg) {
  if (cfg_.prefixes.empty()) cfg_.prefixes = UiPrefixCandidates(0, 0);
}

MqttOemLink::~MqttOemLink() { Stop(); }

void MqttOemLink::Start() {
  if (started_) return;
  started_ = true;
  stop_ = false;
  th_ = std::thread([this] { Main(); });
}

void MqttOemLink::Stop() {
  if (!started_) return;
  stop_ = true;
  cv_.notify_all();
  if (th_.joinable()) th_.join();
  started_ = false;
  connected_ = false;
}

void MqttOemLink::Note(const std::string& line) {
  std::lock_guard<std::mutex> g(mu_);
  trace_.push_back(Fmt("%llu %s", static_cast<unsigned long long>(NowMs() % 1000000), line.c_str()));
  while (trace_.size() > 240) trace_.pop_front();
}

void MqttOemLink::SetState(const char* st, const std::string& detail) {
  std::lock_guard<std::mutex> g(mu_);
  state_ = st;
  detail_ = detail;
}

KV MqttOemLink::State() const {
  std::lock_guard<std::mutex> g(mu_);
  KV all;
  for (const auto& p : states_) all.Merge(p.second);
  return all;
}

KV MqttOemLink::Info() const {
  std::lock_guard<std::mutex> g(mu_);
  KV k;
  k.Set("oem.link.state", state_);
  k.Set("oem.link.detail", detail_);
  k.Set("oem.link.endpoint", Fmt("%s:%d", cfg_.host.c_str(), cfg_.port));
  k.SetI("oem.link.slot", cfg_.slot);
  k.Set("oem.link.class", acceptedPrefix_);
  k.SetI("oem.link.rx", static_cast<long long>(rx_.load()));
  k.SetI("oem.link.tx", static_cast<long long>(tx_.load()));
  k.SetI("oem.link.sessions", static_cast<long long>(sessions_.load()));
  k.SetB("oem.link.capture", capture_);
  return k;
}

std::vector<std::string> MqttOemLink::Trace(size_t n) const {
  std::lock_guard<std::mutex> g(mu_);
  const size_t from = trace_.size() > n ? trace_.size() - n : 0;
  return std::vector<std::string>(trace_.begin() + static_cast<long>(from), trace_.end());
}

void MqttOemLink::SetCapture(bool on) {
  if (capture_ == on) return;
  capture_ = on;
  cv_.notify_all();   // the session loop notices and (un)subscribes
}

bool MqttOemLink::SendRaw(const std::vector<uint8_t>& bytes) {
  std::lock_guard<std::mutex> g(sendMu_);
  if (!tp_) return false;
  if (!tp_->SendAll(bytes.data(), bytes.size())) return false;
  lastTx_ = NowMs();
  tx_++;
  return true;
}

bool MqttOemLink::Publish(const Msg& m) {
  if (!connected_) return false;
  Note("TX " + m.topic + " " + Clip(m.payload, 200));
  return SendRaw(EncodePublish(m.topic, m.payload, 0, false, 0));
}

void MqttOemLink::RequestAll() {
  wantRefresh_ = true;
  cv_.notify_all();
}

void MqttOemLink::SleepInterruptible(uint32_t ms) {
  std::unique_lock<std::mutex> lk(mu_);
  cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return stop_.load(); });
}

// ------------------------------------------------------------------------------------------------ connection
void MqttOemLink::ResetTransport() {
  std::lock_guard<std::mutex> g(sendMu_);
  if (tp_) tp_->Close();
  tp_.reset();
}

bool MqttOemLink::ConnectOnce(std::string* why) {
  bool refused = false;
  for (size_t attempt = 0; attempt < cfg_.prefixes.size() && !stop_; attempt++) {
    const size_t idx = (prefixIdx_ + attempt) % cfg_.prefixes.size();
    const Identity id = MakeIdentity(cfg_.prefixes[idx], cfg_.slot);
    {
      std::lock_guard<std::mutex> g(sendMu_);
      tp_ = cfg_.transport ? cfg_.transport() : MakeTcpTransport();
    }
    std::string err;
    if (!tp_->Open(cfg_.host, cfg_.port, 1500, &err)) {
      *why = err.empty() ? "nothing is listening on the Control Center broker port" : err;
      ResetTransport();
      return false;   // nothing listens: another client class will not help
    }
    const std::vector<uint8_t> c = EncodeConnect(id.clientId, id.user, id.pass, static_cast<uint16_t>(cfg_.keepAliveSec), true);
    if (!SendRaw(c)) {
      *why = "could not send the connect request";
      ResetTransport();
      return false;
    }
    PacketReader rd;
    const uint64_t t0 = NowMs();
    int rc = -1;
    bool gotAck = false, broken = false;
    while (!stop_ && !broken && NowMs() - t0 < 3000 && !gotAck) {
      uint8_t buf[512];
      const int n = tp_->Recv(buf, sizeof buf, 100);
      if (n < 0) break;
      if (n > 0) rd.Feed(buf, static_cast<size_t>(n));
      Packet p;
      PacketReader::Status st;
      while ((st = rd.Next(&p)) == PacketReader::Got) {
        if (p.type == kConnack) {
          bool sp = false;
          if (DecodeConnack(p, &rc, &sp)) gotAck = true;
          break;
        }
      }
      if (st == PacketReader::Bad) broken = true;
    }
    if (!gotAck) {
      *why = "the broker did not answer the connect request";
      ResetTransport();
      return false;
    }
    if (rc == 0) {
      prefixIdx_ = idx;
      std::lock_guard<std::mutex> g(mu_);
      acceptedPrefix_ = id.prefix;
      return true;
    }
    refused = true;
    *why = Fmt("the broker refused client class %s (code %d)", id.prefix.c_str(), rc);
    Note(*why);
    ResetTransport();
    if (rc != 4 && rc != 5) return false;   // only "bad credentials" / "not authorised" are worth another class
  }
  if (refused) *why += "; no other client class was accepted";
  return false;
}

void MqttOemLink::Main() {
  uint32_t backoff = cfg_.reconnectMinMs;
  while (!stop_) {
    SetState("connecting", "");
    std::string why;
    if (!ConnectOnce(&why)) {
      SetState(why.find("refused client class") != std::string::npos ? "refused" : "offline", why);
      if (!why.empty()) Note("connect: " + why);
      SleepInterruptible(backoff);
      backoff = std::min<uint32_t>(cfg_.reconnectMaxMs, backoff * 2);
      continue;
    }
    backoff = cfg_.reconnectMinMs;
    Session();
    connected_ = false;
    {
      std::lock_guard<std::mutex> g(sendMu_);
      if (tp_) {
        const std::vector<uint8_t> d = EncodeDisconnect();
        tp_->SendAll(d.data(), d.size());
      }
    }
    ResetTransport();
    if (!stop_) {
      SetState("offline", "the connection to the OEM service was lost");
      Note("session ended");
      SleepInterruptible(cfg_.reconnectMinMs);
    }
  }
  SetState("offline", "stopped");
}

void MqttOemLink::Handle(const PublishMsg& m) {
  rx_++;
  const std::string stem = StateStem(m.topic);
  if (stem.empty()) {
    if (capture_) Note("RX " + m.topic + " " + Clip(m.payload, 200));
    return;
  }
  Note("RX " + m.topic + Fmt(" (%zu bytes)", m.payload.size()));
  std::string err;
  bool changed;
  {
    std::lock_guard<std::mutex> g(mu_);
    changed = IngestStatus(m.topic, m.payload, &states_, &err);
    if (changed) version_++;
  }
  if (!err.empty()) Note("bad message: " + err);
  if (changed) cv_.notify_all();
}

bool MqttOemLink::WaitChange(uint64_t since, uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lk(mu_);
  return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return version_.load() > since || stop_.load(); }) && version_.load() > since;
}

void MqttOemLink::Session() {
  PacketReader rd;
  uint64_t now = NowMs();
  lastRx_ = lastPing_ = now;
  lastTx_ = now;
  bool awaitingPong = false;
  captureApplied_ = false;
  sessions_++;
  // subscribe first, then ask for everything: the answers come back on the topics we are already listening to
  if (!SendRaw(EncodeSubscribe(nextId_++, StatusFilters(), 0))) return;
  connected_ = true;
  SetState("connected", "");
  Note("connected as " + MakeIdentity(cfg_.prefixes[prefixIdx_], cfg_.slot).clientId);
  wantRefresh_ = true;
  requestTries_ = 0;
  lastRequest_ = 0;

  while (!stop_) {
    // 1. diagnostics subscription follows the toggle
    if (capture_ != captureApplied_) {
      if (capture_) {
        if (!SendRaw(EncodeSubscribe(nextId_++, {kCaptureFilter}, 0))) return;
        Note("capture on");
      } else {
        Note("capture off (takes effect at the next connection)");
      }
      captureApplied_ = capture_.load();
    }
    // 2. status requests: right after connecting, whenever asked, again while the service has not answered, and once a minute
    now = NowMs();
    bool haveFan;
    {
      std::lock_guard<std::mutex> g(mu_);
      haveFan = states_.count("oem.fan") != 0;
    }
    const bool due = wantRefresh_ || (!haveFan && now - lastRequest_ >= (requestTries_ < 12 ? 4000u : 30000u)) || (haveFan && now - lastRequest_ >= 60000);
    if (due) {
      wantRefresh_ = false;
      lastRequest_ = now;
      requestTries_++;
      const Msg reqs[] = {msg::FanGetStatus(), msg::FanGetTable(), msg::KbGetStatus(), msg::SettingGetStatus(), msg::CustomizeGetSupport(), msg::BatteryReport()};
      for (const Msg& r : reqs)
        if (!SendRaw(EncodePublish(r.topic, r.payload, 0, false, 0))) return;
      Note("status requested");
    }
    // 3. incoming
    uint8_t buf[4096];
    const int n = tp_->Recv(buf, sizeof buf, 200);
    if (n < 0) return;
    now = NowMs();
    if (n > 0) {
      lastRx_ = now;
      rd.Feed(buf, static_cast<size_t>(n));
      Packet p;
      PacketReader::Status st;
      while ((st = rd.Next(&p)) == PacketReader::Got) {
        if (p.type == kPublish) {
          PublishMsg pm;
          if (!DecodePublish(p, &pm)) continue;
          if (pm.qos == 1) SendRaw(EncodeAck(kPuback, pm.id));
          else if (pm.qos == 2) SendRaw(EncodeAck(kPubrec, pm.id));
          Handle(pm);
        } else if (p.type == kPubrel) {
          uint16_t id;
          if (DecodeAckId(p, &id)) SendRaw(EncodeAck(kPubcomp, id));
        } else if (p.type == kPingresp) {
          awaitingPong = false;
        } else if (p.type == kDisconnect) {
          return;
        }
      }
      if (st == PacketReader::Bad) {
        Note("broken stream from the broker");
        return;
      }
    }
    // 4. keep-alive
    const uint64_t ka = static_cast<uint64_t>(cfg_.keepAliveSec) * 1000;
    if (awaitingPong && now - lastPing_ > ka) {
      Note("keep-alive timed out");
      return;
    }
    if (!awaitingPong && now - std::max<uint64_t>(lastTx_.load(), lastRx_) >= ka / 2) {
      if (!SendRaw(EncodePing())) return;
      lastPing_ = now;
      awaitingPong = true;
    }
  }
}

}  // namespace oem
}  // namespace bc
