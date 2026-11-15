#include "autostart_task.h"

#include <cctype>
#include <vector>

namespace bc {
namespace {

std::string XmlEscape(const std::string& s) {
  std::string r;
  r.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': r += "&amp;"; break;
      case '<': r += "&lt;"; break;
      case '>': r += "&gt;"; break;
      case '"': r += "&quot;"; break;
      case '\'': r += "&apos;"; break;
      default: r += c;
    }
  }
  return r;
}

std::string XmlUnescape(std::string s) {
  static const struct { const char* from; char to; } kMap[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
  for (const auto& m : kMap) {
    size_t at = 0;
    const std::string from = m.from;
    while ((at = s.find(from, at)) != std::string::npos) {
      s.replace(at, from.size(), 1, m.to);
      at += 1;
    }
  }
  return s;
}

// text between <tag> and </tag> after position `from`; false when absent
bool Between(const std::string& xml, const std::string& tag, size_t from, std::string* out, size_t* endPos = nullptr) {
  const std::string open = "<" + tag + ">", close = "</" + tag + ">";
  const size_t a = xml.find(open, from);
  if (a == std::string::npos) return false;
  const size_t b = xml.find(close, a + open.size());
  if (b == std::string::npos) return false;
  *out = xml.substr(a + open.size(), b - a - open.size());
  if (endPos) *endPos = b + close.size();
  return true;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string StripQuotesAndSpace(const std::string& s) {
  size_t b = 0, e = s.size();
  auto junk = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; };
  while (b < e && junk(s[b])) ++b;
  while (e > b && junk(s[e - 1])) --e;
  return s.substr(b, e - b);
}

}  // namespace

std::string BuildAutostartXml(const std::string& exePath, const std::string& userId) {
  const std::string u = XmlEscape(userId);
  std::string x;
  x += "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n";
  x += "<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n";
  x += "  <RegistrationInfo><Description>Control Center Advanced: starts the program in the notification area when you sign in. "
       "It changes no setting by itself.</Description></RegistrationInfo>\n";
  x += "  <Triggers><LogonTrigger><Enabled>true</Enabled>";
  if (!u.empty()) x += "<UserId>" + u + "</UserId>";
  x += "<Delay>PT10S</Delay></LogonTrigger></Triggers>\n";
  x += "  <Principals><Principal id=\"Author\">";
  if (!u.empty()) x += "<UserId>" + u + "</UserId>";
  x += "<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n";
  x += "  <Settings>\n";
  x += "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n";
  x += "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n";
  x += "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n";
  x += "    <AllowHardTerminate>true</AllowHardTerminate>\n";
  x += "    <StartWhenAvailable>false</StartWhenAvailable>\n";
  x += "    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\n";
  x += "    <AllowStartOnDemand>true</AllowStartOnDemand>\n";
  x += "    <Enabled>true</Enabled>\n";
  x += "    <Hidden>false</Hidden>\n";
  x += "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n";
  x += "    <Priority>5</Priority>\n";
  x += "  </Settings>\n";
  x += "  <Actions Context=\"Author\"><Exec><Command>\"" + XmlEscape(exePath) + "\"</Command><Arguments>";
  x += kAutostartArgs;
  x += "</Arguments></Exec></Actions>\n";
  x += "</Task>\n";
  return x;
}

TaskObservation ParseTaskXml(const std::string& xml) {
  TaskObservation o;
  if (xml.find("<Task") == std::string::npos) return o;   // not a task definition: the answer is unknown
  o.known = true;
  o.exists = true;
  o.enabled = true;
  const size_t settings = xml.find("<Settings>");
  if (settings != std::string::npos) {
    std::string en;
    if (Between(xml, "Enabled", settings, &en)) o.enabled = Lower(StripQuotesAndSpace(en)) != "false";
  }
  std::string v;
  size_t at = 0;
  const size_t actions = xml.find("<Actions");
  if (actions != std::string::npos) at = actions;
  if (Between(xml, "Command", at, &v)) o.command = XmlUnescape(v);
  if (Between(xml, "Arguments", at, &v)) o.args = XmlUnescape(v);
  return o;
}

bool AutostartIsActive(const TaskObservation& o, const std::string& selfPath) {
  if (!o.known || !o.exists || !o.enabled) return false;
  const std::string self = Lower(StripQuotesAndSpace(selfPath));
  if (self.empty()) return false;
  if (Lower(StripQuotesAndSpace(o.command)) != self) return false;
  return StripQuotesAndSpace(o.args) == kAutostartArgs;
}

bool PathIsBelowAny(const std::string& path, const std::vector<std::string>& roots) {
  auto norm = [](std::string s) {
    for (char& c : s) {
      if (c == '/') c = '\\';
      else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
  };
  const std::string p = norm(path);
  if (p.empty()) return false;
  // a path that climbs ("\..\") can point anywhere once Windows resolves it
  if (p.find("\\..\\") != std::string::npos || (p.size() >= 3 && p.compare(p.size() - 3, 3, "\\..") == 0)) return false;
  for (const std::string& root : roots) {
    std::string r = norm(root);
    while (!r.empty() && r.back() == '\\') r.pop_back();
    if (r.size() < 3) continue;                                          // never a bare drive or an empty entry
    if (p.size() > r.size() + 1 && p.compare(0, r.size(), r) == 0 && p[r.size()] == '\\') return true;
  }
  return false;
}

}  // namespace bc
