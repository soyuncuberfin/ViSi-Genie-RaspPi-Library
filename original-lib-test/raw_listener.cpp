#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

int main(int argc, char* argv[]) {
    const char* port = (argc > 1) ? argv[1] : "/dev/ttyUSB0";
    speed_t baud = B115200; // Ekranın Workshop4 yapılandırma hızı

    int fd = open(port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("Port acilamadi");
        return 1;
    }

    struct termios tty;
    tcgetattr(fd, &tty);
    cfmakeraw(&tty);
    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);
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

    printf("Port acildi (%s @ 115200 Baud). Ekran acik kalmali.\n", port);
    printf("Butona basiniz, gelen baytlar yazdirilacak (Ctrl+C ile cikis)...\n");

    unsigned char buf[64];
    while (1) {
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            printf("Gelen (%d byte): ", n);
            for (int i = 0; i < n; i++) {
                printf("0x%02X ", buf[i]);
            }
            printf("\n");
            fflush(stdout);
        }
        usleep(10000);
    }

    close(fd);
    return 0;
}