/*
 * complicated_patched_test.cpp
 *
 * "ComplicatedTest" senaryosu: 3 formlu, her formda 3 WinButton + 1 metin
 * bulunan ViSi-Genie projesi. PATCH'Lİ kutuphane (GeniePiLib) kullanir.
 *
 * Mantik:
 *   - Ekrandaki herhangi bir WinButton'a basildiginda ekran host'a
 *     GENIE_REPORT_EVENT gonderir.
 *   - Kutuphane (debounce dahil) bu event'i suzer. SADECE KABUL EDILEN
 *     event'lerde host, ekrana "sonraki forma gec" komutu yollar:
 *         genieWriteObj(GENIE_OBJ_FORM, sonrakiForm, 0)
 *   - Debounce'un eledigi tekrar event'ler host'a hic ulasmadigi icin
 *     form DEGISMEZ. Boylece debounce'un etkisi ekranda gozle gorulur.
 *
 * Kullanim:
 *   ./complicated_patched_test [device] [baud] [debounce_ms] [label] [duration_sec]
 *     device       varsayilan /dev/ttyUSB0
 *     baud         varsayilan 9600
 *     debounce_ms  varsayilan 150 (0 = debounce kapali)
 *     label        log dosyasi adi: logs/<label>.csv
 *     duration_sec 0 = Ctrl+C'ye kadar
 *
 * CSV kolonlari:
 *   timestamp,event_type,object_type,object_index,value,form_before,
 *   form_after,switch_result,queue_size,raw_total,accepted_total,
 *   rejected_total,dropped_total
 *   switch_result: 0=ACK, -1=NAK, -2=timeout
 */
#include "GeniePiLib.h"

// Header'i (4d-genie-header-generator ciktisi) varsa dahil et. Bu programin
// mantigi makro isimlerine bagli degil (her WinButton kabul edilir), ama
// gercek testte uretilen header'in kullanildigini gostermek icin dahil edilir.
#if defined(__has_include)
#  if __has_include("complicated_objects.h")
#    include "complicated_objects.h"
#  endif
#endif

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>
#include <chrono>
#include <ctime>
#include <thread>

using namespace std::chrono;

// Projedeki form sayisi (Form0, Form1, Form2)
static const int TOTAL_FORMS = 3;

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

static const char *resultName(int r)
{
    switch (r)
    {
        case GENIE_OK:            return "ACK";
        case GENIE_ERROR_NAK:     return "NAK";
        case GENIE_ERROR_TIMEOUT: return "TIMEOUT";
        default:                  return "?";
    }
}

