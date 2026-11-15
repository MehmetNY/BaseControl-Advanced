// The link to the vendor's Control Center service: JSON reader/writer, MQTT codec, message builders, and the session logic against the
// in-memory fake broker + service (src/sim/fake_oem.*).
#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "../src/oem/json.h"
#include "../src/oem/link.h"
#include "../src/oem/mqtt.h"
#include "../src/oem/proto.h"
#include "../src/common/registers.h"
#include "../src/common/util.h"
#include "../src/sim/fake_oem.h"
#include "../src/sim/sim.h"
#include "mini_test.h"

using namespace bc;
using namespace bc::oem;

namespace {
bool WaitFor(const std::function<bool()>& pred, int ms = 4000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!pred()) {
    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() > ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ JSON
TEST(oem_json_parse_basic) {
  JVal v;
  std::string err;
  CHECK(ParseJson("{\"a\":1,\"b\":-2.5,\"c\":\"x\\ny\",\"d\":[1,2,{\"e\":true}],\"f\":null}", &v, &err));
  CHECK(v.IsObj());
  CHECK_EQ(v.Find("a")->AsInt(), 1LL);
  CHECK_NEAR(v.Find("b")->AsNum(), -2.5, 1e-9);
  CHECK_EQ(v.Find("c")->str, std::string("x\ny"));
  CHECK(v.Find("d")->IsArr());
  CHECK_EQ(v.Find("d")->arr.size(), static_cast<size_t>(3));
  CHECK(v.Find("d")->arr[2].Find("e")->AsBool());
  CHECK(v.Find("zzz") == nullptr);
  CHECK(v.FindI("A") != nullptr);   // keys are matched without case on request
}

TEST(oem_json_tolerant_conversions) {
  JVal v;
  CHECK(ParseJson("{\"n\":\"125\",\"h\":\"0x7D\",\"t\":\"true\",\"s\":\" 7 \",\"x\":\"abc\"}", &v, nullptr));
  CHECK_EQ(v.Find("n")->AsInt(), 125LL);
  CHECK_EQ(v.Find("h")->AsInt(), 125LL);
  CHECK(v.Find("t")->AsBool());
  CHECK_EQ(v.Find("s")->AsInt(), 7LL);
  CHECK(!v.Find("x")->Numeric());
  CHECK_EQ(v.Find("x")->AsInt(9), 9LL);
}

TEST(oem_json_rejects_bad_input) {
  JVal v;
  std::string err;
  const char* bad[] = {"", "{", "{\"a\":}", "{\"a\":1,}", "[1,2", "{\"a\" 1}", "nul", "{\"a\":1} x", "\"abc", "{\"a\":\"\\q\"}", "{1:2}", "01", "1.", "-"};
  for (const char* b : bad) {
    CHECK(!ParseJson(b, &v, &err));
    CHECK(!err.empty());
  }
  // limits: depth and size
  std::string deep(40, '[');
  deep += std::string(40, ']');
  CHECK(!ParseJson(deep, &v, &err));
  JsonLimits lim;
  lim.maxBytes = 16;
  CHECK(!ParseJson("{\"aaaaaaaaaaaaaaaaaaaa\":1}", &v, &err, lim));
}

TEST(oem_json_unicode_and_bom) {
  JVal v;
  CHECK(ParseJson("\xEF\xBB\xBF{\"k\":\"\\u00fc\\ud83d\\ude00\"}", &v, nullptr));
  CHECK_EQ(v.Find("k")->str, std::string("\xC3\xBC\xF0\x9F\x98\x80"));
}

TEST(oem_json_flatten) {
  JVal v;
  CHECK(ParseJson("{\"a\":{\"b\":2,\"c\":[10,20]},\"d\":true,\"e\":\"s\",\"f\":null}", &v, nullptr));
  KV kv;
  Flatten(v, "oem.x", &kv);
  CHECK_EQ(kv.Str("oem.x.a.b"), std::string("2"));
  CHECK_EQ(kv.Str("oem.x.a.c.0"), std::string("10"));
  CHECK_EQ(kv.Str("oem.x.a.c.1"), std::string("20"));
  CHECK_EQ(kv.Str("oem.x.a.c.#"), std::string("2"));
  CHECK_EQ(kv.Str("oem.x.d"), std::string("1"));
  CHECK_EQ(kv.Str("oem.x.e"), std::string("s"));
  CHECK(!kv.Has("oem.x.f"));
  // a hostile payload cannot flood the snapshot
  std::string big = "{";
  for (int i = 0; i < 2000; i++) big += Fmt("%s\"k%d\":%d", i ? "," : "", i, i);
  big += "}";
  CHECK(ParseJson(big, &v, nullptr));
  KV out;
  Flatten(v, "p", &out, 64, 600);
  CHECK(out.Size() <= 601);
}

TEST(oem_json_writer) {
  JsonOut j;
  j.BeginObj().Str("a", "q\"\\\n\t\x01").Int("b", -5).Num("c", 1.5).Bool("d", true).Null("e").BeginArr("f").ElemInt(1).ElemStr("x").EndArr().BeginObj("g").EndObj().EndObj();
  CHECK(j.Valid());
  const std::string t = j.Take();
  JVal v;
  std::string err;
  CHECK(ParseJson(t, &v, &err));
  CHECK_EQ(v.Find("a")->str, std::string("q\"\\\n\t\x01"));
  CHECK_EQ(v.Find("b")->AsInt(), -5LL);
  CHECK(v.Find("e")->type == JVal::Null);
  CHECK_EQ(v.Find("f")->arr.size(), static_cast<size_t>(2));
  CHECK(t.find('\n') == std::string::npos);   // control characters never survive unescaped
  JsonOut bad;
  bad.BeginObj().Int(nullptr, 1);
  CHECK(!bad.Valid());
}

// ------------------------------------------------------------------------------------------------ MQTT codec
TEST(oem_mqtt_packet_reader_fragments) {
  const std::vector<uint8_t> a = EncodePublish("Fan/Status", "{\"x\":1}", 0, false, 0);
  const std::vector<uint8_t> b = EncodePing();
  std::vector<uint8_t> all = a;
  all.insert(all.end(), b.begin(), b.end());
  PacketReader rd;
  Packet p;
  int got = 0;
  for (size_t i = 0; i < all.size(); i++) {   // one byte at a time
    rd.Feed(&all[i], 1);
    while (rd.Next(&p) == PacketReader::Got) {
      got++;
      if (got == 1) {
        PublishMsg m;
        CHECK(DecodePublish(p, &m));
        CHECK_EQ(m.topic, std::string("Fan/Status"));
        CHECK_EQ(m.payload, std::string("{\"x\":1}"));
      } else {
        CHECK_EQ(static_cast<int>(p.type), static_cast<int>(kPingreq));
      }
    }
  }
  CHECK_EQ(got, 2);
}

TEST(oem_mqtt_large_payload_remaining_length) {
  const std::string big(70000, 'z');   // needs a three-byte remaining length
  const std::vector<uint8_t> a = EncodePublish("t", big, 0, false, 0);
  PacketReader rd;
  rd.Feed(a.data(), a.size());
  Packet p;
  CHECK(rd.Next(&p) == PacketReader::Got);
  PublishMsg m;
  CHECK(DecodePublish(p, &m));
  CHECK_EQ(m.payload.size(), big.size());
}

TEST(oem_mqtt_reader_rejects_broken_streams) {
  PacketReader rd;
  const uint8_t badLen[] = {0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F};   // five length bytes
  rd.Feed(badLen, sizeof badLen);
  Packet p;
  CHECK(rd.Next(&p) == PacketReader::Bad);
  PacketReader rd2;
  const uint8_t huge[] = {0x30, 0xFF, 0xFF, 0xFF, 0x7F};   // 256 MB declared
  rd2.Feed(huge, sizeof huge);
  CHECK(rd2.Next(&p) == PacketReader::Bad);
  PacketReader rd3;
  const uint8_t part[] = {0x30, 0x05, 'a'};
  rd3.Feed(part, sizeof part);
  CHECK(rd3.Next(&p) == PacketReader::NeedMore);
}

TEST(oem_mqtt_decoders_validate) {
  Packet p;
  p.type = kPublish;
  p.body = {0x00, 0x09, 'a'};   // topic length runs past the end
  PublishMsg m;
  CHECK(!DecodePublish(p, &m));
  Packet ack;
  ack.type = kConnack;
  ack.body = {0x00, 0x04};
  int rc = -1;
  bool sp = true;
  CHECK(DecodeConnack(ack, &rc, &sp));
  CHECK_EQ(rc, 4);
  CHECK(!sp);
  ack.body = {0x00};
  CHECK(!DecodeConnack(ack, &rc, &sp));
}

TEST(oem_mqtt_topic_filters) {
  CHECK(TopicMatches("Fan/Status", "Fan/Status"));
  CHECK(!TopicMatches("Fan/Status", "Fan/Table"));
  CHECK(TopicMatches("Fan/+", "Fan/Status"));
  CHECK(!TopicMatches("Fan/+", "Fan/Status/x"));
  CHECK(TopicMatches("#", "a/b/c"));
  CHECK(TopicMatches("a/#", "a/b/c"));
  CHECK(TopicMatches("a/#", "a"));
  CHECK(!TopicMatches("#", "$SYS/x"));
}

TEST(oem_mqtt_connect_packet) {
  const std::vector<uint8_t> c = EncodeConnect("cid", "usr", "pw", 30, true);
  CHECK_EQ(static_cast<int>(c[0]), 0x10);
  Packet p;
  PacketReader rd;
  rd.Feed(c.data(), c.size());
  CHECK(rd.Next(&p) == PacketReader::Got);
  // protocol name, level 4, flags user+pass+clean, keep-alive 30
  CHECK_EQ(static_cast<int>(p.body[2]), static_cast<int>('M'));
  CHECK_EQ(static_cast<int>(p.body[6]), 4);
  CHECK_EQ(static_cast<int>(p.body[7]), 0xC2);
  CHECK_EQ(static_cast<int>(p.body[9]), 30);
}

// ------------------------------------------------------------------------------------------------ proto
TEST(oem_proto_prefix_selection_follows_the_vendor_rule) {
  CHECK_EQ(SelectUiPrefix(0, 0), std::string("UWPClient"));
  CHECK_EQ(SelectUiPrefix(1, 16), std::string("UWPIntelClient"));
  CHECK_EQ(SelectUiPrefix(1, 25), std::string("UWPIntelClient"));
  CHECK_EQ(SelectUiPrefix(1, 29), std::string("UWPADATAClient"));
  CHECK_EQ(SelectUiPrefix(1, 3), std::string("UWPSTDClient"));
  const std::vector<std::string> c = UiPrefixCandidates(1, 29);
  CHECK_EQ(c.front(), std::string("UWPADATAClient"));
  CHECK_EQ(c.size(), static_cast<size_t>(4));
}

TEST(oem_proto_identity_shape) {
  const Identity id = MakeIdentity("UWPClient", 18);
  CHECK_EQ(id.clientId, std::string("UWPClient_18"));
  CHECK_EQ(id.user, std::string("UWPClient_User_18"));
  CHECK(StartsWith(id.pass, "UWPClient_Pwd"));
  CHECK(EndsWith(id.pass, "_18"));
  CHECK_EQ(id.slot, 18);
}

TEST(oem_proto_fan_messages) {
  JVal v;
  CHECK(ParseJson(msg::FanMode(msg::Mode::Turbo, 2).payload, &v, nullptr));
  CHECK_EQ(v.Find("Action")->str, std::string("OPERATING_TURBO_MODE"));
  CHECK_EQ(v.Find("ProfileIndex")->AsInt(), 2LL);
  CHECK(ParseJson(msg::FanMode(msg::Mode::Office, -1).payload, &v, nullptr));
  CHECK(v.Find("ProfileIndex") == nullptr);   // absent = keep the profile the service has
  CHECK_EQ(msg::FanBoost(true).topic, std::string("Fan/Control"));
  CHECK(ParseJson(msg::FanBoost(false).payload, &v, nullptr));
  CHECK_EQ(v.Find("Action")->str, std::string("FAN_BOOST_OFF"));
}

TEST(oem_proto_detail_allow_list_refuses_firmware_backed_keys) {
  Msg m;
  CHECK(msg::FanDetail({{"PL1", 120}, {"PL2", 130}}, &m));
  JVal v;
  CHECK(ParseJson(m.payload, &v, nullptr));
  CHECK_EQ(v.Find("Action")->str, std::string("SET_OPERATING_MODE_DETAIL"));
  CHECK_EQ(v.Find("PL1")->AsInt(), 120LL);
  // never sent: BIOS variable store, reboot-bound voltage offset, helper-program TGP, memory overclocking
  const char* never[] = {"CPUPerformanceAndOverClockMenuSwitch", "MemoryOverClockSwitch", "CpuOffsetCoreVoltage", "CpuOffsetCoreVoltageOC",
                         "CustomTGPinGCUforGN21_Value", "CustomTGPinGCUforGN21_Enable", "CpuAmdSPL", "OverClockingSwitch", "FanSwitchSpeed",
                         "FanSwitchSpeedEnabled", "GpuConfigurableTGP", "GpuDynamicBoost", "Action", ""};
  for (const char* k : never) {
    CHECK(!msg::FanDetailAllowed(k));
    Msg x;
    CHECK(!msg::FanDetail({{"PL1", 100}, {k, 1}}, &x));
  }
  Msg none;
  CHECK(!msg::FanDetail({}, &none));
}

TEST(oem_proto_keyboard_envelope_has_function_and_action) {
  JVal v;
  CHECK(ParseJson(msg::KbGetStatus().payload, &v, nullptr));
  CHECK_EQ(v.Find("Action")->str, std::string("GETSTATUS"));
  CHECK(v.Find("function") != nullptr);
  msg::KbEffect e;
  e.effect = 4;
  e.light = 9;      // out of range: clamped
  e.speed = -3;
  e.direction = 2;
  e.colors = {{255, 0, 0}, {0, 255, 0}, {0, 0, 300}, {10, 20, 30}};
  const Msg m = msg::KbEffectSet(e);
  CHECK_EQ(m.topic, std::string("Keyboard/Ctrl"));
  CHECK(ParseJson(m.payload, &v, nullptr));
  CHECK_EQ(v.Find("function")->str, std::string("SetEffectALL"));
  CHECK_EQ(v.Find("Action")->str, std::string("SET"));
  CHECK_EQ(v.Find("effect")->AsInt(), 4LL);
  CHECK_EQ(v.Find("light")->AsInt(), 4LL);
  CHECK_EQ(v.Find("speed")->AsInt(), 0LL);
  CHECK(!v.Find("nv_save")->AsBool());
  const JVal* col = v.Find("color");
  CHECK(col != nullptr);
  CHECK_EQ(col->Find("ColorBlocks")->AsInt(), 4LL);
  CHECK_EQ(col->Find("ColorBuffer")->arr.size(), static_cast<size_t>(4));
  CHECK_EQ(col->Find("ColorBuffer")->arr[2].Find("B")->AsInt(), 255LL);   // clamped
  CHECK_EQ(col->Find("ColorBuffer")->arr[3].Find("ID")->AsInt(), 3LL);
}

TEST(oem_proto_setting_allow_list) {
  CHECK(msg::SettingActionAllowed("WIFI_ON"));
  CHECK(msg::SettingActionAllowed("WINKEY_LOCK"));
  CHECK(!msg::SettingActionAllowed("ICCPROFILESETING"));
  CHECK(!msg::SettingActionAllowed("rm -rf"));
  JVal v;
  CHECK(ParseJson(msg::SettingCloseTimer(2, 30).payload, &v, nullptr));
  CHECK_EQ(v.Find("Mins")->AsInt(), 2LL);
  CHECK_EQ(v.Find("Seconds")->AsInt(), 30LL);
  CHECK(ParseJson(msg::BatteryProfile(2).payload, &v, nullptr));
  CHECK_EQ(v.Find("Action")->str, std::string("HEALTHYMODE"));
}

TEST(oem_proto_fan_table_shape) {
  fan::TableImage cpu = fan::Pack(fan::DefaultCpu()), gpu = fan::Pack(fan::DefaultGpu());
  const Msg m = msg::FanTableSet("M1T2", "Default", &cpu, &gpu);
  JVal v;
  CHECK(ParseJson(m.payload, &v, nullptr));
  CHECK_EQ(v.Find("Action")->str, std::string("SET_FAN_SPEED_CURVE_SETTING"));
  CHECK_EQ(v.Find("Name")->str, std::string("M1T2"));
  const JVal* c = v.Find("CPU");
  CHECK(c != nullptr);
  CHECK(c->Find("Duty")->Find("T15") != nullptr);
  CHECK(c->Find("UpT")->Find("T0") != nullptr);
  CHECK(c->Find("DownT")->Find("T9") != nullptr);
  CHECK_EQ(c->Find("Duty")->Find("T3")->AsInt(), static_cast<long long>(cpu.duty[3]));
}

TEST(oem_ingest_status_replaces_and_detects_change) {
  std::map<std::string, KV> st;
  std::string err;
  CHECK(IngestStatus("Fan/Status", "{\"CPU_PL1\":120,\"X\":{\"Y\":3}}", &st, &err));
  CHECK_EQ(st["oem.fan"].Str("oem.fan.CPU_PL1"), std::string("120"));
  CHECK_EQ(st["oem.fan"].Str("oem.fan.X.Y"), std::string("3"));
  CHECK(!IngestStatus("Fan/Status", "{\"CPU_PL1\":120,\"X\":{\"Y\":3}}", &st, &err));   // same content: no change
  CHECK(IngestStatus("Fan/Status", "{\"CPU_PL1\":121}", &st, &err));
  CHECK(!st["oem.fan"].Has("oem.fan.X.Y"));   // a status is a whole object: the old one is replaced
  CHECK(!IngestStatus("Fan/Status", "not json", &st, &err));
  CHECK(!err.empty());
  CHECK_EQ(st["oem.fan"].Str("oem.fan.CPU_PL1"), std::string("121"));   // garbage leaves the state alone
  CHECK(!IngestStatus("Unknown/Topic", "{}", &st, &err));
  CHECK(st.count("oem.unknown") == 0);
}

// ------------------------------------------------------------------------------------------------ session against the fake service
TEST(oem_link_connects_and_receives_status) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.Connected(); }));
  CHECK(WaitFor([&] {
    const KV now = link.State();   // the service answers the status requests one after the other, so wait for every topic the checks below read
    return now.Has("oem.fan.CPU_PL1") && now.Has("oem.kb.effect") && now.Has("oem.set.CloseTimer") && now.Has("oem.table.Name") &&
           now.Has("oem.batt.HealthProtectionStatus") && now.Has("oem.sup.KeyboardType");
  }));
  const KV s = link.State();
  CHECK_EQ(s.Str("oem.fan.CPU_PL1"), std::string("120"));
  CHECK_EQ(s.Str("oem.fan.OperatingMode"), std::string("2"));
  CHECK_EQ(s.Str("oem.kb.color.ColorBlocks"), std::string("8"));
  CHECK_EQ(s.Str("oem.table.Name"), std::string("M1T2"));
  CHECK_EQ(s.Str("oem.batt.HealthProtectionStatus"), std::string("1"));
  CHECK(s.Has("oem.sup.KeyboardType"));
  const KV info = link.Info();
  CHECK_EQ(info.Str("oem.link.state"), std::string("connected"));
  CHECK_EQ(info.Str("oem.link.class"), std::string("UWPClient"));
  CHECK_EQ(info.Int("oem.link.slot"), 18LL);
  CHECK(link.Version() > 0);
  CHECK_EQ(world.LastClientId(), std::string("UWPClient_18"));
  link.Stop();
  CHECK(!link.Connected());
}

