cat << 'EOF' > main.cpp
#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <chrono>
#include <cstdint>
#include "genie_objects.h"

#define GENIE_OBJ_USERBUTTON  0x21
#define GENIE_OBJ_LED_DIGITS   0x0F
#define GENIE_REPORT_EVENT     0x07
#define GENIE_WRITE_OBJ        0x01

int fd = -1;

void send_digit(uint8_t index, uint16_t value) {
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

    write(fd, cmd, 6);
}

int main(int argc, char* argv[]) {
    const char* port = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    fd = open(port, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) {
        std::cerr << "Hata: Port açılamadı: " << port << std::endl;
        return 1;
    }

    struct termios tty;
    tcgetattr(fd, &tty);
    cfsetospeed(&tty, B115200);
    cfsetispeed(&tty, B115200);
    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8 | CLOCAL | CREAD;
    tty.c_iflag = tty.c_oflag = tty.c_lflag = 0;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;
    tcsetattr(fd, TCSANOW, &tty);

    std::cout << "Port acildi: " << port << " (115200 Baud)" << std::endl;
    std::cout << "Debounce testi baslatildi. Butona basiniz..." << std::endl;

    int raw_count = 0;
    int accepted_count = 0;
    int rejected_count = 0;

    auto last_press_time = std::chrono::steady_clock::now();
    const auto debounce_threshold = std::chrono::milliseconds(200);

    // Ekrandaki göstergeleri sıfırla
    send_digit(LEDDIGITS_DIGIT_RAW, 0);
    send_digit(LEDDIGITS_DIGIT_ACCEPTED, 0);
    send_digit(LEDDIGITS_DIGIT_REJECTED, 0);

    uint8_t buf[6];
    while (true) {
        int n = read(fd, buf, 1);
        if (n > 0 && buf[0] == GENIE_REPORT_EVENT) {
            int read_bytes = 1;
            while (read_bytes < 6) {
                int r = read(fd, &buf[read_bytes], 6 - read_bytes);
                if (r > 0) read_bytes += r;
            }

            uint8_t obj_type = buf[1];
            uint8_t obj_index = buf[2];
            uint16_t val = (buf[3] << 8) | buf[4];

            if (obj_index == USERBUTTON_BTN_TEST && val == 1) { // Butona basıldı
                raw_count++;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_press_time);

                if (elapsed >= debounce_threshold || raw_count == 1) {
                    accepted_count++;
                    last_press_time = now;
                    std::cout << "[KABUL] Basim: " << accepted_count 
                              << " (Gecen sure: " << elapsed.count() << "ms)" << std::endl;
                } else {
                    rejected_count++;
                    std::cout << "[RED - Sekme] Gecen sure: " << elapsed.count() 
                              << "ms (< 200ms)" << std::endl;
                }

                // Ekrandaki göstergeleri güncelle
                send_digit(LEDDIGITS_DIGIT_RAW, raw_count);
                send_digit(LEDDIGITS_DIGIT_ACCEPTED, accepted_count);
                send_digit(LEDDIGITS_DIGIT_REJECTED, rejected_count);
            }
        }
    }

    close(fd);
    return 0;
}
EOF