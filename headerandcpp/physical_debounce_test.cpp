/*
 * physical_debounce_test.cpp
 *
 * "ViSi Genie Debouncing Patch'inin Fiziksel 4D Systems Ekran Uzerinde
 * Dogrulanmasi" task'inin host tarafi uygulamasi (Bolum 4/5/7).
 *
 * Ne yapar:
 *  - Gercek /dev/ttyUSBx uzerinden fiziksel ekranla (uLCD-43DT, DIABLO16)
 *    ViSi-Genie protokoluyle konusur (patch'li GeniePiLib uzerinden).
 *  - Ekrandaki BTN_TEST butonuna her fiziksel basista gelen event'i isler.
 *  - Her event ve her ~500ms'de bir durum ozetini CSV olarak loglar:
 *      timestamp, event_type, object_type, object_index, value,
 *      received, accepted, queue_size, raw_total, accepted_total,
 *      rejected_total, dropped_total
 *  - Kabul edilen/ham/reddedilen sayaclarini GERI ekrana yazar
 *    (LEDDIGITS_DIGIT_RAW / _ACCEPTED / _REJECTED), boylece fiziksel
 *    ekranin uzerinde canli olarak debounce'un calistigi gozlemlenebilir.
 *
 * Kullanim:
 *   ./physical_debounce_test [device] [baud] [debounce_ms] [label] [duration_sec]
 *
 *   device        - varsayilan: /dev/ttyUSB0
 *   baud          - varsayilan: 200000 (DIABLO16 icin onerilen; ekraniniza
 *                   gore Workshop4 build/flash sirasinda kullanilan gercek
 *                   hizla ESLESMELI - degilse iletisim kurulamaz)
 *   debounce_ms   - varsayilan: 150 (0 = debounce kapali; Bolum 6
 *                   karsilastirmasi icin 0 vererek "patch etkisiz" durumu
 *                   simule edebilirsiniz, aslinda kutuphane duzeyinde kapatir)
 *   label         - log dosyasi adina eklenecek etiket (orn. "test1_normal_press")
 *   duration_sec  - 0 = suresiz (Ctrl+C ile durdurulur); >0 ise o kadar
 *                   saniye sonra otomatik durur (burst/uzun-sureli testler icin)
 */
#include "GeniePiLib.h"
#include "genie_objects.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <chrono>
#include <ctime>
#include <thread>

using namespace std::chrono;

static volatile sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

static std::string nowTimestamp()
{
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03lld",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, (long long)ms.count());
    return buf;
}

// object type numarasini okunabilir bir isme cevirir (log dosyasini
// insan tarafindan okunur tutmak icin; sadece bu iki tipi biliyoruz,
// gerekirse projeye yeni widget eklendikce buraya eklenir).
static const char *objectTypeName(int object)
{
    switch (object)
    {
        case GENIE_OBJ_USERBUTTON: return "USERBUTTON";
        case GENIE_OBJ_LED_DIGITS: return "LED_DIGITS";
        default: return "UNKNOWN";
    }
}