TEST(oem_link_command_changes_service_state_and_status_follows) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.State().Has("oem.fan.CPU_PL1"); }));
  // the profile in use is Overboost: its own values run, whatever is stored for the user's values
  CHECK_EQ(link.State().Str("oem.fan.OperatingMode"), std::string("2"));
  CHECK_EQ(static_cast<int>(plat.ec[reg::kPl1]), 120);
  // the Customized profile takes the user's values
  CHECK(link.Publish(msg::FanMode(msg::Mode::Custom, -1)));
  CHECK(WaitFor([&] { return link.State().Str("oem.fan.OperatingMode") == "3"; }));
  Msg m;
  CHECK(msg::FanDetail({{"PL1", 95}, {"PL2", 110}}, &m));
  const uint64_t v0 = link.Version();
  CHECK(link.Publish(m));
  CHECK(WaitFor([&] { return link.State().Str("oem.fan.CPU_PL1") == "95"; }));
  CHECK(link.Version() > v0);
  CHECK_EQ(link.State().Str("oem.fan.CPU_PL2"), std::string("110"));
  CHECK_EQ(world.State().detail[3].pl1, 95);
  // the service moved the (simulated) EC; the app itself wrote nothing
  CHECK_EQ(static_cast<int>(plat.ec[reg::kPl1]), 95);
  CHECK_EQ(static_cast<int>(plat.ec[reg::kPl2]), 110);
  CHECK_EQ(plat.ecWrites, 0);
  // mode switch: the Balanced profile brings its own values
  CHECK(link.Publish(msg::FanMode(msg::Mode::Office, -1)));
  CHECK(WaitFor([&] { return link.State().Str("oem.fan.OperatingMode") == "0"; }));
  CHECK_EQ(link.State().Str("oem.fan.CPU_PL1"), std::string("45"));
  CHECK_EQ(static_cast<int>(plat.ec[reg::kPl1]), 45);
  link.Stop();
}

