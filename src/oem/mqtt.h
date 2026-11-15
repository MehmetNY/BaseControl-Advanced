// MQTT 3.1.1 client primitives for the link to the OEM service (the vendor's broker listens on the loopback interface only).
// This file is the wire format: packet encoders, an incremental packet reader and the topic-filter matcher. The session logic lives in
// link.cpp; the socket sits behind ITransport so the same code runs against a fake broker in tests and in the simulator.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bc {
namespace oem {

class ITransport {
 public:
  virtual ~ITransport() = default;
  virtual bool Open(const std::string& host, int port, int timeoutMs, std::string* why) = 0;
  virtual bool SendAll(const uint8_t* p, size_t n) = 0;                 // false = the connection is gone
  virtual int Recv(uint8_t* p, size_t cap, int timeoutMs) = 0;          // > 0 bytes, 0 = nothing within the timeout, < 0 = closed / error
  virtual void Close() = 0;
};
std::unique_ptr<ITransport> MakeTcpTransport();   // Winsock on Windows, BSD sockets elsewhere (tests, simulator)

enum PacketType : uint8_t {
  kConnect = 1, kConnack = 2, kPublish = 3, kPuback = 4, kPubrec = 5, kPubrel = 6, kPubcomp = 7, kSubscribe = 8, kSuback = 9,
  kUnsubscribe = 10, kUnsuback = 11, kPingreq = 12, kPingresp = 13, kDisconnect = 14
};

inline constexpr size_t kMaxPacket = 2u << 20;   // anything larger is treated as a broken stream (status messages are a few KB)

std::vector<uint8_t> EncodeConnect(const std::string& clientId, const std::string& user, const std::string& pass, uint16_t keepAliveSec, bool cleanSession);
std::vector<uint8_t> EncodeSubscribe(uint16_t id, const std::vector<std::string>& filters, uint8_t qos);
std::vector<uint8_t> EncodePublish(const std::string& topic, const std::string& payload, uint8_t qos = 0, bool retain = false, uint16_t id = 0);
std::vector<uint8_t> EncodeAck(PacketType t, uint16_t id);   // PUBACK / PUBREC / PUBREL / PUBCOMP
std::vector<uint8_t> EncodePing();
std::vector<uint8_t> EncodeDisconnect();

struct Packet {
  uint8_t type = 0;
  uint8_t flags = 0;
  std::vector<uint8_t> body;
};

// Bytes in, whole packets out. A malformed length (more than 4 bytes) or a packet above kMaxPacket makes the stream Bad for good.
class PacketReader {
 public:
  enum Status { NeedMore, Got, Bad };
  void Feed(const uint8_t* p, size_t n);
  Status Next(Packet* out);
  void Reset();

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool bad_ = false;
};

struct PublishMsg {
  std::string topic;
  std::string payload;
  uint8_t qos = 0;
  bool retain = false;
  bool dup = false;
  uint16_t id = 0;
};
bool DecodePublish(const Packet& p, PublishMsg* out);
bool DecodeConnack(const Packet& p, int* returnCode, bool* sessionPresent);
bool DecodeSuback(const Packet& p, uint16_t* id, std::vector<uint8_t>* codes);
bool DecodeAckId(const Packet& p, uint16_t* id);

// MQTT topic filter match ("+" one level, "#" the rest). Topics starting with '$' are not matched by a leading wildcard.
bool TopicMatches(const std::string& filter, const std::string& topic);

}  // namespace oem
}  // namespace bc
