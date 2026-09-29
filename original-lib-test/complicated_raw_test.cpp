/*
 * complicated_raw_test.cpp
 *
 * ORIJINAL geniePi.c kutuphanesine DOKUNMADAN calisir.
 * 115200 Baud hizi uzerinde:
 * Arka plan thread'i UART'a yuklendigi anda kutuphanenin
 * nasil DEADLOCK'a girdigini (kilitlendigini) gosteren test programi.
 */

#include <cstdint>
extern "C" {
#include "geniePi.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <termios.h>

using namespace std::chrono;

static const int TOTAL_FORMS = 3;
static volatile sig_atomic_t g_stop = 0;

static void onSignal(int)
{
    if (g_stop)
    {
        static const char msg[] = "\n[DEADLOCK TEYIT EDILDI] Kutuphane while(!genieAck) icinde kilitlendi, 2. Ctrl+C ile cikiliyor!\n";
        ssize_t r = write(STDERR_FILENO, msg, sizeof(msg) - 1);
        (void)r;
        _exit(130);
    }
    g_stop = 1;
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

    // ViSi-Genie Set Baud Komutu: [GENIE_SET, GENIE_OBJ_SYS, BAUD_115200 (0x06), CHECKSUM]
    // 0x05 ^ 0x0A ^ 0x06 = 0x09
    unsigned char baudCmd[] = { 0x05, 0x0A, 0x06, 0x09 };
    write(fd, baudCmd, sizeof(baudCmd));
    tcdrain(fd);
    usleep(50000); // Ekranın yeni baud hızına geçmesini bekle

    close(fd);
    std::printf("[DONANIM] Ekran 115200 baud moduna alindi.\n");
}

// geniePi.c'ye dokunmadan kütüphanenin açtığı fd'yi bulup DTR'ı düşüren bekçi
static bool releaseHardwareReset(const std::string &devicePath)
{
    char fdDirPath[64];
    std::snprintf(fdDirPath, sizeof(fdDirPath), "/proc/%d/fd", getpid());
    DIR *dir = opendir(fdDirPath);
    if (!dir) return false;

    struct dirent *entry;
    char targetPath[PATH_MAX];
    bool success = false;

    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.') continue;
        int fd = std::atoi(entry->d_name);
        char linkPath[PATH_MAX];
        std::snprintf(linkPath, sizeof(linkPath), "/proc/%d/fd/%s", getpid(), entry->d_name);

        ssize_t len = readlink(linkPath, targetPath, sizeof(targetPath) - 1);
        if (len != -1)
        {
            targetPath[len] = '\0';
            if (devicePath == targetPath)
            {
                int bits = TIOCM_DTR | TIOCM_RTS;
                if (ioctl(fd, TIOCMBIC, &bits) == 0) {
                    success = true;
                }
                break;
            }
        }
    }
    closedir(dir);
    return success;
}

static std::atomic<bool> g_workerRunning(true);
static std::atomic<bool> g_telemetryActive(false);

// THREAD 1: Arka plan telemetri saldırı görevi
void backgroundTelemetryTask()
{
    std::printf("[THREAD] Arka plan thread'i uykuda (Buton tetigi bekleniyor)...\n");

    while (!g_telemetryActive && g_workerRunning && !g_stop) {
        std::this_thread::sleep_for(milliseconds(5));
    }

    if (!g_workerRunning || g_stop) return;

    std::printf("\n======================================================\n");
    std::printf("[THREAD SALDIRISI BASLADI] Arka plan UART'a seri yaziyor!\n");
    std::printf("======================================================\n");
    std::fflush(stdout);

    int dummy = 0;
    while (g_workerRunning && !g_stop)
    {
        // Ana thread ile AYNI ANDA portta yazma çakışması oluşturuyoruz:
        genieWriteObj(GENIE_OBJ_WINBUTTON, 0, dummy % 2);
        dummy++;
        std::this_thread::sleep_for(milliseconds(10));
    }
}

int main(int argc, char **argv)
{
    std::string device = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    int baud = 115200; // HIZI KESİN OLARAK 115200 YAPIYORUZ

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::printf("=== ORIJINAL geniePi.c THREAD DEADLOCK TESTI (115200 BAUD) ===\n");

    // 1. Ekranı 115200 hızına geçir
    switchDisplayTo115200(device);
    std::this_thread::sleep_for(milliseconds(100));

    // 2. Kütüphaneyi 115200 baud ile başlat
    std::string devCopy = device;
    if (genieSetup(&devCopy[0], baud) < 0) {
        std::fprintf(stderr, "HATA: Port 115200 baud hizinda acilamadi!\n");
        return 1;
    }

    // 3. Ekranı donanımsal resetten kurtar
    if (releaseHardwareReset(device)) {
        std::printf("[DONANIM] DTR hatti LOW yapildi. Ekran aktif!\n");
    }

    // Ekranın arayüzü çizmesi için bekle
    std::this_thread::sleep_for(milliseconds(1500));

    std::printf("[HAZIR] Ekran 115200 baud dinlemede! Ekrandaki butona basin...\n\n");

    std::thread bgThread(backgroundTelemetryTask);

    int currentForm = 0;

    // THREAD 2: Ana UI Döngüsü
    while (!g_stop)
    {
        if (genieReplyAvail())
        {
            struct genieReplyStruct reply;
            genieGetReply(&reply);

            std::printf("[YAKALANDI - 115200 BAUD] cmd=0x%02X object=%d index=%d\n",
                        reply.cmd, reply.object, reply.index);
            std::fflush(stdout);

            if (reply.cmd == GENIE_REPORT_EVENT)
            {
                int nextForm = (currentForm + 1) % TOTAL_FORMS;

                std::printf("\n>>> [MAIN UI] Buton yakalandi! Form gecisi yapiliyor: Form%d -> Form%d ...\n",
                            currentForm, nextForm);
                std::fflush(stdout);

                // Arka plan thread'ini tam bu anda devreye sokuyoruz:
                g_telemetryActive = true;

                // Ana thread de ekrana yazıyor -> İki thread aynı anda kütüphaneye girer ve DEADLOCK:
                int res = genieWriteObj(GENIE_OBJ_FORM, nextForm, 0);

                currentForm = nextForm;
                std::printf(">>> [MAIN UI] Form degisti (res=%d)\n", res);
                std::fflush(stdout);
            }
        }

        std::this_thread::sleep_for(milliseconds(5));
    }

    g_workerRunning = false;
    if (bgThread.joinable()) bgThread.join();

    genieClose();
    return 0;
}