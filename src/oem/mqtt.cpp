#include "mqtt.h"

#include <cstring>

namespace bc {
namespace oem {

namespace {
void PutU16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x & 0xFF));
}
void PutStr(std::vector<uint8_t>& v, const std::string& s) {
  const size_t n = s.size() > 0xFFFF ? 0xFFFF : s.size();
  PutU16(v, static_cast<uint16_t>(n));
  v.insert(v.end(), s.begin(), s.begin() + static_cast<long>(n));
}
void PutLen(std::vector<uint8_t>& v, size_t len) {
  do {
    uint8_t b = static_cast<uint8_t>(len % 128);
    len /= 128;
    if (len) b |= 0x80;
    v.push_back(b);
  } while (len);
}
std::vector<uint8_t> Frame(uint8_t first, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> out;
  out.reserve(body.size() + 5);
  out.push_back(first);
  PutLen(out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}
bool GetU16(const std::vector<uint8_t>& b, size_t* pos, uint16_t* v) {
  if (*pos + 2 > b.size()) return false;
  *v = static_cast<uint16_t>((b[*pos] << 8) | b[*pos + 1]);
  *pos += 2;
  return true;
}
bool GetStr(const std::vector<uint8_t>& b, size_t* pos, std::string* s) {
  uint16_t n;
  if (!GetU16(b, pos, &n) || *pos + n > b.size()) return false;
  s->assign(reinterpret_cast<const char*>(b.data() + *pos), n);
  *pos += n;
  return true;
}
}  // namespace

std::vector<uint8_t> EncodeConnect(const std::string& clientId, const std::string& user, const std::string& pass, uint16_t keepAliveSec, bool cleanSession) {
  std::vector<uint8_t> b;
  PutStr(b, "MQTT");
  b.push_back(4);   // protocol level 3.1.1
  uint8_t flags = cleanSession ? 0x02 : 0x00;
  if (!user.empty()) flags |= 0x80;
  if (!pass.empty()) flags |= 0x40;
  b.push_back(flags);
  PutU16(b, keepAliveSec);
  PutStr(b, clientId);
  if (!user.empty()) PutStr(b, user);
  if (!pass.empty()) PutStr(b, pass);
  return Frame(static_cast<uint8_t>(kConnect << 4), b);
}

std::vector<uint8_t> EncodeSubscribe(uint16_t id, const std::vector<std::string>& filters, uint8_t qos) {
  std::vector<uint8_t> b;
  PutU16(b, id);
  for (const std::string& f : filters) {
    PutStr(b, f);
    b.push_back(qos & 3);
  }
  return Frame(static_cast<uint8_t>((kSubscribe << 4) | 0x02), b);
}

std::vector<uint8_t> EncodePublish(const std::string& topic, const std::string& payload, uint8_t qos, bool retain, uint16_t id) {
  std::vector<uint8_t> b;
  PutStr(b, topic);
  if (qos > 0) PutU16(b, id);
  b.insert(b.end(), payload.begin(), payload.end());
  return Frame(static_cast<uint8_t>((kPublish << 4) | ((qos & 3) << 1) | (retain ? 1 : 0)), b);
}

std::vector<uint8_t> EncodeAck(PacketType t, uint16_t id) {
  std::vector<uint8_t> b;
  PutU16(b, id);
  return Frame(static_cast<uint8_t>((t << 4) | (t == kPubrel ? 0x02 : 0x00)), b);
}

std::vector<uint8_t> EncodePing() { return Frame(static_cast<uint8_t>(kPingreq << 4), {}); }
std::vector<uint8_t> EncodeDisconnect() { return Frame(static_cast<uint8_t>(kDisconnect << 4), {}); }

// ------------------------------------------------------------------------------------------------ reader
void PacketReader::Feed(const uint8_t* p, size_t n) {
  if (bad_) return;
  buf_.insert(buf_.end(), p, p + n);
}

void PacketReader::Reset() {
  buf_.clear();
  pos_ = 0;
  bad_ = false;
}

