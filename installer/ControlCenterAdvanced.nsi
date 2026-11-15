; Control Center Advanced installer (NSIS 3, Unicode).
;
;   makensis -DVERSION=2.0.0 -DNUMVER=2.0.0 -DSTAGE=<dir with ControlCenterAdvanced.exe, LICENSE.txt, NOTICE.txt, README.txt> -DOUTFILE=<setup.exe> installer/ControlCenterAdvanced.nsi
;
; tools/make_release.sh builds everything. What the installer does, and nothing else:
;   - copies ControlCenterAdvanced.exe, the licence and the notices to C:\Program Files\Control Center Advanced (a folder only administrators can write to:
;     the program runs with administrator rights, and the optional sign-in task starts it that way without a prompt, so the file must not be
;     replaceable by a normal user - the installer refuses a folder outside Program Files)
;   - creates a Start Menu shortcut (and a Desktop shortcut if asked)
;   - creates the sign-in task "Start with Windows" if asked ("ControlCenterAdvanced.exe --autostart on"; off by default)
;   - removes the background service of the 1.0.0 beta if it finds one
; It installs no service of its own and no driver, does not touch the vendor's Control Center and does not change any firmware setting. The
; uninstaller removes the sign-in task and the files, and asks before it deletes your settings.
;
; Silent install: Setup.exe /S   (add /AUTOSTART to also create the sign-in task, /D=C:\Program Files\somewhere as the last argument for another
; folder below Program Files).

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!ifndef VERSION
  !define VERSION "2.0.0"
!endif
!ifndef NUMVER
  !define NUMVER "2.0.0"      ; the numeric part of VERSION (the version resource wants a.b.c.d)
!endif
!ifndef STAGE
  !define STAGE "..\build\stage"
!endif
!ifndef OUTFILE
  !define OUTFILE "..\dist\Control-Center-Advanced-${VERSION}-Setup.exe"
!endif

!define APP        "Control Center Advanced"
!define APPKEY     "ControlCenterAdvanced"
!define PUBLISHER  "MehmetNY"
!define EXE        "ControlCenterAdvanced.exe"
!define HOMEPAGE   "https://github.com/MehmetNY/Control-Center-Advanced"
!define SETUPDOC   "https://github.com/MehmetNY/Control-Center-Advanced/blob/main/docs/SETUP.md"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPKEY}"
!define BETA_SERVICE "ControlCenterAdvanced"          ; the Windows service of the 1.0.0 beta (this version installs none)
!define TASK_NAME  "ControlCenterAdvanced_Autostart"   ; the sign-in task the program creates (see src/common/autostart_task.h)

Name "${APP}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP}"
BrandingText "${APP} ${VERSION}"

VIProductVersion "${NUMVER}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP} setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!insertmacro GetParameters
!insertmacro GetOptions
!insertmacro GetSize

!define MUI_ICON   "..\res\app.ico"
!define MUI_UNICON "..\res\app.ico"
!define MUI_ABORTWARNING
!define MUI_UNABORTWARNING
!define MUI_WELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "welcome.bmp"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "header.bmp"
!define MUI_HEADERIMAGE_UNBITMAP "header.bmp"
!define MUI_COMPONENTSPAGE_SMALLDESC

!define MUI_WELCOMEPAGE_TITLE "$(WelcomeTitle)"
!define MUI_WELCOMEPAGE_TEXT "$(WelcomeText)"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "$(FinishRun)"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchUnelevated
!define MUI_FINISHPAGE_SHOWREADME "${SETUPDOC}"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "$(FinishDoc)"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Turkish"

