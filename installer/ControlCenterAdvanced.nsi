; Control Center Advanced installer (NSIS 3, Unicode).
;
;   makensis -DVERSION=1.0.0-beta -DNUMVER=1.0.0 -DSTAGE=<dir with ControlCenterAdvanced.exe, LICENSE.txt, NOTICE.txt, README.txt> -DOUTFILE=<setup.exe> installer/ControlCenterAdvanced.nsi
;
; tools/make_release.sh builds everything. What the installer does, and nothing else:
;   - copies ControlCenterAdvanced.exe, the licence and the notices to C:\Program Files\Control Center Advanced (a folder only administrators can write to:
;     the service runs as SYSTEM, so the file it runs must not be replaceable by a normal user)
;   - installs and starts the Control Center Advanced service (optional, recommended) with "ControlCenterAdvanced.exe --install-service"
;   - creates a Start Menu shortcut (and a Desktop shortcut if asked)
; It does not install a driver, does not touch the OEM Control Center and does not change any firmware setting. The uninstaller gives the OEM
; services back (ControlCenterAdvanced.exe --uninstall-service), removes the service and the files, and asks before it deletes your settings.
;
; Silent install: Setup.exe /S   (add /NOSERVICE to skip the service, /D=C:\path as the last argument for another folder - the service
; installer refuses an unprotected folder and copies the program below Program Files itself).

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma
RequestExecutionLevel admin

!ifndef VERSION
  !define VERSION "1.0.0-beta"
!endif
!ifndef NUMVER
  !define NUMVER "1.0.0"      ; the numeric part of VERSION (the version resource wants a.b.c.d)
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
LangString WelcomeText ${LANG_ENGLISH} "A light, native control centre for Monster Tulpar / XMG Neo / Tongfang GM5TG7 laptops.$\r$\n$\r$\nIt is an independent project and is not affiliated with Monster, XMG, Tongfang, Intel or NVIDIA. It changes power, fan and charging settings through the laptop's embedded controller; read the safety notes in the README before you use it.$\r$\n$\r$\nClick Next to continue."
LangString WelcomeText ${LANG_TURKISH} "Monster Tulpar / XMG Neo / Tongfang GM5TG7 dizüstü bilgisayarları için hafif, yerel bir denetim merkezi.$\r$\n$\r$\nBağımsız bir projedir; Monster, XMG, Tongfang, Intel veya NVIDIA ile bağlantısı yoktur. Güç, fan ve şarj ayarlarını dizüstünün gömülü denetleyicisi üzerinden değiştirir; kullanmadan önce README'deki güvenlik notlarını okuyun.$\r$\n$\r$\nDevam etmek için İleri'ye tıklayın."

LangString SecMainName ${LANG_ENGLISH} "${APP} (required)"
LangString SecMainName ${LANG_TURKISH} "${APP} (gerekli)"
LangString SecMainDesc ${LANG_ENGLISH} "The program, the licence and the notices."
LangString SecMainDesc ${LANG_TURKISH} "Program, lisans ve bildirimler."
LangString SecServiceName ${LANG_ENGLISH} "Background service (recommended)"
LangString SecServiceName ${LANG_TURKISH} "Arka plan hizmeti (önerilir)"
LangString SecServiceDesc ${LANG_ENGLISH} "Lets the window run without administrator rights, applies your settings at start-up and keeps the fan watchdog running. Without it, run the program as administrator."
LangString SecServiceDesc ${LANG_TURKISH} "Pencerenin yönetici hakları olmadan çalışmasını sağlar, ayarlarınızı açılışta uygular ve fan korumasını çalışır tutar. Olmazsa programı yönetici olarak çalıştırmanız gerekir."
LangString SecStartName ${LANG_ENGLISH} "Start Menu shortcut"
LangString SecStartName ${LANG_TURKISH} "Başlat menüsü kısayolu"
LangString SecStartDesc ${LANG_ENGLISH} "Adds ${APP} to the Start Menu."
LangString SecStartDesc ${LANG_TURKISH} "${APP}'i Başlat menüsüne ekler."
LangString SecDesktopName ${LANG_ENGLISH} "Desktop shortcut"
LangString SecDesktopName ${LANG_TURKISH} "Masaüstü kısayolu"
LangString SecDesktopDesc ${LANG_ENGLISH} "Adds ${APP} to the desktop."
LangString SecDesktopDesc ${LANG_TURKISH} "${APP}'i masaüstüne ekler."

LangString FinishRun ${LANG_ENGLISH} "Start ${APP}"
LangString FinishRun ${LANG_TURKISH} "${APP}'i başlat"
LangString FinishDoc ${LANG_ENGLISH} "Open the setup notes (optional CPU register access)"
LangString FinishDoc ${LANG_TURKISH} "Kurulum notlarını aç (isteğe bağlı CPU yazmaç erişimi)"

