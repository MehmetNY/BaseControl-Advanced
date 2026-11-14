// Named-pipe server hosted by the SYSTEM service (or by --local). Windows only.
//  * FILE_FLAG_FIRST_PIPE_INSTANCE on the first instance: a squatter that created the name earlier is detected
//  * PIPE_REJECT_REMOTE_CLIENTS: local callers only
//  * ACL: SYSTEM + Administrators full control, Authenticated Users read/write (the UI is not elevated). With the
//    "admin only" safety option the server additionally refuses set/cmd requests from non-administrators.
//  * strict decoder, typed settings only, per-connection rate limiting, bounded message sizes
#pragma once
#include <atomic>
#include <list>
#include <mutex>
#include <thread>

#include "../hw/winutil.h"
#include "protocol.h"

namespace bc {

class PipeServer {
 public:
  PipeServer(Controller& c, mgp::ILog& log, std::string mode, std::string version);
  ~PipeServer();
  bool Start(std::string* why);
  void Stop();
  int Clients() const { return clients_.load(); }

 private:
  struct Conn;
  void AcceptLoop();
  void ClientLoop(HANDLE pipe);
  bool ReadMessage(HANDLE pipe, std::string* out);
  bool WriteMessage(HANDLE pipe, const std::string& msg);
  std::string Handle(Conn& cn, const proto::Frame& f, bool* close);
  void UpdateInterest();
  bool ClientIsAdmin(HANDLE pipe);

  Controller& c_;
  mgp::ILog& log_;
  std::string mode_, version_;
  std::atomic<bool> stop_{false};
  std::atomic<int> clients_{0};
  HANDLE stopEv_ = nullptr;
  std::thread accept_;
  std::mutex mu_;
  std::list<std::thread> workers_;
  std::list<Conn*> conns_;
  bool started_ = false;
};

}  // namespace bc