TEST(oem_link_a_ready_made_profile_keeps_its_own_values_in_the_ec) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.State().Has("oem.fan.CPU_PL1"); }));
  Msg m;
  CHECK(msg::FanDetail({{"PL1", 95}}, &m));
  CHECK(link.Publish(m));
  CHECK(WaitFor([&] { return link.State().Str("oem.fan.CPU_PL1") == "95"; }));
  // the status echoes the stored value, but the EC - what the CPU obeys - is unchanged: this is why the program reads the EC back
  CHECK_EQ(static_cast<int>(plat.ec[reg::kPl1]), 120);
  link.Stop();
}

TEST(oem_link_keyboard_effect_reaches_every_block) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.State().Has("oem.kb.color.ColorBlocks"); }));
  msg::KbEffect e;
  e.effect = 0;
  for (int i = 0; i < 8; i++) e.colors.push_back(msg::KbColor{i * 30, 255 - i * 30, 7 * i});
  CHECK(link.Publish(msg::KbEffectSet(e)));
  CHECK(WaitFor([&] { return link.State().Str("oem.kb.color.ColorBuffer.7.R") == "210"; }));
  const sim::FakeServiceState st = world.State();
  for (int i = 0; i < 8; i++) {
    CHECK_EQ(st.kb.rgb[3 * i], i * 30);
    CHECK_EQ(st.kb.rgb[3 * i + 1], 255 - i * 30);
  }
  link.Stop();
}

