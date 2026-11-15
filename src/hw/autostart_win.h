// "Start with Windows": the Task Scheduler task of src/common/autostart_task.h, created and queried through schtasks.exe (system folder,
// fixed arguments). Windows only.
#pragma once
#include <string>

#include "../common/autostart_task.h"

namespace bc {
namespace hw {

TaskObservation QueryAutostart();
// The sign-in task runs this program with administrator rights and no prompt, so it is only created for a copy that sits in a folder normal
// users cannot write to (Program Files, which is where the installer puts it). False says why.
bool AutostartAllowedHere(std::string* why);
// Creates or removes the task; the result is checked by asking the scheduler again. Also removes the sign-in entry in the Run registry key
// that the first build of this program used (Windows does not start elevated programs from there).
bool SetAutostart(bool on, std::string* why);

}  // namespace hw
}  // namespace bc
