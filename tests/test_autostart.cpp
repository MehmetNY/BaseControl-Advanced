// "Start with Windows" as a Task Scheduler task: the task definition the program writes, how the scheduler's answer is read, and when the task
// counts as "ours and active". (Running schtasks.exe itself is Windows-only and is covered by the Wine flows, not here.)
#include <string>
#include <vector>

#include "../src/common/autostart_task.h"
#include "mini_test.h"

using namespace bc;

namespace {
const char kExe[] = "C:\\Program Files\\Control Center Advanced\\ControlCenterAdvanced.exe";
}

TEST(autostart_xml_runs_this_program_with_the_highest_rights_at_sign_in_and_nothing_else) {
  const std::string x = BuildAutostartXml(kExe, "PC\\Mehmet");
  CHECK(x.find("<LogonTrigger>") != std::string::npos);
  CHECK(x.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos);
  CHECK(x.find("<LogonType>InteractiveToken</LogonType>") != std::string::npos);
  CHECK(x.find("<UserId>PC\\Mehmet</UserId>") != std::string::npos);
  CHECK(x.find("<Arguments>--tray</Arguments>") != std::string::npos);
  CHECK(x.find(std::string("<Command>\"") + kExe + "\"</Command>") != std::string::npos);
  CHECK(x.find("<Enabled>true</Enabled>") != std::string::npos);
  CHECK(x.find("<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>") != std::string::npos);
  CHECK(x.find("<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>") != std::string::npos);
  CHECK(x.find("<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>") != std::string::npos);
  // one action only, and no second program, script or network access in the definition
  size_t n = 0;
  for (size_t at = x.find("<Exec>"); at != std::string::npos; at = x.find("<Exec>", at + 1)) n++;
  CHECK_EQ(n, static_cast<size_t>(1));
  CHECK(x.find("powershell") == std::string::npos);
  CHECK(x.find("cmd.exe") == std::string::npos);
}

TEST(autostart_xml_escapes_what_it_is_given) {
  const std::string x = BuildAutostartXml("C:\\Tom & Jerry\\<app>.exe", "DOM\\o'neil");
  CHECK(x.find("Tom &amp; Jerry") != std::string::npos);
  CHECK(x.find("&lt;app&gt;.exe") != std::string::npos);
  CHECK(x.find("o&apos;neil") != std::string::npos);
  CHECK(x.find("Tom & Jerry") == std::string::npos);
}

TEST(autostart_xml_without_a_user_id_still_forms_a_task) {
  const std::string x = BuildAutostartXml(kExe, "");
  CHECK(x.find("<UserId>") == std::string::npos);
  CHECK(x.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos);
}

TEST(autostart_the_task_we_write_is_read_back_as_active) {
  const std::string x = BuildAutostartXml(kExe, "PC\\Mehmet");
  const TaskObservation o = ParseTaskXml(x);
  CHECK(o.known);
  CHECK(o.exists);
  CHECK(o.enabled);
  CHECK_EQ(o.args, std::string("--tray"));
  CHECK(AutostartIsActive(o, kExe));
  CHECK(AutostartIsActive(o, std::string("\"") + kExe + "\""));                 // quotes around the path do not matter
  CHECK(AutostartIsActive(o, "c:\\program files\\control center advanced\\controlcenteradvanced.exe"));   // nor does the case
}

TEST(autostart_a_task_for_another_program_or_other_arguments_is_not_ours) {
  const TaskObservation o = ParseTaskXml(BuildAutostartXml(kExe, "PC\\Mehmet"));
  CHECK(!AutostartIsActive(o, "C:\\Elsewhere\\ControlCenterAdvanced.exe"));       // the program was moved or reinstalled
  CHECK(!AutostartIsActive(o, ""));
  TaskObservation other = o;
  other.args = "--tray --something";
  CHECK(!AutostartIsActive(other, kExe));
  other.args = "";
  CHECK(!AutostartIsActive(other, kExe));
}

