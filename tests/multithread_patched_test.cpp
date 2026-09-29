/*
 * multithread_patched_test.cpp
 *
 * GELISMIS / PATCHLI GeniePiLib KUTUPHANESI TESTI.
 * 
 * Ozellikler:
 * - 115200 Baud hizi
 * - Arka planda 10 ms aralikla UART telemetri saldirisi (Deadlock kontrolu)
 * - Debouncing analizi: Ham temas, kabul edilen ve elenen (bounce) sayaclari
 * - CSV loglama ve terminal canli trafik ozeti
 */

#include "GeniePiLib.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

using namespace std::chrono;

static const int TOTAL_FORMS = 3;
static volatile sig_atomic_t g_stop = 0;

static void onSignal(int)
{
    g_stop = 1;
}

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

// Ekranı açılış varsayılanı olan 9600'den 115200 baud'a yükselten köprü fonksiyonu
static void switchDisplayTo115200(const std::string &device)
{
    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return;

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) == 0) {
        cfsetispeed(&tio, B9600);
        cfsetospeed(&tio, B9600);
        tio.c_cflag &= ~HUPCL;
        tio.c_cflag |= (CLOCAL | CREAD | CS8);
        tcsetattr(fd, TCSANOW, &tio);
    }

    // ViSi-Genie Set Baud: [GENIE_SET, GENIE_OBJ_SYS, BAUD_115200 (0x06), CHECKSUM]
    unsigned char baudCmd[] = { 0x05, 0x0A, 0x06, 0x09 };
    write(fd, baudCmd, sizeof(baudCmd));
    tcdrain(fd);
    usleep(50000);

    close(fd);
    std::printf("[DONANIM] Ekran 115200 baud moduna alindi.\n");
}

static std::atomic<bool> g_workerRunning(true);
static std::atomic<bool> g_telemetryActive(false);
static std::atomic<unsigned long> g_spamWrites(0);
static std::atomic<unsigned long> g_spamFails(0);

// THREAD 1: Arka plan telemetri saldırı görevi (Deadlock oluşturma yükü)
void backgroundTelemetryTask(GeniePi *pGenie)
{
    std::printf("[THREAD] Arka plan thread'i uykuda (Buton tetigi bekleniyor)...\n");

    while (!g_telemetryActive && g_workerRunning && !g_stop) {
        std::this_thread::sleep_for(milliseconds(5));
    }

    if (!g_workerRunning || g_stop) return;

    std::printf("\n======================================================\n");
    std::printf("[THREAD SALDIRISI BASLADI] Arka plan UART'a yukleniyor (10ms periyot)!\n");
    std::printf("======================================================\n");
    std::fflush(stdout);

    int dummy = 0;
    while (g_workerRunning && !g_stop)
    {
        int res = pGenie->genieWriteObj(GENIE_OBJ_USER_LED, 0, dummy % 2);
        if (res == GENIE_OK) {
            g_spamWrites++;
        } else {
            g_spamFails++;
        }
        dummy++;

        std::this_thread::sleep_for(milliseconds(10));
    }
    std::printf("[THREAD] Arka plan thread'i sonlandi.\n");
}

