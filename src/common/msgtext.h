// Text helpers for the messages the controller produces ("no light bar controller", "CPU is at 95 C; refusing ..."): matching a message
// that was built with printf against a translation table entry, and the capital letter a sentence on screen needs. Portable, so the host
// tests cover it (the UI layer in src/ui/i18n.cpp is Windows-only).
#pragma once
#include <string>
#include <vector>

namespace bc {

// A table entry whose English text holds printf specifiers ("CPU is at %d C; ...") is split into the literal text around them. A message
// that was produced from it ("CPU is at 95 C; ...") matches when it has the same literal text; the pieces that stand where the
// specifiers were then fill the specifiers of the translated text.
class MsgTemplates {
 public:
  // Entries without a specifier, and entries that are almost all specifier ("%s W"), are not templates: an exact lookup serves the first,
  // and the second would match everything.
  void Add(const std::string& english, const std::string& translated);
  bool Translate(const std::string& message, std::string* out) const;
  size_t Count() const { return t_.size(); }

 private:
  struct Tmpl {
    std::vector<std::string> lits;   // the literal text around the specifiers; lits.size() == number of specifiers + 1
    std::string translated;
    size_t weight = 0;               // total literal length: the more specific template is tried first
  };
  std::vector<Tmpl> t_;              // ordered by descending weight
};

// "no EC link" -> "No EC link". Only a first word made of letters changes ("win32=5" and "0x0463 unreadable" stay as they are).
std::string CapitalizeMessage(std::string s);

}  // namespace bc