TEST(autostart_a_disabled_or_missing_task_is_not_active) {
  std::string x = BuildAutostartXml(kExe, "PC\\Mehmet");
  const size_t at = x.find("<Enabled>true</Enabled>", x.find("<Settings>"));
  CHECK(at != std::string::npos);
  x.replace(at, std::string("<Enabled>true</Enabled>").size(), "<Enabled>false</Enabled>");
  const TaskObservation off = ParseTaskXml(x);
  CHECK(off.known);
  CHECK(off.exists);
  CHECK(!off.enabled);
  CHECK(!AutostartIsActive(off, kExe));

  TaskObservation none;                // the scheduler could not be asked
  CHECK(!none.known);
  CHECK(!AutostartIsActive(none, kExe));
  none.known = true;                   // it answered: there is no such task
  CHECK(!AutostartIsActive(none, kExe));
}

TEST(autostart_text_that_is_not_a_task_definition_is_unknown_not_missing) {
  for (const char* junk : {"", "ERROR: The system cannot find the file specified.", "<html>nope</html>", "<Other><Command>x</Command></Other>"}) {
    const TaskObservation o = ParseTaskXml(junk);
    CHECK(!o.known);
    CHECK(!o.exists);
    CHECK(!AutostartIsActive(o, kExe));
  }
}

TEST(autostart_the_command_is_read_from_the_action_not_from_somewhere_else_in_the_file) {
  // a <Command> inside another element before <Actions> must not be taken for the program that is started
  const std::string x =
      "<Task><RegistrationInfo><Description><Command>evil.exe</Command></Description></RegistrationInfo>"
      "<Settings><Enabled>true</Enabled></Settings>"
      "<Actions><Exec><Command>\"C:\\real.exe\"</Command><Arguments>--tray</Arguments></Exec></Actions></Task>";
  const TaskObservation o = ParseTaskXml(x);
  CHECK(o.known);
  CHECK(AutostartIsActive(o, "C:\\real.exe"));
  CHECK(!AutostartIsActive(o, "evil.exe"));
}

TEST(autostart_only_points_at_a_program_in_a_folder_that_normal_users_cannot_write_to) {
  const std::vector<std::string> roots = {"C:\\Program Files", "C:\\Program Files (x86)\\", "C:/Windows"};
  CHECK(PathIsBelowAny(kExe, roots));
  CHECK(PathIsBelowAny("c:\\PROGRAM FILES\\Control Center Advanced\\ControlCenterAdvanced.exe", roots));     // case does not matter
  CHECK(PathIsBelowAny("C:\\Program Files (x86)\\Tool\\tool.exe", roots));                                  // a root given with a trailing slash
  CHECK(PathIsBelowAny("C:\\Windows\\System32\\tool.exe", roots));                                          // a root given with forward slashes
  CHECK(!PathIsBelowAny("C:\\Users\\Mehmet\\Downloads\\ControlCenterAdvanced.exe", roots));
  CHECK(!PathIsBelowAny("D:\\Program Files\\Control Center Advanced\\ControlCenterAdvanced.exe", roots));   // another drive
  CHECK(!PathIsBelowAny("C:\\Program Files Evil\\x.exe", roots));                                           // a longer name that starts the same way
  CHECK(!PathIsBelowAny("C:\\Program Files", roots));                                                       // the folder itself is not a program inside it
  CHECK(!PathIsBelowAny("C:\\Program Files\\..\\Users\\x\\y.exe", roots));                                  // climbing out
  CHECK(!PathIsBelowAny("C:\\Program Files\\Tool\\..", roots));
  CHECK(!PathIsBelowAny("", roots));
  CHECK(!PathIsBelowAny(kExe, {}));
  CHECK(!PathIsBelowAny("C:\\x.exe", {"C:\\", "", "C:"}));                                                  // a bare drive or an empty root never counts
}
