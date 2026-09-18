/*
 * GeniePiLib.h
 *
 *  4D Systems Genie (Visi-Genie) seri port kutuphanesi -- AYRIK (header + cpp) surum.
 *  (GeniePi.h ile ayni kutuphanenin, .h/.cpp olarak bolunmus hali.)
 *
 *  4D Systems Genie (Visi-Genie) seri port kutuphanesi.
 *  Orijinal geniePi.c + geniePi.h (Gordon Henderson / 4D Systems) dosyalarinin
 *  BIREBIR AYNI ISLEVDE, C++ class'ina sarilmis ve hata duzeltilmis halidir.
 *
 *  Tum makrolar, struct alanlari ve fonksiyon imzalari GERCEK geniePi.h
 *  dosyasindaki degerlerle BIREBIR AYNI tutuldu.
 *
 *  DEGISEN TEK SEY:
 *   1) BUG DUZELTMESI: genieReplyListener icinde "cmd, object, index" gibi
 *      degiskenler ekrandan bayt okuma hatasinda -1 (sentinel) donebiliyor.
 *      Orijinal kodda bunlar "unsigned int" idi ve "== -1" ile
 *      karsilastiriliyordu; bu C'de "kazara" calisan ama yanlis/tehlikeli
 *      bir kaliptir. Burada bu okuma degiskenleri "int" olarak birakildi
 *      (sentinel -1 dogru temsil edilsin diye).
 *   2) Global static degiskenler + pthread yerine class uyeleri +
 *      std::thread / std::mutex kullanildi (C++ istegi geregi).
 *
 *  Fonksiyon isimleri, struct alanlari (geniePi.h ile BIREBIR AYNI),
 *  makrolar (GENIE_* sabitleri, geniePi.h ile BIREBIR AYNI), zamanlama
 *  (5ms/50ms/10s), akis mantigi (ACK/NAK bekleme, checksum XOR, ilk
 *  acilista dummy byte gonderme, magic bytes'taki null-terminated dizi
 *  davranisi) BIREBIR ORIJINALDEKI GIBI KORUNDU.
 *
 *  Kullanim:
 *      GeniePi genie;
 *      genie.genieSetup("/dev/ttyUSB0", 115200);
 *      genie.genieWriteObj(GENIE_OBJ_ILED_DIGITS_L, 0, 1234);
 */

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <condition_variable>   // Requirement 2: thread-safe queue icin
#include <deque>                // Requirement 2/4: sirali, "geri koyulabilir" kuyruk icin

// ---------------------------------------------------------------------
// Genie komutlari & cevaplari (geniePi.h ile BIREBIR AYNI degerler)
// ---------------------------------------------------------------------
#define GENIE_ACK                    0x06
#define GENIE_NAK                    0x15

#define GENIE_READ_OBJ                    0
#define GENIE_WRITE_OBJ                    1
#define GENIE_WRITE_STR                    2
#define GENIE_WRITE_STRU                   3
#define GENIE_WRITE_CONTRAST               4
#define GENIE_REPORT_OBJ                   5
#define GENIE_REPORT_EVENT                 7
#define GENIE_MAGIC_BYTES                  8
#define GENIE_DOUBLE_BYTES                 9
#define GENIE_REPORT_MAGIC_BYTES          10
#define GENIE_REPORT_DOUBLE_BYTES         11
#define GENIE_WRITE_INH_LABEL             12

