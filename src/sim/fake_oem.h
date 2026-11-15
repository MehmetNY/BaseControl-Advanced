// A stand-in for the vendor's Control Center services, entirely in memory: an MQTT 3.1.1 broker that checks client identities the way the
// real loopback broker does, and a "service" behind it that answers the control topics, keeps the profile / keyboard / battery state and
// moves the simulated EC where the real service would. It exists so that the whole OEM link (session, parsing, requests, verification by EC
// read-back) is exercised by unit tests, the simulator window and the Wine UI flows without any vendor software.
//
// It is NOT evidence about the real service: every field and every reaction here comes from the protocol notes (docs/PROTOCOL.md) and from
// assumptions written down next to the code.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/fan.h"
#include "../oem/link.h"
#include "../oem/mqtt.h"
#include "sim.h"

namespace bc {
namespace sim {

struct FakeDetail {
  int oc = 1;                       // OverClockingSwitch ("Enable Automatic GPU Overboost"); when on the *OC offsets below are the ones in use
  int pl1 = 120, pl2 = 120, pl4 = 200;
  int fanSwEn = 0, fanSw = 20;      // FanSwitchSpeedEnabled / FanSwitchSpeed
  int tccSw = 0, tcc = 5;           // CpuTccOffsetSwitch / CpuTccOffset
  int gCore = 0, gMem = 0, gTemp = 87;
  int dynBoost = 0;
  int gCoreOc = 0, gMemOc = 0;
};

struct FakeKb {
  int power = 1, mode = 1, effect = 0, light = 3, speed = 2, direction = -1;
  int blocks = 8;
  std::vector<int> rgb;             // 3 values per block
};

struct FakeServiceState {
  int mode = 1;                     // 0 office, 1 gaming, 2 turbo, 3 custom (the vendor's OperatingMode numbering)
  int profile[4] = {0, 0, 0, 0};
  bool boost = false, fanCustom = false;
  bool detailAnyMode = false;       // false: SET_OPERATING_MODE_DETAIL only reaches the EC while the custom profile is active (assumed)
  FakeDetail detail[4];
  FakeKb kb;
  int battMode = 1;                 // 0 high capacity, 1 balanced, 2 eco
  int closeMin = 0, closeSec = 30;
  std::map<std::string, int> sw;    // Setting/Control switches by action stem ("WIFI", "WINKEY" ...): 1 = the ON / LOCK action was sent last
  bool swInverted = false;          // true: the switches act the other way round in the EC (ON/LOCK clears the bit); the polarity is unverified on the real model
  std::string tableName = "M1T1";
  fan::TableImage cpuTable, gpuTable;
  int serviceEcWrites = 0;          // times the service changed the simulated EC (counted apart from the app's own writes)
};

class FakeOemWorld {
 public:
  explicit FakeOemWorld(Platform* plat);     // plat may be null: protocol-only tests
  ~FakeOemWorld();
  FakeOemWorld(const FakeOemWorld&) = delete;
  FakeOemWorld& operator=(const FakeOemWorld&) = delete;

  // client side: a transport whose Open() connects to the in-memory broker (LinkConfig::transport)
  std::unique_ptr<oem::ITransport> NewTransport();
  oem::LinkConfig LinkConfigFor(const std::vector<std::string>& prefixes = {});   // transport + short timings for tests / the simulator

  // ---- scenario controls
  void SetServiceUp(bool up);                               // false: connecting fails (the vendor service is not running)
  void SetAcceptedPrefixes(const std::vector<std::string>& p);
  void DropConnections();                                   // the broker restarts: every session ends
  void SetSilent(bool on);                                  // the broker works but the service does not answer
  void SetEcWritesApply(bool on);                           // false: the service "accepts" detail changes but the EC never changes
  void SetKeyboardBlocks(int n);
  // Another program changes the keyboard light behind the app's back (the vendor's own window, say): the service publishes the new
  // status to every client. what = power | bright | speed | effect | direction | color (value = 0xRRGGBB, every block) | timer (seconds).
  void ExternalKeyboard(const std::string& what, long long value);

  // ---- introspection
  std::vector<std::string> Received() const;                // "topic payload" of every control message the service got
  void ClearReceived();
  int Connections() const;
  int Accepted() const;
  int Refused() const;
  std::string LastClientId() const;
  FakeServiceState State() const;
  void SetState(const FakeServiceState& s);                 // test setup; re-applies the profile to the EC

 public:
  struct Impl;   // public only so the in-memory transport (an implementation detail of the .cpp) can name it
 private:
  std::unique_ptr<Impl> d_;
};

}  // namespace sim
}  // namespace bc
