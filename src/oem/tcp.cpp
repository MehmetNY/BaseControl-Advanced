// Loopback TCP transport for the OEM service link: Winsock on Windows, BSD sockets on every other host (unit tests, simulator).
#include <cstring>
#include <string>

#include "mqtt.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace bc {
namespace oem {
namespace {

#ifdef _WIN32
using Sock = SOCKET;
constexpr Sock kBad = INVALID_SOCKET;
void EnsureNet() {
  static const bool once = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  (void)once;
}
void CloseSock(Sock s) { closesocket(s); }
void SetNonBlocking(Sock s, bool nb) {
  u_long m = nb ? 1 : 0;
  ioctlsocket(s, FIONBIO, &m);
}
bool WouldBlock() {
  const int e = WSAGetLastError();
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
int WaitFd(Sock s, bool write, int timeoutMs) {
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(s, &fds);
  timeval tv;
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  return select(0, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &tv);
}
#else
using Sock = int;
constexpr Sock kBad = -1;
void EnsureNet() {}
void CloseSock(Sock s) { ::close(s); }
void SetNonBlocking(Sock s, bool nb) {
  int f = fcntl(s, F_GETFL, 0);
  fcntl(s, F_SETFL, nb ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
}
bool WouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
int WaitFd(Sock s, bool write, int timeoutMs) {
  pollfd p;
  p.fd = s;
  p.events = write ? POLLOUT : POLLIN;
  p.revents = 0;
  int r = poll(&p, 1, timeoutMs);
  return r;
}
#endif

class Tcp : public ITransport {
 public:
  ~Tcp() override { Close(); }

  bool Open(const std::string& host, int port, int timeoutMs, std::string* why) override {
    Close();
    EnsureNet();
    // Only the loopback interface is ever contacted: the broker of the OEM service does not accept anything else.
    if (host != "127.0.0.1" && host != "localhost") {
      if (why) *why = "refusing to connect to a non-loopback address";
      return false;
    }
    sockaddr_in a;
    std::memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = htonl(0x7F000001u);
    Sock s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBad) {
      if (why) *why = "socket() failed";
      return false;
    }
    SetNonBlocking(s, true);
    int r = ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    if (r != 0 && !WouldBlock()) {
      CloseSock(s);
      if (why) *why = "connection refused (the OEM service is not listening)";
      return false;
    }
    if (r != 0) {
      int w = WaitFd(s, true, timeoutMs);
      int soerr = 0;
#ifdef _WIN32
      int len = sizeof soerr;
      getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
#else
      socklen_t len = sizeof soerr;
      getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &len);
#endif
      if (w <= 0 || soerr != 0) {
        CloseSock(s);
        if (why) *why = w == 0 ? "connection timed out" : "connection refused (the OEM service is not listening)";
        return false;
      }
    }
    SetNonBlocking(s, false);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    s_ = s;
    return true;
  }

  bool SendAll(const uint8_t* p, size_t n) override {
    if (s_ == kBad) return false;
    size_t off = 0;
    while (off < n) {
#ifdef _WIN32
      int k = ::send(s_, reinterpret_cast<const char*>(p + off), static_cast<int>(n - off), 0);
#else
      ssize_t k = ::send(s_, p + off, n - off, MSG_NOSIGNAL);
#endif
      if (k <= 0) return false;
      off += static_cast<size_t>(k);
    }
    return true;
  }

  int Recv(uint8_t* p, size_t cap, int timeoutMs) override {
    if (s_ == kBad) return -1;
    int w = WaitFd(s_, false, timeoutMs);
    if (w == 0) return 0;
    if (w < 0) return -1;
#ifdef _WIN32
    int k = ::recv(s_, reinterpret_cast<char*>(p), static_cast<int>(cap), 0);
#else
    ssize_t k = ::recv(s_, p, cap, 0);
#endif
    if (k > 0) return static_cast<int>(k);
    return -1;   // 0 = the peer closed in an orderly way
  }

  void Close() override {
    if (s_ != kBad) {
      CloseSock(s_);
      s_ = kBad;
    }
  }

 private:
  Sock s_ = kBad;
};

}  // namespace

std::unique_ptr<ITransport> MakeTcpTransport() { return std::unique_ptr<ITransport>(new Tcp); }

}  // namespace oem
}  // namespace bc
