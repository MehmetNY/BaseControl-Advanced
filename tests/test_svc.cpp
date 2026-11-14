// Wire protocol + backend routing (portable parts of the service layer; the pipe itself is Windows-only).
#include <string>

#include <unistd.h>

#include "../src/common/controller.h"
#include "../src/sim/sim.h"
#include "../src/svc/backend.h"
#include "../src/svc/protocol.h"
#include "mini_test.h"

using namespace bc;

namespace {
Controller::Options ManualOpts(const char* mode) {
  Controller::Options o;
  o.manual = true;
  o.mode = mode;
  return o;
}
}  // namespace

TEST(proto_roundtrip_request_and_result) {
  Request r;
  r.op = "set";
  r.key = "ec.pl1";
  r.value = "95";
  r.args.Set("consent.ctgp_over_oem", "1");
  r.args.Set("note", "line one\nline two = x");
  const std::string wire = proto::Encode("req", 42, proto::RequestToKv(r));
  proto::Frame f;
  CHECK(proto::DecodeRequest(wire, &f));
  CHECK_EQ(f.op, std::string("req"));
  CHECK_EQ(f.id, 42u);
  Request back;
  std::string why;
  CHECK(proto::KvToRequest(f.kv, &back, &why));
  CHECK_EQ(back.op, r.op);
  CHECK_EQ(back.key, r.key);
  CHECK_EQ(back.value, r.value);
  CHECK_EQ(back.args.Str("consent.ctgp_over_oem"), std::string("1"));
  CHECK_EQ(back.args.Str("note"), std::string("line one\nline two = x"));

  Result res;
  res.ok = false;
  res.code = "gate.consent";
  res.msg = "needs consent: x";
  res.data.Set("need", "ctgp_over_oem");
  proto::Frame g;
  CHECK(proto::DecodeResponse(proto::Encode("", 7, proto::ResultToKv(res)), &g));
  Result rb = proto::KvToResult(g.kv);
  CHECK(!rb.ok);
  CHECK_EQ(rb.code, std::string("gate.consent"));
  CHECK_EQ(rb.msg, res.msg);
  CHECK_EQ(rb.data.Str("need"), std::string("ctgp_over_oem"));
}

TEST(proto_decoder_rejects_garbage) {
  proto::Frame f;
  CHECK(!proto::DecodeRequest("", &f));
  CHECK(!proto::DecodeRequest(std::string("_op=req\0x=1", 11), &f));      // NUL byte
  CHECK(!proto::DecodeRequest("_id=1\nkey=x\n", &f));                     // no _op
  CHECK(!proto::DecodeRequest("_op=req\n_id=abc\n", &f));                 // bad id
  CHECK(!proto::DecodeRequest("_op=req\n_id=-3\n", &f));                  // negative id
  CHECK(!proto::DecodeRequest("_op=r e q\n_id=1\n", &f));                 // bad op charset
  CHECK(!proto::DecodeRequest("_op=req\n_id=1\nbad key=1\n", &f));        // bad key charset
  CHECK(!proto::DecodeRequest("_op=req\n_id=1\nk\xC3\xBC=1\n", &f));      // non-ASCII key
  CHECK(!proto::DecodeRequest(std::string(proto::kMaxRequestBytes + 1, 'a'), &f));  // too large
  CHECK(proto::DecodeRequest("_op=hello\n_id=1\nproto=1\n", &f));
  CHECK(!proto::DecodeResponse(std::string(proto::kMaxResponseBytes + 1, 'a'), &f));
}

TEST(proto_request_validation_limits) {
  Request out;
  std::string why;
  auto mk = [](const char* op, const char* key, const std::string& val) {
    KV kv;
    kv.Set("op", op);
    kv.Set("key", key);
    kv.Set("value", val);
    return kv;
  };
  CHECK(proto::KvToRequest(mk("set", "ec.pl1", "90"), &out, &why));
  CHECK(proto::KvToRequest(mk("cmd", "profile.apply", ""), &out, &why));
  CHECK(proto::KvToRequest(mk("cfg", "cfg.readonly", "1"), &out, &why));
  CHECK(!proto::KvToRequest(mk("peek", "ec.pl1", "1"), &out, &why));                 // unknown op
  CHECK(!proto::KvToRequest(mk("set", "", "1"), &out, &why));                        // empty key
  CHECK(!proto::KvToRequest(mk("set", "ec pl1", "1"), &out, &why));                  // bad charset
  CHECK(!proto::KvToRequest(mk("set", "ec.pl1", std::string(5000, '9')), &out, &why));  // value too long
  CHECK(!proto::KvToRequest(mk("cfg", "ec.pl1", "1"), &out, &why));                  // cfg ops must live under cfg.
  CHECK(!proto::KvToRequest(mk("cfg", "cfg.x", std::string(600, 'a')), &out, &why)); // cfg value cap
  KV many = mk("cmd", "reapply", "");
  for (int i = 0; i < 40; i++) many.Set("a.k" + std::to_string(i), "1");
  CHECK(!proto::KvToRequest(many, &out, &why));                                       // too many arguments
  KV badArg = mk("cmd", "reapply", "");
  badArg.Set("a.bad key", "1");
  CHECK(!proto::KvToRequest(badArg, &out, &why));
}