TEST(oem_link_reconnects_after_the_broker_restarts) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.Connected() && link.State().Has("oem.fan.CPU_PL1"); }));
  const long long s0 = link.Info().Int("oem.link.sessions");
  world.DropConnections();
  CHECK(WaitFor([&] { return link.Info().Int("oem.link.sessions") > s0 && link.Connected(); }));
  CHECK(WaitFor([&] { return link.State().Has("oem.fan.CPU_PL1"); }));
  link.Stop();
}

TEST(oem_link_service_not_running_is_reported_and_recovers) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  world.SetServiceUp(false);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.Info().Str("oem.link.state") == "offline" && !link.Info().Str("oem.link.detail").empty(); }));
  CHECK(!link.Connected());
  CHECK(!link.Publish(msg::FanBoost(true)));   // nothing is queued while there is no session
  world.SetServiceUp(true);
  CHECK(WaitFor([&] { return link.Connected(); }));
  link.Stop();
}

TEST(oem_link_tries_the_other_client_classes_when_refused) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  world.SetAcceptedPrefixes({"UWPSTDClient"});
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.Connected(); }));
  CHECK_EQ(link.Info().Str("oem.link.class"), std::string("UWPSTDClient"));
  CHECK(world.Refused() >= 1);
  link.Stop();
}

