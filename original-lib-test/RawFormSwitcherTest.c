#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include "geniePi.h"

#define SERIAL_PORT "/dev/ttyUSB0"
#define BAUD_RATE   9600

static volatile int running = 1;
static void sig_handler(int sig) {
    (void)sig;
    running = 0;
}

static unsigned long long current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)(tv.tv_sec) * 1000 + (unsigned long long)(tv.tv_usec) / 1000;
}

int main(void) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("==============================================================\n");
    printf("  ORIJINAL GENIE-PI HAM EVENT VE DEBOUNCE TESTI               \n");
    printf("==============================================================\n");

    // 1. Port başlatılır
    if (genieSetup(SERIAL_PORT, BAUD_RATE) < 0) {
        fprintf(stderr, "[HATA] genieSetup basarisiz oldu!\n");
        return 1;
    }

    printf("[BASARILI] Orijinal geniePi baglantisi acildi.\n");
    printf("Ekrandaki butona dokunun (Cikis: Ctrl + C)...\n\n");

    // DİKKAT: Ekranı kilitleyen ilk form yazma komutunu ÇIKARDIK.
    // Ekran açık kalacak ve doğrudan buton dinlemeye geçilecek.

    unsigned long total_events = 0;
    unsigned long long last_time = 0;
    int current_form = 0;
    struct genieReplyStruct reply;

    while (running) { 
        while (genieReplyAvail()) {
            genieGetReply(&reply);

            // Buton event'i geldiğinde (UserButton: 0x21 veya WinButton: 0x06)
            if (reply.cmd == GENIE_REPORT_EVENT &&
               (reply.object == GENIE_OBJ_USERBUTTON || reply.object == GENIE_OBJ_WINBUTTON || reply.object == 0x21)) {
                
                total_events++;
                unsigned long long now = current_time_ms();
                unsigned long diff = (last_time == 0) ? 0 : (unsigned long)(now - last_time);
                last_time = now;

                printf("<- [HAM EVENT #%lu] Obj: 0x%02X, Idx: %d, Deger: %u | Sure: %lu ms\n",
                       total_events, reply.object, reply.index, reply.data, diff);

                if (diff > 0 && diff < 300) {
                    printf("   >>> [BOUNCE TESPIT EDILDI]: %lu ms arayla geldi, DEBOUNCE OLMADIGI ICIN ELENMEDI!\n", diff);
                }

                // Form geçişi tetiklemesi:
                current_form = (current_form == 0) ? 1 : 0;
                printf("   --> Form %d komutu gonderiliyor...\n", current_form);
                
                // Form komutu:
                genieWriteObj(GENIE_OBJ_FORM, current_form, 0);
                printf("   --> Form %d komutu tamamlandi.\n\n", current_form);
            }
        }
    
    }

    printf("\n\nTest bitti. Toplam Ham Event: %lu\n", total_events);
    return 0;
}