TEST(proto_has_no_way_to_name_a_raw_address) {
  // the registry is the only thing a "set" can reach: nothing that looks like an EC/MSR address is a valid key
  sim::World w;
  Controller c(w.hw(), ManualOpts("sim"));
  c.Start();
  for (const char* k : {"ec.raw", "ec.0x783", "msr.0x610", "0x743", "ecwrite", "ec.write", "raw.ec"}) {
    Request r;
    r.op = "set";
    r.key = k;
    r.value = "1";
    Result res = c.ExecuteSync(r);
    CHECK(!res.ok);
    CHECK(res.code == "unknown" || res.code == "range");
  }
  CHECK_EQ(w.plat.ecWrites, 0);
}

TEST(proto_user_session_key_routing_table) {
  CHECK(proto::IsUserSessionKey("disp.brightness"));
  CHECK(proto::IsUserSessionKey("disp.refresh"));
  CHECK(proto::IsUserSessionKey("dev.touchpad"));
  CHECK(!proto::IsUserSessionKey("dev.wifi"));
  CHECK(!proto::IsUserSessionKey("ec.pl1"));
  CHECK(!proto::IsUserSessionKey("display.x"));
}

TEST(backend_local_passthrough) {
  sim::World w;
  Controller c(w.hw(), ManualOpts("sim"));
  c.Start();
  LocalBackend b(c, "sim");
  CHECK(b.Connected());
  CHECK_EQ(b.Mode(), std::string("sim"));
  KV s = b.Snapshot();
  CHECK(s.Size() > 20);
}

TEST(backend_composite_merges_and_routes) {
  sim::World svc;   // "service" side: owns the EC etc.
  sim::World user;  // "user session" side: only display/touchpad are used
  Controller::Options o1;
  o1.mode = "service";
  Controller::Options o2;
  o2.mode = "local";
  Controller csvc(svc.hw(), o1);  // worker threads, virtual clocks: only explicit requests run
  Controller cuser(user.hw(), o2);
  user.display.brightness = 33;
  svc.display.brightness = 91;
  csvc.Start();
  cuser.Start();
  LocalBackend remote(csvc, "service");
  CompositeBackend comp(remote, cuser);
  CHECK_EQ(comp.Mode(), std::string("service"));
  CHECK(comp.Connected());

  // display brightness must come from the user-side controller, everything else from the service
  KV s = comp.Snapshot();
  CHECK_EQ(s.Int("disp.bright", -1), 33);
  CHECK(s.Has("ec.pl1"));

  Request rb;
  rb.op = "set";
  rb.key = "disp.bright";
  rb.value = "55";
  const uint64_t idb = comp.Submit(rb);
  CHECK(idb > 0);
  Request re;
  re.op = "set";
  re.key = "ec.pl1";
  re.value = "100";
  const uint64_t ide = comp.Submit(re);
  CHECK(ide > 0 && ide != idb);
  Result r1, r2;
  bool g1 = false, g2 = false;
  for (int i = 0; i < 300 && !(g1 && g2); i++) {
    if (!g1) g1 = comp.TakeResult(idb, &r1);
    if (!g2) g2 = comp.TakeResult(ide, &r2);
    if (!(g1 && g2)) usleep(10000);
  }
  CHECK(g1);
  CHECK(g2);
  CHECK(r1.ok);
  CHECK_EQ(user.display.brightness, 55);   // went to the user-side controller
  CHECK_EQ(svc.display.brightness, 91);    // the service-side display was never touched
  CHECK(!comp.TakeResult(idb, &r1));       // results are one-shot
  csvc.Stop();
  cuser.Stop();
}