LangString NeedX64 ${LANG_ENGLISH} "${APP} needs 64-bit Windows 10 or 11."
LangString NeedX64 ${LANG_TURKISH} "${APP} için 64 bit Windows 10 veya 11 gerekir."
LangString Stopping ${LANG_ENGLISH} "Stopping the running service and program..."
LangString Stopping ${LANG_TURKISH} "Çalışan hizmet ve program durduruluyor..."
LangString InstallingService ${LANG_ENGLISH} "Installing and starting the Control Center Advanced service..."
LangString InstallingService ${LANG_TURKISH} "Control Center Advanced hizmeti kuruluyor ve başlatılıyor..."
LangString ServiceFailed ${LANG_ENGLISH} "The service could not be installed or started (exit code $0).$\r$\n$\r$\nThe program still works: run it as administrator, or install the service later from the Safety and service page."
LangString ServiceFailed ${LANG_TURKISH} "Hizmet kurulamadı veya başlatılamadı (çıkış kodu $0).$\r$\n$\r$\nProgram yine de çalışır: yönetici olarak çalıştırın ya da hizmeti sonra Güvenlik ve hizmet sayfasından kurun."
LangString GivingBack ${LANG_ENGLISH} "Giving the OEM services back and removing the Control Center Advanced service..."
LangString GivingBack ${LANG_TURKISH} "OEM hizmetleri geri veriliyor ve Control Center Advanced hizmeti kaldırılıyor..."
LangString AskData ${LANG_ENGLISH} "Also delete your Control Center Advanced settings, profiles and log files?$\r$\n$\r$\nChoose No to keep them for a later installation."
LangString AskData ${LANG_TURKISH} "Control Center Advanced ayarlarınız, profilleriniz ve günlük dosyaları da silinsin mi?$\r$\n$\r$\nSonraki bir kurulum için saklamak istiyorsanız Hayır'ı seçin."

; ---- install ---------------------------------------------------------------------------------------------------------------------------------
Var NoService

Function LaunchUnelevated
  ; the installer runs elevated; the program should not: let the desktop shell start it
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${EXE}"'
FunctionEnd

Function StopRunning
  DetailPrint "$(Stopping)"
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    nsExec::Exec '"$INSTDIR\${EXE}" --stop-service'
    Pop $0
  ${EndIf}
  ; a window or tray icon of this installation holds the file open
  nsExec::Exec `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Get-Process -Name ControlCenterAdvanced -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force"`
  Pop $0
  Sleep 400
FunctionEnd

Section "$(SecMainName)" SecMain
  SectionIn RO
  SetShellVarContext all
  SetRegView 64
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

Section "$(SecServiceName)" SecService
  SetShellVarContext all
  SetRegView 64
  DetailPrint "$(InstallingService)"
  nsExec::ExecToLog '"$INSTDIR\${EXE}" --install-service'
  Pop $0
  ${If} $0 != 0
    ${IfNot} ${Silent}
      MessageBox MB_OK|MB_ICONEXCLAMATION "$(ServiceFailed)"
    ${EndIf}
  ${EndIf}
SectionEnd

Section "$(SecStartName)" SecStart
  SetShellVarContext all
  CreateShortCut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Section /o "$(SecDesktopName)" SecDesktop
  SetShellVarContext all
  CreateShortCut "$DESKTOP\${APP}.lnk" "$INSTDIR\${EXE}" "" "$INSTDIR\${EXE}" 0
SectionEnd

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "$(NeedX64)"
    Abort
  ${EndIf}
  SetRegView 64
  ${GetParameters} $0
  ${GetOptions} $0 "/NOSERVICE" $1
  ${IfNot} ${Errors}
    StrCpy $NoService 1
  ${EndIf}
  ${If} $NoService == 1
    SectionSetFlags ${SecService} 0
  ${EndIf}
FunctionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "$(SecMainDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecService} "$(SecServiceDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStart} "$(SecStartDesc)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "$(SecDesktopDesc)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---- uninstall -------------------------------------------------------------------------------------------------------------------------------
Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  DetailPrint "$(Stopping)"
  ; --uninstall-service stops the service, hands the OEM services back if Control Center Advanced had taken them over, and removes the service
  DetailPrint "$(GivingBack)"
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    nsExec::ExecToLog '"$INSTDIR\${EXE}" --uninstall-service'
    Pop $0
  ${EndIf}
  nsExec::Exec `powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "Get-Process -Name ControlCenterAdvanced -ErrorAction SilentlyContinue | Where-Object { $$_.Path -like '$INSTDIR\*' } | Stop-Process -Force"`
  Pop $0
  Sleep 400

  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  Delete "$INSTDIR\${EXE}"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\NOTICE.txt"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; "start with Windows" of the person who runs the uninstaller (the program's own switch writes it)
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
