Control Center Advanced - read me first
====================================

A light, native control centre for the Monster Tulpar T5 V21.7 (GM5TG7Y) and the XMG Neo 15 M21 (GM5TG7W):
performance modes and power limits, fans, keyboard light, battery and display - in the layout of the vendor's own Control Center, with
a Confirm button next to every slider and the extended GPU power (cTGP) on the Performance page.

STATUS
  This is a pre-release. It has been checked with a simulator that stands in for the laptop and for the vendor's service, and with unit
  tests. It has NOT been run on a real laptop yet. The program is unsigned, so Windows SmartScreen warns when you start the installer.

BEFORE YOU USE IT
  - The program needs the vendor's own Control Center on the same laptop: the vendor's background service does the privileged work
  (profile values, fans, keyboard light, charge profile) and its driver lets the program read the embedded controller.
  - Changing power and fan settings can make a laptop unstable and, in the worst case, damage it. The program is free software and comes
    with NO WARRANTY.
  - Change one thing at a time and watch the temperatures.
  - A slider only changes a draft: nothing is sent to the laptop until you press Confirm. The sliders follow changes that other
    programs make in the background.
  - The vendor's ready-made modes (Office, Gaming, Turbo) keep their own values: only the Custom mode is edited.

HOW TO START
  Start menu > Control Center Advanced (Windows asks once for administrator rights: the program needs them to read the embedded controller).
  To look around without touching any hardware:  ControlCenterAdvanced.exe --sim
  "Start with Windows" (Preferences, or the installer option) starts the program hidden in the tray at sign-in; it only works for the copy in
  Program Files.

IF SOMETHING GOES WRONG
  - Service > Write protection > Read-only stops every hardware write and every message to the vendor's service at once.
  - Performance modes: pick one of the vendor's ready-made modes. Custom mode: Vendor defaults > Restore defaults. Fan settings: Vendor's
    curves. GPU power: back to 125 W.
  - Uninstalling removes the program and its sign-in task; the vendor's Control Center is not touched. A restart clears whatever the
    firmware does not keep.

UPGRADING FROM THE 1.0.0 BETA
  The installer removes the beta's background service. If you used the beta's "Take over from the OEM service", the vendor's services may
  still be disabled: open Service > Start the vendor's services.

MORE
  Documentation, source code, releases, issues:  https://github.com/MehmetNY/Control-Center-Advanced
  Licence: GNU General Public License version 3 or later (LICENSE.txt). Credits: NOTICE.txt.


----------------------------------------------------------------------------------------------------------------

Control Center Advanced - beni oku
===============================

Monster Tulpar T5 V21.7 (GM5TG7Y) ve XMG Neo 15 M21 (GM5TG7W) için hafif, yerel bir denetim merkezi:
performans modları ve güç sınırları, fanlar, klavye ışığı, pil ve ekran - üreticinin kendi Control Center'ının düzeninde, her
kaydırıcının yanında bir Onayla düğmesiyle ve Performans sayfasında genişletilmiş GPU gücüyle (cTGP).

DURUM
  Bu bir ön sürümdür (pre-release). Dizüstünün ve üreticinin hizmetinin yerine geçen bir simülatörle ve birim testleriyle denenmiştir.
  Gerçek bir dizüstünde henüz ÇALIŞTIRILMAMIŞTIR. Program imzasızdır; bu yüzden kurulum dosyasını başlatırken Windows SmartScreen uyarı verir.

KULLANMADAN ÖNCE
  - Program aynı dizüstünde üreticinin kendi Control Center'ının kurulu olmasını ister: ayrıcalıklı işleri (profil değerleri, fanlar,
    klavye ışığı, şarj profili) üreticinin arka plan hizmeti yapar; sürücüsü de programın gömülü denetleyiciyi okumasını sağlar.
  - Güç ve fan ayarlarını değiştirmek dizüstünü kararsız yapabilir, en kötü durumda zarar verebilir. Program özgür yazılımdır ve HİÇBİR GARANTİ vermez.
  - Her seferinde tek bir şeyi değiştirin ve sıcaklıkları izleyin.
  - Kaydırıcı yalnızca bir taslağı değiştirir: Onayla'ya basmadan dizüstüne hiçbir şey gönderilmez. Kaydırıcılar, başka programların arka planda
    yaptığı değişiklikleri izler.
  - Üreticinin hazır modları (Ofis, Oyun, Turbo) kendi değerlerini korur: yalnızca Özel mod düzenlenir.

NASIL BAŞLATILIR
  Başlat menüsü > Control Center Advanced (Windows bir kez yönetici hakkı sorar: program gömülü denetleyiciyi okumak için buna ihtiyaç duyar).
  Donanıma dokunmadan gezmek için:  ControlCenterAdvanced.exe --sim
  "Windows ile başlat" (Tercihler ya da kurulumdaki seçenek) programı oturum açılırken tepside gizli başlatır; yalnızca Program Files
  içindeki kopya için çalışır.

BİR ŞEY ters giderse
  - Hizmet > Yazma koruması > Salt okunur, tüm donanım yazmalarını ve üreticinin hizmetine giden her iletiyi hemen durdurur.
  - Performans modları: üreticinin hazır modlarından birini seçin. Özel mod: Üretici varsayılanları > Varsayılanları geri yükle. Fan ayarları:
    Üretici eğrileri. GPU gücü: 125 W'a geri.
  - Kaldırma işlemi programı ve oturum açma görevini siler; üreticinin Control Center'ına dokunmaz. Yeniden başlatmak, firmware'in
    saklamadığı her şeyi temizler.

1.0.0 BETA'DAN YÜKSELTME
  Kurulum programı beta sürümünün arka plan hizmetini kaldırır. Beta'daki "OEM hizmetinden devral" özelliğini kullandıysanız üreticinin
  hizmetleri hâlâ devre dışı olabilir: Hizmet > Üreticinin hizmetlerini başlat'ı açın.

DAHA FAZLASI
  Belgeler, kaynak kod, sürümler, sorunlar:  https://github.com/MehmetNY/Control-Center-Advanced
  Lisans: GNU Genel Kamu Lisansı sürüm 3 veya sonrası (LICENSE.txt). Katkıda bulunanlar: NOTICE.txt.