TEST(oem_link_reports_refusal_when_no_class_is_accepted) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  world.SetAcceptedPrefixes({"SomethingElse"});
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.Info().Str("oem.link.state") == "refused"; }));
  CHECK(!link.Connected());
  link.Stop();
}

TEST(oem_link_second_session_with_the_same_id_replaces_the_first) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink a(world.LinkConfigFor()), b(world.LinkConfigFor());
  a.Start();
  CHECK(WaitFor([&] { return a.Connected(); }));
  b.Start();
  CHECK(WaitFor([&] { return b.Info().Int("oem.link.sessions") >= 1; }));
  // both use slot 18: the broker keeps one of them, and each link recovers by itself (they take turns, never both dead for good)
  CHECK(WaitFor([&] { return world.Accepted() >= 2; }));
  a.Stop();
  b.Stop();
}

TEST(oem_link_capture_records_foreign_traffic) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.SetCapture(true);
  link.Start();
  CHECK(WaitFor([&] { return link.Connected(); }));
  // another client (the vendor's own UI, say) talks to the service: the capture sees the control message
  MqttOemLink other(world.LinkConfigFor());
  LinkConfig oc = world.LinkConfigFor();
  oc.slot = 3;
  MqttOemLink second(oc);
  second.Start();
  CHECK(WaitFor([&] { return second.Connected(); }));
  CHECK(second.Publish(msg::FanBoost(true)));
  CHECK(WaitFor([&] {
    for (const std::string& l : link.Trace(80))
      if (l.find("RX Fan/Control") != std::string::npos) return true;
    return false;
  }));
  second.Stop();
  link.Stop();
}

