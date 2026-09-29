#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <csignal>
#include <chrono>
#include <ctime>
#include <string>
#include <thread>
#include "genie_objects.h"

#define GENIE_OBJ_USERBUTTON  0x21
#define GENIE_OBJ_LED_DIGITS  0x0F
#define GENIE_REPORT_EVENT    0x07
#define GENIE_WRITE_OBJ       0x01

using namespace std::chrono;

static volatile sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

static std::string nowTimestamp() {
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

static void writeDigit(int fd, uint8_t index, uint16_t value) {
    if (fd < 0) return;
    uint8_t cmd[6];
    cmd[0] = GENIE_WRITE_OBJ;
    cmd[1] = GENIE_OBJ_LED_DIGITS;
    cmd[2] = index;
    cmd[3] = (value >> 8) & 0xFF;
    cmd[4] = value & 0xFF;
    uint8_t checksum = 0;
    for (int i = 0; i < 5; i++) checksum ^= cmd[i];
    cmd[5] = checksum;
    ssize_t ignored = write(fd, cmd, 6);
    (void)ignored;
}

int main(int argc, char **argv) {
    std::string device      = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    int         baud        = (argc > 2) ? std::atoi(argv[2]) : 9600;
    std::string label       = (argc > 3) ? argv[3] : "original_run";
    int         durationSec = (argc > 4) ? std::atoi(argv[4]) : 0;

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string logPath = "logs/" + label + ".csv";
    FILE *log = std::fopen(logPath.c_str(), "w");
    if (!log) {
        std::fprintf(stderr, "HATA: log dosyasi acilamadi: %s ('logs' klasoru var mi?)\n", logPath.c_str());
        return 1;
    }

    std::fprintf(log, "timestamp,event_type,object_type,object_index,value,"
                       "received,accepted,queue_size,raw_total,accepted_total,"
                       "rejected_total,dropped_total\n");
    std::fflush(log);

    int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("Port acilamadi");
        std::fclose(log);
        return 1;
    }

    struct termios tty;
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);

    speed_t baudRate = (baud == 115200) ? B115200 : B9600;
    cfsetospeed(&tty, baudRate);
    cfsetispeed(&tty, baudRate);

    tty.c_cflag &= ~HUPCL;
    tty.c_cflag |= (CLOCAL | CREAD | CS8);
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 1;
    tcsetattr(fd, TCSANOW, &tty);

    int status = 0;
    if (ioctl(fd, TIOCMGET, &status) == 0) {
        status &= ~(TIOCM_DTR | TIOCM_RTS);
        ioctl(fd, TIOCMSET, &status);
    }

    std::printf("Baglaniyor (ORIJINAL kutuphane davranisi, ham okuma): %s @ %d baud, label=%s\n",
                device.c_str(), baud, label.c_str());
    std::printf("Baglanti kuruldu. Butona basabilirsiniz. Ctrl+C ile durdurun%s.\n",
                durationSec > 0 ? " (veya sure dolunca durur)" : "");

    unsigned int acceptedCount = 0;
    auto start = steady_clock::now();
    auto lastStatus = start;

    unsigned char rxBuf[64];

    std::printf("Donguye girildi, port okunuyor (fd=%d)...\n", fd);
    while (!g_stop) {
        if (durationSec > 0 &&
            duration_cast<seconds>(steady_clock::now() - start).count() >= durationSec) {
            std::printf("Sure doldu (%ds), durduruluyor.\n", durationSec);
            break;
        }

        int n = read(fd, rxBuf, sizeof(rxBuf));
        if (n > 0) {
            std::printf("[RX HAM %d byte]: ", n);
            for (int i = 0; i < n; i++) {
                std::printf("0x%02X ", rxBuf[i]);
            }
            std::printf("\n");
            std::fflush(stdout);

            // Paket kontrolu: Genie Report Event (0x07) + USERBUTTON (0x21)
            for (int i = 0; i <= n - 6; i++) {
                if (rxBuf[i] == GENIE_REPORT_EVENT && rxBuf[i + 1] == GENIE_OBJ_USERBUTTON) {
                    uint8_t objIndex = rxBuf[i + 2];
                    uint16_t val = (uint16_t)((rxBuf[i + 3] << 8) | rxBuf[i + 4]);

                    // ONEMLI DUZELTME: "if (val > 0)" FILTRESI KALDIRILDI.
                    // Butonunuz value=0 gonderiyor (daha once patch'li
                    // testlerde de gozlemlemistik) - o filtre TUM gercek
                    // event'leri sessizce eliyordu.
                    ++acceptedCount;

                    std::fprintf(log, "%s,EVENT,USERBUTTON,%d,%u,1,1,NA,%u,%u,0,NA\n",
                                 nowTimestamp().c_str(), objIndex, val,
                                 acceptedCount, acceptedCount);
                    std::fflush(log);

                    std::printf(">>> [EVENT KABUL] Buton Index=%d Deger=%u (Toplam=%u)\n",
                                objIndex, val, acceptedCount);

                    writeDigit(fd, LEDDIGITS_DIGIT_RAW, acceptedCount);
                    writeDigit(fd, LEDDIGITS_DIGIT_ACCEPTED, acceptedCount);
                }
            }
        }

        auto now = steady_clock::now();
        if (duration_cast<milliseconds>(now - lastStatus).count() >= 500) {
            lastStatus = now;
            std::fprintf(log, "%s,STATUS,,,,0,0,NA,%u,%u,0,NA\n",
                         nowTimestamp().c_str(), acceptedCount, acceptedCount);
            std::fflush(log);
        }

        usleep(10000);
    }

    std::printf("\n=== OZET (%s, ORIJINAL mantik) ===\n", label.c_str());
    std::printf("Alinan event (raw=accepted, filtre yok): %u\n", acceptedCount);
    std::printf("Debounce ile elenen:                     0 (kavram yok)\n");
    std::printf("Queue overflow (dropped):                 bilinmiyor (orijinalde yok)\n");
    std::printf("Log dosyasi: %s\n", logPath.c_str());

    close(fd);
    std::fclose(log);
    return 0;
}