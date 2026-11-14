// Messages the controller produces: matching one that was built with printf against its translation table entry, and the capital letter.
#include <string>

#include "../src/common/msgtext.h"
#include "mini_test.h"

using namespace bc;

namespace {
std::string Tr(const MsgTemplates& m, const std::string& msg) {
  std::string o;
  return m.Translate(msg, &o) ? o : std::string("<no match>");
}
}  // namespace

TEST(msgtext_template_fills_the_translated_text) {
  MsgTemplates m;
  m.Add("CPU is at %d C; refusing to raise power limits above 92 C", "CPU %d °C'de; güç sınırları yükseltilmiyor");
  m.Add("could not stop the OEM service: %s", "OEM hizmeti durdurulamadı: %s");
  m.Add("offset outside the driver range %d..%d", "Ofset aralık dışında (%d..%d)");
  CHECK_EQ(m.Count(), static_cast<size_t>(3));
  CHECK_EQ(Tr(m, "CPU is at 95 C; refusing to raise power limits above 92 C"),
           std::string("CPU 95 °C'de; güç sınırları yükseltilmiyor"));
  // a specifier at the very end takes the rest of the message, whatever it holds (here a Windows error text with its own colon)
  CHECK_EQ(Tr(m, "could not stop the OEM service: Access is denied: code 5"), std::string("OEM hizmeti durdurulamadı: Access is denied: code 5"));
  CHECK_EQ(Tr(m, "could not stop the OEM service: "), std::string("OEM hizmeti durdurulamadı: "));
  // two specifiers: the literal between them tells them apart, and a negative number is one piece
  CHECK_EQ(Tr(m, "offset outside the driver range -200..300"), std::string("Ofset aralık dışında (-200..300)"));
}

TEST(msgtext_template_rejects_what_does_not_match) {
  MsgTemplates m;
  m.Add("CPU is at %d C; refusing to raise power limits above 92 C", "x %d");
  m.Add("could not stop the OEM service: %s", "y %s");
  std::string o;
  CHECK(!m.Translate("", &o));
  CHECK(!m.Translate("CPU is at 95 C", &o));                                              // the end of the literal text is missing
  CHECK(!m.Translate("the CPU is at 95 C; refusing to raise power limits above 92 C", &o));   // the start differs
  CHECK(!m.Translate("could not start the OEM service: x", &o));
  CHECK(!m.Translate("no EC link", &o));
}

TEST(msgtext_entries_that_are_not_templates) {
  MsgTemplates m;
  m.Add("no EC link", "EC yok");                    // no specifier: an exact lookup serves it
  m.Add("%s W", "%s W");                            // almost all specifier: it would match everything
  m.Add("%d%%", "%%%d");
  CHECK_EQ(m.Count(), static_cast<size_t>(0));
  m.Add("limit is %d%% of the maximum power", "sınır en yüksek gücün %%%d kadarı");   // "%%" is a literal percent sign
  CHECK_EQ(m.Count(), static_cast<size_t>(1));
  CHECK_EQ(Tr(m, "limit is 85% of the maximum power"), std::string("sınır en yüksek gücün %85 kadarı"));
}

TEST(msgtext_most_specific_template_wins) {
  MsgTemplates m;
  m.Add("could not %s the OEM service: %s", "generic %s %s");
  m.Add("could not stop the OEM service: %s", "specific %s");
  CHECK_EQ(Tr(m, "could not stop the OEM service: denied"), std::string("specific denied"));   // more literal text: tried first
  CHECK_EQ(Tr(m, "could not restore the OEM service: denied"), std::string("generic restore denied"));
}

TEST(msgtext_capitalises_a_sentence_not_a_token) {
  CHECK_EQ(CapitalizeMessage("no EC link"), std::string("No EC link"));
  CHECK_EQ(CapitalizeMessage("blocked"), std::string("Blocked"));
  CHECK_EQ(CapitalizeMessage("the OEM service is running; take over first"), std::string("The OEM service is running; take over first"));
  CHECK_EQ(CapitalizeMessage("Already capital"), std::string("Already capital"));
  CHECK_EQ(CapitalizeMessage("win32=5"), std::string("win32=5"));                       // not a word
  CHECK_EQ(CapitalizeMessage("0x0463 unreadable"), std::string("0x0463 unreadable"));
  CHECK_EQ(CapitalizeMessage("PL1=45 PL2=60"), std::string("PL1=45 PL2=60"));
  CHECK_EQ(CapitalizeMessage(""), std::string(""));
  CHECK_EQ(CapitalizeMessage("İyi mi"), std::string("İyi mi"));         // a Turkish capital is left alone
}