TEST(oem_link_status_from_another_client_shows_up_live) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink ours(world.LinkConfigFor());
  ours.Start();
  CHECK(WaitFor([&] { return ours.State().Has("oem.fan.CPU_PL1"); }));
  LinkConfig oc = world.LinkConfigFor();
  oc.slot = 4;
  MqttOemLink vendorUi(oc);
  vendorUi.Start();
  CHECK(WaitFor([&] { return vendorUi.Connected(); }));
  Msg m;
  CHECK(msg::FanDetail({{"PL4", 180}}, &m));
  CHECK(vendorUi.Publish(m));
  // the service broadcasts its status; our link follows without having asked
  CHECK(WaitFor([&] { return ours.State().Str("oem.fan.CPU_PL4") == "180"; }));
  vendorUi.Stop();
  ours.Stop();
}

TEST(oem_link_follows_keyboard_changes_made_by_another_program) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.State().Has("oem.kb.color.ColorBlocks"); }));
  CHECK_EQ(link.State().Str("oem.kb.light"), "3");
  // the vendor's own window (or anything else) changes the light: the service publishes the new status and our link follows
  world.ExternalKeyboard("bright", 0);
  CHECK(WaitFor([&] { return link.State().Str("oem.kb.light") == "0"; }));
  world.ExternalKeyboard("color", 0x1ec8ff);
  CHECK(WaitFor([&] { return link.State().Str("oem.kb.color.ColorBuffer.7.R") == "30" && link.State().Str("oem.kb.color.ColorBuffer.0.B") == "255"; }));
  world.ExternalKeyboard("power", 0);
  CHECK(WaitFor([&] { return link.State().Str("oem.kb.powerStatus") == "0"; }));
  world.ExternalKeyboard("nonsense", 1);   // an unknown field changes nothing and publishes nothing
  CHECK_EQ(world.State().kb.power, 0);
  link.Stop();
}