// ---------------------------------------------------------------------
// Objeler (geniePi.h ile BIREBIR AYNI degerler)
// ---------------------------------------------------------------------
#define GENIE_OBJ_DIPSW                    0
#define GENIE_OBJ_KNOB                     1
#define GENIE_OBJ_ROCKERSW                 2
#define GENIE_OBJ_ROTARYSW                 3
#define GENIE_OBJ_SLIDER                   4
#define GENIE_OBJ_TRACKBAR                 5
#define GENIE_OBJ_WINBUTTON                6
#define GENIE_OBJ_ANGULAR_METER            7
#define GENIE_OBJ_COOL_GAUGE               8
#define GENIE_OBJ_CUSTOM_DIGITS            9
#define GENIE_OBJ_FORM                    10
#define GENIE_OBJ_GAUGE                   11
#define GENIE_OBJ_IMAGE                   12
#define GENIE_OBJ_KEYBOARD                13
#define GENIE_OBJ_LED                     14
#define GENIE_OBJ_LED_DIGITS              15
#define GENIE_OBJ_METER                   16
#define GENIE_OBJ_STRINGS                 17
#define GENIE_OBJ_THERMOMETER             18
#define GENIE_OBJ_USER_LED                19
#define GENIE_OBJ_VIDEO                   20
#define GENIE_OBJ_STATIC_TEXT             21
#define GENIE_OBJ_SOUND                   22
#define GENIE_OBJ_TIMER                   23
#define GENIE_OBJ_SPECTRUM                24
#define GENIE_OBJ_SCOPE                   25
#define GENIE_OBJ_TANK                    26
#define GENIE_OBJ_USERIMAGES              27
#define GENIE_OBJ_PINOUTPUT               28
#define GENIE_OBJ_PININPUT                29
#define GENIE_OBJ_4DBUTTON                30
#define GENIE_OBJ_ANIBUTTON                31
#define GENIE_OBJ_COLORPICKER              32
#define GENIE_OBJ_USERBUTTON               33
#define GENIE_OBJ_SMARTGAUGE              35
#define GENIE_OBJ_SMARTSLIDER             36
#define GENIE_OBJ_SMARTKNOB               37
#define GENIE_OBJ_ISMARTGAUGE             35
#define GENIE_OBJ_ISMARTSLIDER            36
#define GENIE_OBJ_ISMARTKNOB              37
#define GENIE_OBJ_ILED_DIGITS_H           38
#define GENIE_OBJ_IANGULAR_METER          39
#define GENIE_OBJ_IGAUGE                  40
#define GENIE_OBJ_ILABEL                  41
#define GENIE_OBJ_ILABELB                 41
#define GENIE_OBJ_IUSER_GAUGE             42
#define GENIE_OBJ_IMEDIA_GAUGE            43
#define GENIE_OBJ_IMEDIA_THERMOMETER      44
#define GENIE_OBJ_ILED                    45
#define GENIE_OBJ_IMEDIA_LED              46
#define GENIE_OBJ_ILED_DIGITS_L           47
#define GENIE_OBJ_ILED_DIGITS             47
#define GENIE_OBJ_INEEDLE                 48
#define GENIE_OBJ_IRULER                  49
#define GENIE_OBJ_ILED_DIGIT              50
#define GENIE_OBJ_IBUTTOND                51
#define GENIE_OBJ_IBUTTONE                52
#define GENIE_OBJ_IMEDIA_BUTTON           53
#define GENIE_OBJ_ITOGGLE_INPUT           54
#define GENIE_OBJ_IDIAL                   55
#define GENIE_OBJ_IMEDIA_ROTARY           56
#define GENIE_OBJ_IROTARY_INPUT           57
#define GENIE_OBJ_ISWITCH                 58
#define GENIE_OBJ_ISWITCHB                59
#define GENIE_OBJ_ISLIDERE                60
#define GENIE_OBJ_IMEDIA_SLIDER           61
#define GENIE_OBJ_ISLIDERH                62
#define GENIE_OBJ_ISLIDERG                63
#define GENIE_OBJ_ISLIDERF                64
#define GENIE_OBJ_ISLIDERD                65
#define GENIE_OBJ_ISLIDERC                66
#define GENIE_OBJ_ILINEAR_INPUT           67

#define MAX_GENIE_REPLYS                  16

// ---------------------------------------------------------------------
// Requirement 5: ACK -> basari, NAK -> protokol hatasi, timeout -> timeout
// hatasi. GENIE_OK == 0 oldugu icin eski "if (genie.genieWriteObj(...) == 0)"
// kontrolleri KIRILMADAN calismaya devam eder; sadece artik gercek hata
// kodlari da ayirt edilebiliyor (eskiden fonksiyonlar NAK gelse bile hep 0
// donuyordu, bkz. GeniePiLib.cpp'deki degisiklikler).
// ---------------------------------------------------------------------
enum GenieResult
{
    GENIE_OK             = 0,
    GENIE_ERROR_NAK       = -1,
    GENIE_ERROR_TIMEOUT   = -2,
};

