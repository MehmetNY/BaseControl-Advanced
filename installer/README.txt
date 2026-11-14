BaseControl Advanced - read me first
====================================

A light, native control centre for the Monster Tulpar T5 V21.7 (GM5TG7Y) and the XMG Neo 15 M21 (GM5TG7W):
power limits, fans, lighting, battery and display.

STATUS
  This is a pre-release. It has been checked with a simulator and unit tests, NOT on a real laptop by its
  author. The program is unsigned, so Windows SmartScreen warns when you start the installer.

BEFORE YOU USE IT
  - Changing power and fan settings can make a laptop unstable and, in the worst case, damage it. The program
    is free software and comes with NO WARRANTY.
  - Change one thing at a time and watch the temperatures.
  - A slider only changes a draft: nothing is sent to the laptop until you press Confirm.
  - On a laptop the program does not recognise, hardware writes stay locked until you unlock them yourself
    (Safety and service).

HOW TO START
  Start menu > BaseControl Advanced. The window needs no administrator rights; the background service that
  was installed with it does the privileged work.
  Without the service: start BaseControl.exe and, on the "not connected" page, choose "Run elevated without
  the service". To look around without touching any hardware: BaseControl.exe --sim

IF SOMETHING GOES WRONG
  - Safety and service > Read-only mode stops every hardware write at once.
  - Performance > Fan settings: set the cooling mode to Auto and press Confirm to give the fans back to the
    firmware.
  - Uninstalling removes the service and gives the OEM Control Center's service back. A restart clears
    whatever the firmware does not keep.

MORE
  Documentation, source code, releases, issues:  https://github.com/MehmetNY/BaseControl-Advanced
  Licence: GNU General Public License version 3 or later (LICENSE.txt). Credits: NOTICE.txt.


----------------------------------------------------------------------------------------------------------------

BaseControl Advanced - beni oku
===============================

Monster Tulpar T5 V21.7 (GM5TG7Y) ve XMG Neo 15 M21 (GM5TG7W) için hafif, yerel bir denetim merkezi:
güç sınırları, fanlar, aydınlatma, pil ve ekran.

DURUM
  Bu bir ön sürümdür. Bir simülatör ve birim testleriyle denenmiştir; yazarı tarafından gerçek bir dizüstünde
  DENENMEMİŞTİR. Program imzasızdır; bu yüzden kurulum dosyasını başlatırken Windows SmartScreen uyarı verir.

KULLANMADAN ÖNCE
  - Güç ve fan ayarlarını değiştirmek dizüstünü kararsız yapabilir, en kötü durumda zarar verebilir. Program
    özgür yazılımdır ve HİÇBİR GARANTİ vermez.
  - Her seferinde tek bir şeyi değiştirin ve sıcaklıkları izleyin.
  - Kaydırıcı yalnızca bir taslağı değiştirir: Onayla'ya basmadan dizüstüne hiçbir şey gönderilmez.
  - Programın tanımadığı bir dizüstünde donanım yazmaları, siz açana kadar kilitli kalır (Güvenlik ve hizmet).

NASIL BAŞLATILIR
  Başlat menüsü > BaseControl Advanced. Pencere yönetici hakkı istemez; ayrıcalıklı işleri kurulumla gelen
  arka plan hizmeti yapar.
  Hizmet olmadan: BaseControl.exe'yi başlatın, "bağlı değil" sayfasında "Hizmet olmadan yönetici olarak çalıştır"ı
  seçin. Donanıma dokunmadan gezmek için: BaseControl.exe --sim

BİR ŞEY ters giderse
  - Güvenlik ve hizmet > Salt okunur mod, tüm donanım yazmalarını hemen durdurur.
  - Performans > Fan ayarları: soğutma modunu Otomatik yapıp Onayla'ya basın; fanlar üretici firmware'ine geri verilir.
  - Kaldırma işlemi hizmeti siler ve OEM Control Center'ın hizmetini geri verir. Yeniden başlatmak, firmware'in
    saklamadığı her şeyi temizler.

DAHA FAZLASI
  Belgeler, kaynak kod, sürümler, sorunlar:  https://github.com/MehmetNY/BaseControl-Advanced
  Lisans: GNU Genel Kamu Lisansı sürüm 3 veya sonrası (LICENSE.txt). Katkıda bulunanlar: NOTICE.txt.
