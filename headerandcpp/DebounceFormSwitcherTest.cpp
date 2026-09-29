#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <iomanip>
#include "GeniePiLib.h"

// --- AYARLAR ---
static const char* SERIAL_PORT = "/dev/ttyUSB0";
static const int   BAUD_RATE   = 9600;

// Dokunmatik panel form geçişleri için ideal debounce filtresi (400 ms)
static const unsigned int DEBOUNCE_TIME_MS = 150;

// Programın Ctrl+C ile güvenli durdurulması
static std::atomic<bool> g_running{true};
static void signalHandler(int signum) {
    (void)signum;
    g_running = false;
}

int main() {
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    GeniePi genie;

    std::cout << "====================================================\n";
    std::cout << "  4D Systems ViSi-Genie Form Switcher & Debounce   \n";
    std::cout << "====================================================\n";
    std::cout << "Porta bağlanılıyor: " << SERIAL_PORT << " (" << BAUD_RATE << " baud)...\n";

    // Kütüphane seviyesindeki debounce süresini ayarla
    genie.setDebounceTime(DEBOUNCE_TIME_MS);

    // Kütüphaneyi ve arka plan dinleyici thread'ini başlat (genieSetup)
    // genieSetup char* beklediği için const_cast kullanıyoruz
    if (genie.genieSetup(const_cast<char*>(SERIAL_PORT), BAUD_RATE) < 0) {
        std::cerr << "[HATA] Seri port açılamadı! Port adını ve izinlerini kontrol edin.\n";
        return 1;
    }

    std::cout << "[BAŞARILI] Bağlantı sağlandı.\n";
    std::cout << "Filtre Penceresi: " << DEBOUNCE_TIME_MS << " ms\n";
    std::cout << "Ekrandaki butona dokunun... (Çıkış: Ctrl + C)\n\n";

    // Başlangıç formu olarak Form0'ı göster
    int currentForm = 0;
    genie.genieWriteObj(GENIE_OBJ_FORM, currentForm, 0);

    // Uygulama seviyesi sayaçlar ve zaman damgası
    uint32_t acceptedSwitches = 0;
    uint32_t rejectedPresses  = 0;
    auto lastFormSwitch = std::chrono::steady_clock::now() - std::chrono::milliseconds(DEBOUNCE_TIME_MS);

    while (g_running) {
        // Kütüphanenin thread-safe kuyruğunda mesaj var mı?
        if (genie.genieReplyAvail()) {
            struct genieReplyStruct reply;
            genie.genieGetReply(&reply);

            // Sadece buton event'lerini işle (UserButton: 0x21/33 veya WinButton: 0x06/6)
            if (reply.cmd == GENIE_REPORT_EVENT && 
               (reply.object == GENIE_OBJ_USERBUTTON || reply.object == GENIE_OBJ_WINBUTTON || reply.object == 0x21)) {
                
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFormSwitch).count();

                // 400 ms'den daha kısa sürede gelen basışlar (farklı formdaki buton olsa dahi) reddedilir
                if (elapsed < DEBOUNCE_TIME_MS) {
                    rejectedPresses++;
                    std::cout << " [REDDEDİLDİ] Hızlı dokunuş elendi! (" 
                              << elapsed << " ms sonra geldi, limit: " << DEBOUNCE_TIME_MS << " ms)\n";
                } else {
                    // Süre dolmuş, yeni geçerli basış: Formu değiştir
                    lastFormSwitch = now;
                    currentForm = (currentForm == 0) ? 1 : 0;
                    
                    // Ekrana form geçiş komutu gönder
                    genie.genieWriteObj(GENIE_OBJ_FORM, currentForm, 0);
                    acceptedSwitches++;

                    std::cout << " [KABUL EDİLDİ] -> Form " << currentForm 
                              << " açıldı (Geçen süre: " << elapsed << " ms)\n";
                }
            }
        }

        // CPU'yu yormamak için kısa bekleme
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cout << "\n\nTest durduruluyor...\n";
    genie.genieClose();

    // Test Sonu İstatistik Özeti
    uint32_t rawLibraryEvents = genie.getRawEventCount();
    uint32_t debouncedLibraryEvents = genie.getDebouncedEventCount();

    std::cout << "====================================================\n";
    std::cout << "                  TEST ÖZETİ                        \n";
    std::cout << "====================================================\n";
    std::cout << "Kütüphane Tarafı Ham Olay Sayısı  : " << rawLibraryEvents << "\n";
    std::cout << "Kütüphane Tarafı Elenen Sayısı   : " << debouncedLibraryEvents << "\n";
    std::cout << "----------------------------------------------------\n";
    std::cout << "Uygulama Kabul Edilen Form Geçişi : " << acceptedSwitches << "\n";
    std::cout << "Uygulama Reddedilen Hızlı Basış   : " << rejectedPresses << "\n";
    std::cout << "Toplam İşlenen Buton Etkileşimi   : " << (acceptedSwitches + rejectedPresses) << "\n";
    std::cout << "====================================================\n";

    return 0;
}