// ---------------------------------------------------------------------
// Struct'lar (geniePi.h ile BIREBIR AYNI alan tipleri)
// ---------------------------------------------------------------------
struct genieReplyStruct
{
    int          cmd;
    int          object;
    int          index;
    unsigned int data;
};

struct genieMagicReplyStruct
{
    int          cmd;
    int          index;
    int          length;
    unsigned int data[100];
};

union FloatLongFrame {
    float    floatValue;
    int32_t  longValue;
    uint32_t ulongValue;
    int16_t  wordValue[2];
};

class GeniePi {
public:
    GeniePi()  = default;
    ~GeniePi() { genieClose(); }

    GeniePi(const GeniePi&)            = delete;
    GeniePi& operator=(const GeniePi&) = delete;

    int  genieSetup(char *device, int baud);
    void genieClose(void);

    // Requirement 8 (stress test) icin: gercek donaniminiz olmadan
    // socketpair() gibi onceden acilmis bir fd'yi "seri port" olarak
    // baglar ve listener thread'i baslatir. genieSetup ile ayni sonucu
    // verir, sadece genieOpen() (gercek /dev/ttyUSB0 acma) adimini atlar.
    void genieAttachFd(int fd);

    // --- Requirement 1: Configurable debounce ---
    //   genie.setDebounceTime(150);  // onerilen varsayilan
    //   genie.setDebounceTime(0);    // debounce'u tamamen kapatir
    void         setDebounceTime(unsigned int milliseconds);
    unsigned int getDebounceTime(void) const;

    // --- Requirement 5: Configurable ACK/NAK timeout ---
    void         setAckTimeout(unsigned int milliseconds); // varsayilan 500ms
    unsigned int getAckTimeout(void) const;

    // --- Requirement 3: Queue overflow gozlemlenebilirligi ---
    uint32_t getDroppedEventCount(void) const;

    int  genieReplyAvail(void);
    void genieGetReply(struct genieReplyStruct *reply);

    int genieReadObj (int object, int index);
    int genieWriteObj (int object, int index, unsigned int data);

    int genieWriteShortToIntLedDigits (int index, int16_t data);
    int genieWriteLongToIntLedDigits  (int index, int32_t data);
    int genieWriteFloatToIntLedDigits (int index, float data);

    int genieWriteContrast (int value);

    int genieWriteStr  (int index, char *string);
    int genieWriteStrU (int index, char *string);

    int genieWriteStrHex  (int index, long n);
    int genieWriteStrDec  (int index, long n);
    int genieWriteStrOct  (int index, long n);
    int genieWriteStrBin  (int index, long n);
    int genieWriteStrBase (int index, long n, int base);
    int genieWriteStrFloat(int index, float n, int precision);

    int genieWriteInhLabelDefault (int index);
    int genieWriteInhLabel        (int index, char *string);
    int genieWriteInhLabelHex     (int index, long n);
    int genieWriteInhLabelDec     (int index, long n);
    int genieWriteInhLabelOct     (int index, long n);
    int genieWriteInhLabelBin     (int index, long n);
    int genieWriteInhLabelBase    (int index, long n, int base);
    int genieWriteInhLabelFloat   (int index, float n, int precision);

    int genieWriteMagicBytes  (int magic_index, unsigned int *byteArray);
    int genieWriteDoubleBytes (int magic_index, unsigned int *doubleByteArray);

    int genieChecksumErrors = 0;
    int genieTimeouts       = 0;

private:
    int  genieOpen (char *device, int baud);
    void genieFlush (int fd);
    int  genieDataAvail (int fd);

    unsigned int millis (void);
    void delay (unsigned int howLong);
    void delayMicroseconds (unsigned int howLong);

    int  genieGetchar (void);
    void geniePutchar (int data);

    void genieReplyListener (void);