TEST(oem_fake_service_ignores_malformed_requests) {
  sim::Clock clk;
  sim::Platform plat(clk);
  sim::FakeOemWorld world(&plat);
  MqttOemLink link(world.LinkConfigFor());
  link.Start();
  CHECK(WaitFor([&] { return link.State().Has("oem.fan.CPU_PL1"); }));
  Msg bad{"Fan/Control", "{\"Action\":"};
  CHECK(link.Publish(bad));
  Msg bad2{"Fan/Control", "{\"Action\":\"SET_FAN_SPEED_CURVE_SETTING\",\"CPU\":{\"Duty\":1}}"};
  CHECK(link.Publish(bad2));
  CHECK(link.Publish(msg::FanBoost(true)));
  CHECK(WaitFor([&] { return link.State().Str("oem.fan.FanBoostEnable") == "1"; }));
  CHECK_EQ(world.State().cpuTable.duty[3], fan::Pack(fan::DefaultCpu()).duty[3]);
  link.Stop();
}

TEST(oem_link_survives_garbage_from_the_broker) {
  // a transport that answers the connect request with noise: the session must end cleanly and the link keep retrying
  struct Noise : ITransport {
    bool sent = false;
    bool Open(const std::string&, int, int, std::string*) override { return true; }
    bool SendAll(const uint8_t*, size_t) override { sent = true; return true; }
    int Recv(uint8_t* p, size_t cap, int) override {
      static const uint8_t junk[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      const size_t n = std::min(cap, sizeof junk);
      std::memcpy(p, junk, n);
      return static_cast<int>(n);
    }
    void Close() override {}
  };
  LinkConfig cfg;
  cfg.prefixes = {"UWPClient"};
  cfg.reconnectMinMs = 20;
  cfg.reconnectMaxMs = 40;
  cfg.transport = [] { return std::unique_ptr<ITransport>(new Noise); };
  MqttOemLink link(cfg);
  link.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(!link.Connected());
  link.Stop();
}