; ---- texts -------------------------------------------------------------------------------------------------------------------------------
LangString WelcomeTitle ${LANG_ENGLISH} "Welcome to ${APP}"
LangString WelcomeTitle ${LANG_TURKISH} "${APP} kurulumuna hoş geldiniz"
LangString WelcomeText ${LANG_ENGLISH} "A light, native control centre for Monster Tulpar / XMG Neo / Tongfang GM5TG7 laptops. It works together with the vendor's own Control Center: its service does the privileged work, so the vendor's Control Center has to be installed on this laptop.$\r$\n$\r$\nIt is an independent project and is not affiliated with Monster, XMG, Tongfang, Intel or NVIDIA. It changes power, fan, keyboard-light and charging settings; read the safety notes in the README before you use it. The program is not signed and has not been run on a real laptop yet.$\r$\n$\r$\nClick Next to continue."
LangString WelcomeText ${LANG_TURKISH} "Monster Tulpar / XMG Neo / Tongfang GM5TG7 dizüstü bilgisayarları için hafif, yerel bir denetim merkezi. Üreticinin kendi Control Center'ı ile birlikte çalışır: ayrıcalıklı işleri onun hizmeti yapar; bu yüzden bu dizüstünde üreticinin Control Center'ı kurulu olmalıdır.$\r$\n$\r$\nBağımsız bir projedir; Monster, XMG, Tongfang, Intel veya NVIDIA ile bağlantısı yoktur. Güç, fan, klavye ışığı ve şarj ayarlarını değiştirir; kullanmadan önce README'deki güvenlik notlarını okuyun. Program imzasızdır ve gerçek bir dizüstünde henüz çalıştırılmamıştır.$\r$\n$\r$\nDevam etmek için İleri'ye tıklayın."

LangString SecMainName ${LANG_ENGLISH} "${APP} (required)"
LangString SecMainName ${LANG_TURKISH} "${APP} (gerekli)"
LangString SecMainDesc ${LANG_ENGLISH} "The program, the licence and the notices. It installs no service and no driver."
LangString SecMainDesc ${LANG_TURKISH} "Program, lisans ve bildirimler. Hizmet ya da sürücü kurmaz."
LangString SecStartName ${LANG_ENGLISH} "Start Menu shortcut"
LangString SecStartName ${LANG_TURKISH} "Başlat menüsü kısayolu"
LangString SecStartDesc ${LANG_ENGLISH} "Adds ${APP} to the Start Menu."
LangString SecStartDesc ${LANG_TURKISH} "${APP}'i Başlat menüsüne ekler."
LangString SecDesktopName ${LANG_ENGLISH} "Desktop shortcut"
LangString SecDesktopName ${LANG_TURKISH} "Masaüstü kısayolu"
LangString SecDesktopDesc ${LANG_ENGLISH} "Adds ${APP} to the desktop."
LangString SecDesktopDesc ${LANG_TURKISH} "${APP}'i masaüstüne ekler."
LangString SecAutoName ${LANG_ENGLISH} "Start with Windows"
LangString SecAutoName ${LANG_TURKISH} "Windows ile başlat"
LangString SecAutoDesc ${LANG_ENGLISH} "Starts the program hidden in the tray when you sign in (a scheduled task with administrator rights, no prompt) so that your GPU power setting is applied again. You can also change this in Preferences."
LangString SecAutoDesc ${LANG_TURKISH} "Oturum açınca programı tepside gizli başlatır (yönetici haklı, istemsiz bir zamanlanmış görev); böylece GPU güç ayarınız yeniden uygulanır. Bunu Tercihler'den de değiştirebilirsiniz."

LangString FinishRun ${LANG_ENGLISH} "Start ${APP}"
LangString FinishRun ${LANG_TURKISH} "${APP}'i başlat"
LangString FinishDoc ${LANG_ENGLISH} "Open the setup notes"
LangString FinishDoc ${LANG_TURKISH} "Kurulum notlarını aç"