    int  _genieReadObj (int object, int index);
    int  _genieWriteObj (int object, int index, unsigned int data);
    int  _genieWriteContrast (int value);
    int  _genieWriteStr (int index, char *string);
    int  _genieWriteStrU (int index, char *string);
    int  _genieMakeStr (int index, long n, int base);
    int  _genieWriteStrFloat (int index, float n, int precision);
    int  _genieWriteInhLabel (int index, char *string);
    int  _genieMakeInhLabel (int index, long n, int base);
    int  _genieWriteInhLabelFloat (int index, float n, int precision);
    int  _genieWriteMagicBytes (int magic_index, unsigned int *byteArray);
    int  _genieWriteDoubleBytes (int magic_index, unsigned int *doubleByteArray);

    // --- Requirement 1: debounce karari ---
    bool shouldDebounce(int object, int index, unsigned int data,
                         std::chrono::steady_clock::time_point now);

    // --- Requirement 5: ACK/NAK'i condition_variable ile, TIMEOUT'lu bekler ---
    int  waitForAck(void);

    // --- Requirement 4: genieReadObj sirasinda "ilgisiz" olarak biriktirilen
    // event'leri, sirayi bozmadan kuyrugun basina geri koyar ---
    void requeueFront(std::deque<genieReplyStruct> &items);

private:
    // ===================================================================
    // Requirement 2/3/4: Thread-Safe Reply/Event Queue
    //
    // ESKI: sabit boyutlu genieReplys[MAX_GENIE_REPLYS] dizisi + duz int
    // genieReplysHead/genieReplysTail. Listener thread head'i, application
    // thread tail'i degistiriyordu; aralarinda HICBIR senkronizasyon
    // (mutex/atomic) yoktu -> veri yarisi (data race).
    //
    // YENI: std::deque + mutex + condition_variable. deque secildi cünkü
    // Requirement 4 (genieReadObj event kaybini engelleme) icin, kendi
    // cevabini beklerken karsilasilan ilgisiz mesajlarin (ör. button
    // event'i) kuyrugun BASINA, SIRASI BOZULMADAN geri konulmasi gerekiyor;
    // bu sabit ring buffer'da dogal degil, deque'de trivial (insert(begin,...)).
    // ===================================================================
    std::mutex                        replyQueueMutex;
    std::condition_variable           replyQueueCv;
    std::deque<genieReplyStruct>      replyQueue;
    std::deque<genieMagicReplyStruct> magicReplyQueue;
    static constexpr size_t GENIE_QUEUE_CAPACITY = 256; // eski 16'dan buyutuldu

    // Requirement 3: overflow artik sessiz degil, gozlemlenebilir.
    std::atomic<uint32_t> droppedEvents{0};

    // ===================================================================
    // Requirement 1: Button Event Debounce
    //
    // BILEREK tekil ("son kabul edilen event") bir hafiza tutuluyor -
    // object+index bazinda bir HARITA degil. Sebep: Event Ordering
    // (Requirement 6) ile celisiyor olmasi -- Button1 -> Button2 -> Button1
    // -> Button3 sirasinda ikinci Button1'in, arada Button2 kabul edildigi
    // icin YENIDEN "yeni" sayilmasi gerekiyor. Karar SADECE zaman damgasi
    // karsilastirmasi ile veriliyor; sleep()/usleep()/delay() YOK.
    // ===================================================================
    struct LastEvent {
        int object = -1, index = -1;
        unsigned int data = 0;
        std::chrono::steady_clock::time_point ts{};
        bool valid = false;
    };
    std::mutex   debounceMutex;
    LastEvent    lastEvent;
    std::atomic<unsigned int> debounceMs{150}; // onerilen varsayilan

    // ===================================================================
    // Requirement 5: ACK/NAK Timeout
    //
    // genieAck/genieNak atomic bayraklari KALDI (API'yi bozmamak icin),
    // ama artik "while(...) delay(1)" ile degil, condition_variable ile
    // ve bir TIMEOUT ile bekleniyor (bkz. waitForAck()).
    // ===================================================================
    std::atomic<bool> genieAck{false};
    std::atomic<bool> genieNak{false};
    std::mutex             ackMutex;
    std::condition_variable ackCv;
    std::atomic<unsigned int> ackTimeoutMs{500}; // onerilen varsayilan

    std::mutex  genieMutex;
    int         genieFd = -1;

    unsigned long long epoch = 0;

    std::thread       listenerThread;
    std::atomic<bool> listenerRunning{false};
};