int main(int argc, char **argv)
{
    std::string device     = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    int         baud       = (argc > 2) ? std::atoi(argv[2]) : 9600;
    unsigned    debounceMs = (argc > 3) ? (unsigned)std::atoi(argv[3]) : 150;
    std::string label      = (argc > 4) ? argv[4] : "complicated_patchli";
    int         durationSec = (argc > 5) ? std::atoi(argv[5]) : 0;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string logPath = "logs/" + label + ".csv";
    FILE *log = std::fopen(logPath.c_str(), "w");
    if (!log)
    {
        std::fprintf(stderr, "HATA: log dosyasi acilamadi: %s ('logs' klasoru var mi?)\n",
                     logPath.c_str());
        return 1;
    }
    std::fprintf(log, "timestamp,event_type,object_type,object_index,value,form_before,"
                      "form_after,switch_result,queue_size,raw_total,accepted_total,"
                      "rejected_total,dropped_total\n");
    std::fflush(log);

    GeniePi genie;
    genie.setDebounceTime(debounceMs);

    std::printf("Baglaniyor (PATCH'Li kutuphane): %s @ %d baud, debounce=%ums, label=%s\n",
                device.c_str(), baud, debounceMs, label.c_str());

    std::string deviceCopy = device;
    if (genie.genieSetup(&deviceCopy[0], baud) < 0)
    {
        std::fprintf(stderr, "HATA: seri port acilamadi: %s\n", device.c_str());
        std::fclose(log);
        return 1;
    }
    std::printf("Baglanti kuruldu. Ekranin acilmasi bekleniyor (2 sn)...\n");
    std::this_thread::sleep_for(seconds(2));

    // Bilinen bir baslangic durumu: ekrani Form0'a al.
    int currentForm = 0;
    int initRes = genie.genieWriteObj(GENIE_OBJ_FORM, currentForm, 0);
    std::printf("Baslangic: Form%d'e gecis komutu -> %s\n", currentForm, resultName(initRes));

    std::printf("Hazir! Herhangi bir butona basin, her KABUL EDILEN basista ekran\n"
                "sonraki forma gececek. Ctrl+C ile durdurun%s.\n",
                durationSec > 0 ? " (veya sure dolunca durur)" : "");

    unsigned int acceptedCount = 0;
    unsigned int switchFailures = 0;
    unsigned int formSwitches = 0;
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

        if (genie.genieReplyAvail())
        {
            genieReplyStruct reply;
            genie.genieGetReply(&reply);

            // 9 WinButton'un hepsi: index'e bakmiyoruz
            if (reply.cmd == GENIE_REPORT_EVENT && reply.object == GENIE_OBJ_WINBUTTON)
            {
                ++acceptedCount;

                int formBefore = currentForm;
                int formAfter  = (currentForm + 1) % TOTAL_FORMS;

                // Sonraki forma gec (ACK bekler, en fazla ackTimeout kadar)
                int res = genie.genieWriteObj(GENIE_OBJ_FORM, formAfter, 0);
                if (res == GENIE_OK)
                {
                    currentForm = formAfter;
                    ++formSwitches;
                }
                else
                {
                    ++switchFailures;
                    formAfter = formBefore; // gecis dogrulanamadi
                }

                std::fprintf(log, "%s,EVENT,WINBUTTON,%d,%u,%d,%d,%d,%zu,%u,%u,%u,%u\n",
                             nowTimestamp().c_str(), (int)reply.index, (unsigned)reply.data,
                             formBefore, formAfter, res, genie.getQueueSize(),
                             genie.getRawEventCount(), acceptedCount,
                             genie.getDebouncedEventCount(), genie.getDroppedEventCount());
                std::fflush(log);

                std::printf("[EVENT] WinButton index=%d  Form%d -> Form%d (%s)  "
                            "(raw=%u accepted=%u rejected=%u dropped=%u)\n",
                            (int)reply.index, formBefore, formAfter, resultName(res),
                            genie.getRawEventCount(), acceptedCount,
                            genie.getDebouncedEventCount(), genie.getDroppedEventCount());
            }
        }

        auto now = steady_clock::now();
        if (duration_cast<milliseconds>(now - lastStatus).count() >= 500)
        {
            lastStatus = now;
            std::fprintf(log, "%s,STATUS,,,,%d,%d,,%zu,%u,%u,%u,%u\n",
                         nowTimestamp().c_str(), currentForm, currentForm,
                         genie.getQueueSize(), genie.getRawEventCount(), acceptedCount,
                         genie.getDebouncedEventCount(), genie.getDroppedEventCount());
            std::fflush(log);
        }

        std::this_thread::sleep_for(milliseconds(5));
    }

    std::printf("\n=== OZET (%s, PATCH'Li kutuphane) ===\n", label.c_str());
    std::printf("Ham event (raw):             %u\n", genie.getRawEventCount());
    std::printf("Kabul edilen (accepted):     %u\n", acceptedCount);
    std::printf("Debounce ile elenen:         %u\n", genie.getDebouncedEventCount());
    std::printf("Form gecisi (basarili):      %u\n", formSwitches);
    std::printf("Form gecisi (basarisiz):     %u\n", switchFailures);
    std::printf("Queue overflow (dropped):    %u\n", genie.getDroppedEventCount());
    std::printf("Son form:                    Form%d\n", currentForm);
    std::printf("Log dosyasi: %s\n", logPath.c_str());

    genie.genieClose();
    std::fclose(log);
    return 0;
}