LangString NeedX64 ${LANG_ENGLISH} "${APP} needs 64-bit Windows 10 or 11."
LangString NeedX64 ${LANG_TURKISH} "${APP} için 64 bit Windows 10 veya 11 gerekir."
LangString BadDir ${LANG_ENGLISH} "${APP} runs with administrator rights, so it can only be installed below Program Files, where only administrators can change files.$\r$\n$\r$\nChosen folder: $INSTDIR"
LangString BadDir ${LANG_TURKISH} "${APP} yönetici haklarıyla çalışır; bu yüzden yalnızca dosyaları yalnızca yöneticilerin değiştirebildiği Program Files altına kurulabilir.$\r$\n$\r$\nSeçilen klasör: $INSTDIR"
LangString Stopping ${LANG_ENGLISH} "Stopping the running program..."
LangString Stopping ${LANG_TURKISH} "Çalışan program durduruluyor..."
LangString RemovingBeta ${LANG_ENGLISH} "Removing the background service of the beta version (if there is one)..."
LangString RemovingBeta ${LANG_TURKISH} "Beta sürümünün arka plan hizmeti (varsa) kaldırılıyor..."
LangString CreatingTask ${LANG_ENGLISH} "Creating the sign-in task..."
LangString CreatingTask ${LANG_TURKISH} "Oturum açma görevi oluşturuluyor..."
LangString TaskFailed ${LANG_ENGLISH} "The sign-in task could not be created (exit code $0).$\r$\n$\r$\nThe program works without it. You can try again later with Preferences > Start with Windows."
LangString TaskFailed ${LANG_TURKISH} "Oturum açma görevi oluşturulamadı (çıkış kodu $0).$\r$\n$\r$\nProgram onsuz da çalışır. Daha sonra Tercihler > Windows ile başlat ile yeniden deneyebilirsiniz."
LangString NoVendor ${LANG_ENGLISH} "The vendor's Control Center was not found on this computer.$\r$\n$\r$\n${APP} changes settings through the vendor's background service and reads the embedded controller through the vendor's driver, so it can only look around until the vendor's Control Center is installed."
LangString NoVendor ${LANG_TURKISH} "Üreticinin Control Center'ı bu bilgisayarda bulunamadı.$\r$\n$\r$\n${APP} ayarları üreticinin arka plan hizmeti üzerinden değiştirir ve gömülü denetleyiciyi üreticinin sürücüsüyle okur; üreticinin Control Center'ı kurulana kadar yalnızca göz atabilir."
LangString RemovingTask ${LANG_ENGLISH} "Removing the sign-in task..."
LangString RemovingTask ${LANG_TURKISH} "Oturum açma görevi kaldırılıyor..."
LangString AskData ${LANG_ENGLISH} "Also delete your Control Center Advanced settings and log files?$\r$\n$\r$\nChoose No to keep them for a later installation."
LangString AskData ${LANG_TURKISH} "Control Center Advanced ayarlarınız ve günlük dosyaları da silinsin mi?$\r$\n$\r$\nSonraki bir kurulum için saklamak istiyorsanız Hayır'ı seçin."

; ---- install ---------------------------------------------------------------------------------------------------------------------------------
Function LaunchUnelevated
  ; the installer runs elevated; the program should start as the person who is signed in: let the desktop shell start it (Windows asks once)
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${EXE}"'
FunctionEnd

; Ends a running copy of this installation (a window or tray icon holds the file open). Redirection is off so that the 64-bit tools are used.
Function StopRunning
  DetailPrint "$(Stopping)"
  ${DisableX64FSRedirection}
  nsExec::Exec `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Get-Process -Name ControlCenterAdvanced -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force"`
  Pop $0
  ${EnableX64FSRedirection}
  Sleep 400
FunctionEnd

; The 1.0.0 beta ran its own Windows service from this folder; it holds the file open and has no use any more.
Function RemoveBetaService
  DetailPrint "$(RemovingBeta)"
  ${DisableX64FSRedirection}
  nsExec::Exec '"$SYSDIR\sc.exe" stop ${BETA_SERVICE}'
  Pop $0
  Sleep 1500
  nsExec::Exec '"$SYSDIR\sc.exe" delete ${BETA_SERVICE}'
  Pop $0
  ${EnableX64FSRedirection}
FunctionEnd