int main(int argc, char **argv)
{
    std::string device     = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    int         baud       = 115200;
    unsigned    debounceMs = (argc > 2) ? (unsigned)std::atoi(argv[2]) : 150; // Varsayılan 150 ms filtre penceresi

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::printf("=== GeniePiLib THREAD-SAFETY VE DEBOUNCE TESTI ===\n");
    std::printf("Port: %s | Baud: %d | Debounce Penceresi: %u ms\n", device.c_str(), baud, debounceMs);

    // 1. Ekranı 115200 hızına geçir
    switchDisplayTo115200(device);
    std::this_thread::sleep_for(milliseconds(100));

    // 2. Kütüphane nesnesi (İç filtrelemeyi 0 yapıp metrikleri test katmanında şeffafça sayıyoruz)
    GeniePi genie;
    genie.setDebounceTime(0);

    std::string devCopy = device;
    if (genie.genieSetup(&devCopy[0], baud) < 0) {
        std::fprintf(stderr, "HATA: Port acilamadi: %s\n", device.c_str());
        return 1;
    }

    std::string logPath = "logs/complicated_patched_debounce.csv";
    FILE *log = std::fopen(logPath.c_str(), "w");
    if (log) {
        std::fprintf(log, "timestamp,event_type,button_index,raw_count,accepted_count,dropped_count,elapsed_ms,action\n");
        std::fflush(log);
    }

    // Ekranın açılış çizimlerini tamamlaması için bekleme
    std::this_thread::sleep_for(milliseconds(1500));

    std::printf("[BASARILI] GeniePiLib dinlemede!\n");
    std::printf("SENARYO: Butona bastiginizda arka plan yuklenecek, hem gecisler hem de\n");
    std::printf("mukerrer ark/bounce temaslari canli olarak raporlanacak.\n\n");

    std::thread bgThread(backgroundTelemetryTask, &genie);

    int currentForm = 0;
    unsigned int rawTouches       = 0;
    unsigned int acceptedButtons  = 0;
    unsigned int debouncedDrops   = 0;
    unsigned int formSwitchSuccess = 0;

    auto lastButtonTime = steady_clock::now() - milliseconds(debounceMs + 100);
    auto lastStatus     = steady_clock::now();

    // THREAD 2: Ana UI Döngüsü
    while (!g_stop)
    {
        if (genie.genieReplyAvail())
        {
            struct genieReplyStruct reply;
            genie.genieGetReply(&reply);

            if (reply.cmd == GENIE_REPORT_EVENT && reply.object == GENIE_OBJ_WINBUTTON)
            {
                rawTouches++;
                auto now = steady_clock::now();
                auto elapsedMs = duration_cast<milliseconds>(now - lastButtonTime).count();

                // DEBOUNCE KONTROLU: Son kabul edilen basıştan bu yana geçen süre pencereden küçükse
                if (elapsedMs < debounceMs && acceptedButtons > 0)
                {
                    debouncedDrops++;
                    std::printf(">>> [DEBOUNCE ELENDI] %lld ms icinde gelen mukerrer temas yutuldu! (Toplam Elenen: %u)\n",
                                (long long)elapsedMs, debouncedDrops);
                    std::fflush(stdout);

                    if (log) {
                        std::fprintf(log, "%s,BOUNCE,%d,%u,%u,%u,%lld,DROPPED\n",
                                     nowTimestamp().c_str(), reply.index, rawTouches,
                                     acceptedButtons, debouncedDrops, (long long)elapsedMs);
                        std::fflush(log);
                    }
                }
                else
                {
                    // Temiz ve geçerli basış kabul edildi
                    lastButtonTime = now;
                    acceptedButtons++;

                    int nextForm = (currentForm + 1) % TOTAL_FORMS;
                    std::printf("\n>>> [KABUL EDILDI] Temiz Buton (index=%d, fark=%lld ms)! Form%d -> Form%d yapiliyor...\n",
                                reply.index, (long long)elapsedMs, currentForm, nextForm);
                    std::fflush(stdout);

                    // Arka plan saldırısını ilk geçerli basışta tetikle
                    g_telemetryActive = true;

                    int res = genie.genieWriteObj(GENIE_OBJ_FORM, nextForm, 0);

                    if (res == GENIE_OK) {
                        currentForm = nextForm;
                        formSwitchSuccess++;
                        std::printf(">>> [MAIN UI] Form BASARIYLA degisti! (Form: %d | Toplam Basarili Gecis: %u)\n",
                                    currentForm, formSwitchSuccess);
                    } else {
                        std::printf(">>> [MAIN UI] Gecis basarisiz (Hata kodu: %d)\n", res);
                    }
                    std::fflush(stdout);

                    if (log) {
                        std::fprintf(log, "%s,ACCEPTED,%d,%u,%u,%u,%lld,SWITCH_FORM_%d\n",
                                     nowTimestamp().c_str(), reply.index, rawTouches,
                                     acceptedButtons, debouncedDrops, (long long)elapsedMs, nextForm);
                        std::fflush(log);
                    }
                }
            }
        }

        // Canlı durum raporu
        auto now = steady_clock::now();
        if (duration_cast<milliseconds>(now - lastStatus).count() >= 1000)
        {
            lastStatus = now;
            if (g_telemetryActive) {
                std::printf("[CANLI TRAFIK] Telemetri: %lu (Fail: %lu) | Ham Temas: %u | Kabul: %u | Debounce Elenen: %u | Form Gecisi: %u\n",
                            g_spamWrites.load(), g_spamFails.load(), rawTouches,
                            acceptedButtons, debouncedDrops, formSwitchSuccess);
                std::fflush(stdout);
            }
        }

        std::this_thread::sleep_for(milliseconds(5));
    }

    std::printf("\nTest sonlandiriliyor, thread kapatiliyor...\n");
    g_workerRunning = false;
    if (bgThread.joinable()) {
        bgThread.join();
    }

    genie.genieClose();
    if (log) std::fclose(log);

    std::printf("\n=== FINAL TEST RAPORU ===\n");
    std::printf("Toplam Ham Dokunus (Raw Touches) : %u\n", rawTouches);
    std::printf("Kabul Edilen Temiz Basış         : %u\n", acceptedButtons);
    std::printf("Debounce ile Elenen Sıçrama      : %u\n", debouncedDrops);
    std::printf("Basarili Form Gecis Sayisi       : %u\n", formSwitchSuccess);
    std::printf("Telemetri Paket Sayisi (UART Yuk): %lu\n", g_spamWrites.load());
    std::printf("=========================================\n");

    return 0;
}