PacketReader::Status PacketReader::Next(Packet* out) {
  if (bad_) return Bad;
  const size_t avail = buf_.size() - pos_;
  if (avail < 2) return NeedMore;
  size_t i = pos_ + 1;
  size_t mult = 1, len = 0;
  int digits = 0;
  for (;;) {
    if (i >= buf_.size()) return NeedMore;
    const uint8_t d = buf_[i++];
    len += static_cast<size_t>(d & 0x7F) * mult;
    digits++;
    if (!(d & 0x80)) break;
    if (digits >= 4) {
      bad_ = true;
      return Bad;
    }
    mult *= 128;
  }
  if (len > kMaxPacket) {
    bad_ = true;
    return Bad;
  }
  if (buf_.size() - i < len) return NeedMore;
  out->type = static_cast<uint8_t>(buf_[pos_] >> 4);
  out->flags = static_cast<uint8_t>(buf_[pos_] & 0x0F);
  out->body.assign(buf_.begin() + static_cast<long>(i), buf_.begin() + static_cast<long>(i + len));
  pos_ = i + len;
  if (pos_ > 65536 && pos_ * 2 > buf_.size()) {   // compact now and then so a long-lived connection does not keep growing
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(pos_));
    pos_ = 0;
  } else if (pos_ == buf_.size()) {
    buf_.clear();
    pos_ = 0;
  }
  return Got;
}

// ------------------------------------------------------------------------------------------------ decoders
bool DecodePublish(const Packet& p, PublishMsg* out) {
  if (p.type != kPublish) return false;
  size_t pos = 0;
  PublishMsg m;
  m.qos = static_cast<uint8_t>((p.flags >> 1) & 3);
  m.retain = (p.flags & 1) != 0;
  m.dup = (p.flags & 8) != 0;
  if (m.qos == 3) return false;
  if (!GetStr(p.body, &pos, &m.topic)) return false;
  if (m.qos > 0 && !GetU16(p.body, &pos, &m.id)) return false;
  m.payload.assign(reinterpret_cast<const char*>(p.body.data() + pos), p.body.size() - pos);
  *out = std::move(m);
  return true;
}

bool DecodeConnack(const Packet& p, int* rc, bool* present) {
  if (p.type != kConnack || p.body.size() < 2) return false;
  if (present) *present = (p.body[0] & 1) != 0;
  if (rc) *rc = p.body[1];
  return true;
}

bool DecodeSuback(const Packet& p, uint16_t* id, std::vector<uint8_t>* codes) {
  if (p.type != kSuback || p.body.size() < 2) return false;
  size_t pos = 0;
  if (!GetU16(p.body, &pos, id)) return false;
  codes->assign(p.body.begin() + static_cast<long>(pos), p.body.end());
  return true;
}

bool DecodeAckId(const Packet& p, uint16_t* id) {
  size_t pos = 0;
  return GetU16(p.body, &pos, id);
}

// ------------------------------------------------------------------------------------------------ topic filters
bool TopicMatches(const std::string& filter, const std::string& topic) {
  if (filter.empty() || topic.empty()) return false;
  if (topic[0] == '$' && (filter[0] == '#' || filter[0] == '+')) return false;
  size_t fi = 0, ti = 0;
  for (;;) {
    size_t fe = filter.find('/', fi);
    const std::string fl = filter.substr(fi, fe == std::string::npos ? std::string::npos : fe - fi);
    if (fl == "#") return fe == std::string::npos;   // '#' must be last
    size_t te = topic.find('/', ti);
    const std::string tl = topic.substr(ti, te == std::string::npos ? std::string::npos : te - ti);
    if (fl != "+" && fl != tl) return false;
    if (fe == std::string::npos || te == std::string::npos) {
      if (fe == std::string::npos && te == std::string::npos) return true;
      // "a/b/#" also matches "a/b"
      if (te == std::string::npos && fe != std::string::npos) return filter.substr(fe + 1) == "#";
      return false;
    }
    fi = fe + 1;
    ti = te + 1;
  }
}

}  // namespace oem
}  // namespace bc