Section "$(SecMainName)" SecMain
  SectionIn RO
  SetShellVarContext all
  SetRegView 64
  Call RemoveBetaService
  Call StopRunning
  SetOutPath "$INSTDIR"
  File "/oname=${EXE}" "${STAGE}\${EXE}"
  File "${STAGE}\LICENSE.txt"
  File "${STAGE}\NOTICE.txt"
  File "${STAGE}\README.txt"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  WriteRegStr HKLM "Software\${APPKEY}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${EXE}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "${HOMEPAGE}"
  WriteRegStr HKLM "${UNINST_KEY}" "HelpLink" "${HOMEPAGE}/issues"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

Section "$(SecStartName)" SecStart
  SetShellVarContext all
  CreateShortCut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Section /o "$(SecDesktopName)" SecDesktop
  SetShellVarContext all
  CreateShortCut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Section /o "$(SecAutoName)" SecAuto
  SetShellVarContext all
  SetRegView 64
  DetailPrint "$(CreatingTask)"
  nsExec::ExecToLog '"$INSTDIR\${EXE}" --autostart on'
  Pop $0
  ${If} $0 != 0
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONEXCLAMATION "$(TaskFailed)"
    ${EndIf}
  ${EndIf}
SectionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "$(NeedX64)"
    Abort
  ${EndIf}
  SetRegView 64
  ; the program runs with administrator rights (and the optional task starts it that way without a prompt): only a folder below Program Files will do
  StrLen $2 "$PROGRAMFILES64"
  StrCpy $1 "$INSTDIR" $2
  StrCpy $3 "$INSTDIR" 1 $2
  ${If} $1 != "$PROGRAMFILES64"
  ${OrIf} $3 != "\"
    StrLen $2 "$PROGRAMFILES32"
    StrCpy $1 "$INSTDIR" $2
    StrCpy $3 "$INSTDIR" 1 $2
    ${If} $1 != "$PROGRAMFILES32"
    ${OrIf} $3 != "\"
      ${IfNot} ${Silent}
        MessageBox MB_OK|MB_ICONSTOP "$(BadDir)"
      ${EndIf}
      Abort
    ${EndIf}
  ${EndIf}
  ${GetParameters} $0
  ${GetOptions} $0 "/AUTOSTART" $1
  ${IfNot} ${Errors}
    SectionSetFlags ${SecAuto} ${SF_SELECTED}
  ${EndIf}
FunctionEnd

Function .onInstSuccess
  ; a friendly note, not an obstacle: the program looks around without the vendor's Control Center but cannot change anything
  ${IfNot} ${FileExists} "$PROGRAMFILES64\OEM\Control Center\*.*"
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONINFORMATION "$(NoVendor)"
    ${EndIf}
  ${EndIf}
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "$(SecMainDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStart} "$(SecStartDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(SecDesktopDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAuto} "$(SecAutoDesc)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---- uninstall -------------------------------------------------------------------------------------------------------------------------------
Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  DetailPrint "$(RemovingTask)"
  ; the program removes its own sign-in task; the scheduler command is the fallback when the program file is already gone
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    nsExec::ExecToLog '"$INSTDIR\${EXE}" --autostart off'
    Pop $0
  ${EndIf}
  ${DisableX64FSRedirection}
  nsExec::Exec '"$SYSDIR\schtasks.exe" /Delete /TN "${TASK_NAME}" /F'
  Pop $0
  DetailPrint "$(Stopping)"
  nsExec::Exec `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Get-Process -Name ControlCenterAdvanced -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force"`
  Pop $0
  ${EnableX64FSRedirection}
  Sleep 400

  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\NOTICE.txt"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; the sign-in entry of the 1.0.0 beta of the person who runs the uninstaller (this version uses a scheduled task instead)
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "ControlCenterAdvanced"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\${APPKEY}"

  ; settings and logs are kept unless asked: a later installation picks them up again
  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(AskData)" IDNO keep
    RMDir /r "$APPDATA\ControlCenterAdvanced"          ; %ProgramData%\ControlCenterAdvanced (all-users context)
    RMDir /r "$LOCALAPPDATA\ControlCenterAdvanced"
    keep:
  ${EndIf}
SectionEnd