int main(int argc, char **argv)
{
    std::string device      = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    int         baud        = (argc > 2) ? std::atoi(argv[2]) : 200000;
    unsigned    debounceMs  = (argc > 3) ? (unsigned)std::atoi(argv[3]) : 150;
    std::string label       = (argc > 4) ? argv[4] : "run";
    int         durationSec = (argc > 5) ? std::atoi(argv[5]) : 0;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // --- Log dosyasini ac (test/physical-debounce-test/logs/ altina) ---
    std::string logPath = "logs/" + label + ".csv";
    FILE *log = std::fopen(logPath.c_str(), "w");
    if (!log)
    {
        std::fprintf(stderr, "HATA: log dosyasi acilamadi: %s "
                      "('logs' klasoru mevcut mu?)\n", logPath.c_str());
        return 1;
    }
    std::fprintf(log, "timestamp,event_type,object_type,object_index,value,"
                       "received,accepted,queue_size,raw_total,accepted_total,"
                       "rejected_total,dropped_total\n");
    std::fflush(log);

    // --- Ekrana baglan ---
    GeniePi genie;
    genie.setDebounceTime(debounceMs);
    std::printf("Baglaniyor: %s @ %d baud, debounce=%ums, label=%s%s\n",
                device.c_str(), baud, debounceMs, label.c_str(),
                durationSec > 0 ? (", sure=" + std::to_string(durationSec) + "s").c_str() : "");

    // GeniePi::genieSetup, char* bekliyor (const char* degil) - orijinal
    // API imzasi boyle, biz de aynen kullaniyoruz.
    std::string deviceCopy = device;
    if (genie.genieSetup(&deviceCopy[0], baud) < 0)
    {
        std::fprintf(stderr, "HATA: seri port acilamadi: %s\n", device.c_str());
        std::fclose(log);
        return 1;
    }
    
    std::printf("Baglanti kuruldu. Ekranin acilmasi bekleniyor (2 sn)...\n");
    // Ekranın Form0'ı (MAIN_FORM) çizmesini sağla
    genie.genieWriteObj(GENIE_OBJ_FORM, FORM_MAIN_FORM, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    std::printf("Hazir! Butona basabilirsiniz. Ctrl+C ile durdurun%s.\n",
                durationSec > 0 ? " (veya sure dolunca otomatik duracak)" : "");

    unsigned int acceptedCount = 0;
    auto start = steady_clock::now();
    auto lastStatus = start;

    while (!g_stop)
    {
        if (durationSec > 0 &&
            duration_cast<seconds>(steady_clock::now() - start).count() >= durationSec)
        {
            std::printf("Sure doldu (%ds), durduruluyor.\n", durationSec);
            break;
        }

        // --- Gelen event'leri isle (kisa timeout ile polling - non-blocking
        // ana dongu, boylece durationSec/Ctrl+C kontrolu de yapilabiliyor) ---
        if (genie.genieReplyAvail())
        {
            genieReplyStruct reply;
            genie.genieGetReply(&reply);

            if (reply.cmd == GENIE_REPORT_EVENT &&
                reply.object == GENIE_OBJ_USERBUTTON &&
                reply.index == USERBUTTON_BTN_TEST)
            {
                ++acceptedCount;

                // Requirement: "event received", "accepted/rejected by
                // debounce logic" -- bu satira ulasan her event, tanim
                // geregi kabul edilmis olandir (debounce'un elediklerini
                // host hic gormez; onlarin sayisi asagidaki periyodik
                // ozet satirinda rejected_total ile raporlanir).
                std::fprintf(log, "%s,EVENT,%s,%d,%u,1,1,%zu,%u,%u,%u,%u\n",
                             nowTimestamp().c_str(),
                             objectTypeName(reply.object), reply.index, reply.data,
                             genie.getQueueSize(), genie.getRawEventCount(),
                             acceptedCount, genie.getDebouncedEventCount(),
                             genie.getDroppedEventCount());
                std::fflush(log);

                std::printf("[EVENT] object=%s index=%d value=%u "
                            "(raw=%u accepted=%u rejected=%u dropped=%u)\n",
                            objectTypeName(reply.object), reply.index, reply.data,
                            genie.getRawEventCount(), acceptedCount,
                            genie.getDebouncedEventCount(), genie.getDroppedEventCount());

                // --- Sayaclari geri ekrana yaz, fiziksel ekranda canli
                //     olarak gorunsun (Bolum 2'nin istedigi UI) ---
                genie.genieWriteObj(GENIE_OBJ_LED_DIGITS, LEDDIGITS_DIGIT_RAW, genie.getRawEventCount());
                genie.genieWriteObj(GENIE_OBJ_LED_DIGITS, LEDDIGITS_DIGIT_ACCEPTED, acceptedCount);
                genie.genieWriteObj(GENIE_OBJ_LED_DIGITS, LEDDIGITS_DIGIT_REJECTED, genie.getDebouncedEventCount());
            }
        }

        // --- Periyodik durum satiri (event olmasa bile, "queue state"
        //     zaman icinde de gozlemlenebilsin diye - ozellikle burst ve
        //     uzun-sureli testlerde onemli) ---
        auto now = steady_clock::now();
        if (duration_cast<milliseconds>(now - lastStatus).count() >= 500)
        {
            lastStatus = now;
            std::fprintf(log, "%s,STATUS,,,,0,0,%zu,%u,%u,%u,%u\n",
                         nowTimestamp().c_str(), genie.getQueueSize(),
                         genie.getRawEventCount(), acceptedCount,
                         genie.getDebouncedEventCount(), genie.getDroppedEventCount());
            std::fflush(log);
        }

        std::this_thread::sleep_for(milliseconds(5));
    }

    std::printf("\n=== OZET (%s) ===\n", label.c_str());
    std::printf("Ham event (raw):        %u\n", genie.getRawEventCount());
    std::printf("Kabul edilen (accepted): %u\n", acceptedCount);
    std::printf("Debounce ile elenen:    %u\n", genie.getDebouncedEventCount());
    std::printf("Queue overflow (dropped):%u\n", genie.getDroppedEventCount());
    std::printf("Log dosyasi: %s\n", logPath.c_str());

    genie.genieClose();
    std::fclose(log);
    return 0;
}