// The connection to the OEM Control Center service. IOemLink is what the controller sees; MqttOemLink is the real thing (a thread that
// keeps one MQTT session to the loopback broker alive, remembers the last status message of every topic and sends commands);
// src/sim has an in-memory twin for tests and the simulator.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/kv.h"
#include "mqtt.h"
#include "proto.h"

namespace bc {
namespace oem {

class IOemLink {
 public:
  virtual ~IOemLink() = default;
  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual bool Connected() const = 0;
  // Queues one command. false = there is no session right now and nothing was queued (the caller reports that, it does not retry blindly).
  virtual bool Publish(const Msg& m) = 0;
  // Asks every domain for its status again (after a reconnect, or when the UI wants fresh data).
  virtual void RequestAll() = 0;
  virtual uint64_t Version() const = 0;     // grows whenever a status message changed the state
  // Waits in real time (the answer comes from another thread, so a virtual clock cannot stand in) until Version() is above `since` or
  // the time is up. true = the state changed.
  virtual bool WaitChange(uint64_t since, uint32_t timeoutMs) = 0;
  virtual KV State() const = 0;             // flattened last status of every topic we keep: "oem.fan.CPU_PL1" = "120" ...
  virtual KV Info() const = 0;              // link facts for the Service page (no credentials)
  virtual std::vector<std::string> Trace(size_t n) const = 0;
  virtual void SetCapture(bool on) = 0;     // diagnostics: also record every other message on the broker
};

// Parses one status message and stores it under its snapshot stem; shared by the real link and the in-memory twin.
// Returns true when `states` changed. A message that is not valid JSON is ignored (err says why).
bool IngestStatus(const std::string& topic, const std::string& payload, std::map<std::string, KV>* states, std::string* err);

struct LinkConfig {
  std::string host = "127.0.0.1";
  int port = kBrokerPort;
  std::vector<std::string> prefixes;                   // client classes to try, first one preferred
  int slot = kOurSlot;
  int keepAliveSec = 30;
  uint32_t reconnectMinMs = 1000, reconnectMaxMs = 15000;
  std::function<std::unique_ptr<ITransport>()> transport;   // default: MakeTcpTransport
};

class MqttOemLink : public IOemLink {
 public:
  explicit MqttOemLink(const LinkConfig& cfg);
  ~MqttOemLink() override;
  void Start() override;
  void Stop() override;
  bool Connected() const override { return connected_; }
  bool Publish(const Msg& m) override;
  void RequestAll() override;
  uint64_t Version() const override { return version_; }
  bool WaitChange(uint64_t since, uint32_t timeoutMs) override;
  KV State() const override;
  KV Info() const override;
  std::vector<std::string> Trace(size_t n) const override;
  void SetCapture(bool on) override;

 private:
  void Main();
  bool ConnectOnce(std::string* why);
  void Session();
  void SleepInterruptible(uint32_t ms);
  void Note(const std::string& line);          // trace ring (caller must not hold mu_)
  void SetState(const char* st, const std::string& detail);
  void Handle(const PublishMsg& m);
  void ResetTransport();
  bool SendRaw(const std::vector<uint8_t>& bytes);   // serialised: commands come from other threads than the session loop

  LinkConfig cfg_;
  std::unique_ptr<ITransport> tp_;
  std::thread th_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint64_t> version_{0};
  std::atomic<bool> capture_{false};
  std::atomic<bool> captureApplied_{false};
  std::atomic<bool> wantRefresh_{false};
  mutable std::mutex mu_;
  std::mutex sendMu_;                          // guards tp_ against concurrent send/reset (never held while waiting for data)
  std::condition_variable cv_;
  std::map<std::string, KV> states_;
  std::deque<std::string> trace_;
  std::string state_ = "offline", detail_, acceptedPrefix_;
  std::atomic<uint64_t> rx_{0}, tx_{0}, sessions_{0};
  size_t prefixIdx_ = 0;
  uint16_t nextId_ = 1;
  std::atomic<uint64_t> lastTx_{0};
  uint64_t lastRx_ = 0, lastPing_ = 0, lastRequest_ = 0;
  int requestTries_ = 0;
  bool started_ = false;
};

}  // namespace oem
}  // namespace bc
