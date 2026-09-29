/*
 * GeniePiLib.cpp
 *
 *  GeniePiLib.h implementasyonu. (GeniePi.h header-only surumuyle
 *  BIREBIR AYNI davranisa sahiptir, sadece ayri derleniyor.)
 */

#include "GeniePiLib.h"



// ===========================================================================
// Requirement 5: ACK/NAK Timeout
//
// waitForAck() eski "while (!genieAck.load() && !genieNak.load()) delay(1);"
// desenini TEK bir yerde topluyor: artik busy-wait yerine condition_variable
// kullaniyor VE bir timeout'a sahip. ACK gelirse GENIE_OK, NAK gelirse
// GENIE_ERROR_NAK, sure dolarsa GENIE_ERROR_TIMEOUT donuyor.
// ===========================================================================
int GeniePi::waitForAck(void)
{
    std::unique_lock<std::mutex> lock(ackMutex);
    unsigned int timeout = ackTimeoutMs.load();
    bool signaled = ackCv.wait_for(lock, std::chrono::milliseconds(timeout),
                                    [this] { return genieAck.load() || genieNak.load(); });
    if (!signaled)
        return GENIE_ERROR_TIMEOUT; // artik SONSUZA KADAR beklemiyor
    return genieNak.load() ? GENIE_ERROR_NAK : GENIE_OK;
}

// ===========================================================================
// Requirement 1: Button Event Debounce (bkz. GeniePiLib.h'deki aciklama:
// tek/global "son kabul edilen event" - object+index bazli harita DEGIL)
// ===========================================================================
bool GeniePi::shouldDebounce(int object, int index, unsigned int data,
                              std::chrono::steady_clock::time_point now)
{
    unsigned int window = debounceMs.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(debounceMutex);

    if (window > 0 && lastEvent.valid &&
        lastEvent.object == object && lastEvent.index == index && lastEvent.data == data)
    {
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEvent.ts).count();
        if (elapsedMs >= 0 && (unsigned long long)elapsedMs < window)
            return true; // gercek duplicate: ayni obj+idx+data, pencere icinde, arada baska event yok
    }

    lastEvent = LastEvent{object, index, data, now, true};
    return false;
}

void GeniePi::setDebounceTime(unsigned int milliseconds) { debounceMs.store(milliseconds, std::memory_order_relaxed); }
unsigned int GeniePi::getDebounceTime(void) const { return debounceMs.load(std::memory_order_relaxed); }

void GeniePi::setAckTimeout(unsigned int milliseconds) { ackTimeoutMs.store(milliseconds); }
unsigned int GeniePi::getAckTimeout(void) const { return ackTimeoutMs.load(); }

uint32_t GeniePi::getDroppedEventCount(void) const { return droppedEvents.load(std::memory_order_relaxed); }

uint32_t GeniePi::getRawEventCount(void) const { return rawEventCount.load(std::memory_order_relaxed); }
uint32_t GeniePi::getDebouncedEventCount(void) const { return debouncedEventCount.load(std::memory_order_relaxed); }
size_t GeniePi::getQueueSize(void) { std::lock_guard<std::mutex> lock(replyQueueMutex); return replyQueue.size(); }


// ===========================================================================
// Requirement 4: genieReadObj'nin ilgisiz bulup biriktirdigi event'leri
// SIRAYI BOZMADAN kuyrugun basina geri koyar.
// ===========================================================================
void GeniePi::requeueFront(std::deque<genieReplyStruct> &items)
{
    if (items.empty()) return;
    std::lock_guard<std::mutex> lock(replyQueueMutex);
    replyQueue.insert(replyQueue.begin(), items.begin(), items.end());
    replyQueueCv.notify_all();
}

int GeniePi::genieOpen(char *device, int baud)
{
    struct termios options;
    speed_t myBaud;
    int status, fd;

    switch (baud)
    {
        case     50: myBaud =     B50; break;
        case     75: myBaud =     B75; break;
        case    110: myBaud =    B110; break;
        case    134: myBaud =    B134; break;
        case    150: myBaud =    B150; break;
        case    200: myBaud =    B200; break;
        case    300: myBaud =    B300; break;
        case    600: myBaud =    B600; break;
        case   1200: myBaud =   B1200; break;
        case   1800: myBaud =   B1800; break;
        case   2400: myBaud =   B2400; break;
        case   9600: myBaud =   B9600; break;
        case  19200: myBaud =  B19200; break;
        case  38400: myBaud =  B38400; break;
        case  57600: myBaud =  B57600; break;
        case 115200: myBaud = B115200; break;
        case 230400: myBaud = B230400; break;
        default:
            return -2;
    }

    if ((fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK)) == -1)
        return -1;

    fcntl(fd, F_SETFL, O_RDWR);

    tcgetattr(fd, &options);

    cfmakeraw(&options);
    cfsetispeed(&options, myBaud);
    cfsetospeed(&options, myBaud);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    options.c_oflag &= ~OPOST;

    options.c_cc[VMIN]  = 0;
    options.c_cc[VTIME] = 100;

    tcsetattr(fd, TCSANOW | TCSAFLUSH, &options);
    int modem_lines = 0;
ioctl(fd, TIOCMGET, &modem_lines);
modem_lines &= ~(TIOCM_DTR | TIOCM_RTS);
ioctl(fd, TIOCMSET, &modem_lines);

    // ioctl(fd, TIOCMGET, &status);
    //status |= TIOCM_DTR;
    //status |= TIOCM_RTS;
    //ioctl(fd, TIOCMSET, &status);

    usleep(10000);

    return fd;
}

void GeniePi::genieFlush(int fd)
{
    tcflush(fd, TCIOFLUSH);
}

void GeniePi::genieClose(void)
{
    listenerRunning.store(false);
    if (listenerThread.joinable())
        listenerThread.join();

    if (genieFd != -1)
    {
        close(genieFd);
        genieFd = -1;
    }
}

int GeniePi::genieDataAvail(int fd)
{
    int result;
    if (ioctl(fd, FIONREAD, &result) == -1)
        return -1;
    return result;
}

unsigned int GeniePi::millis(void)
{
    struct timeval tv;
    unsigned long long t1;

    gettimeofday(&tv, NULL);
    t1 = (tv.tv_sec * 1000000ULL + tv.tv_usec) / 1000;

    return (unsigned int)(t1 - epoch);
}

void GeniePi::delay(unsigned int howLong)
{
    struct timespec sleeper, dummy;
    sleeper.tv_sec  = (time_t)(howLong / 1000);
    sleeper.tv_nsec = (long)(howLong % 1000) * 1000000;
    nanosleep(&sleeper, &dummy);
}

void GeniePi::delayMicroseconds(unsigned int howLong)
{
    struct timespec sleeper, dummy;
    sleeper.tv_sec  = 0;
    sleeper.tv_nsec = (long)(howLong * 1000);
    nanosleep(&sleeper, &dummy);
}

int GeniePi::genieGetchar(void)
{
    unsigned int timeUp = millis() + 5;
    unsigned char x;

    while (millis() < timeUp)
    {
        if (genieDataAvail(genieFd))
        {
            if (read(genieFd, &x, 1) == 1)
                return ((int)x) & 0xFF;
            return -1;
        }
        else
            delayMicroseconds(101);
    }
    return -1;
}

void GeniePi::geniePutchar(int data)
{
    unsigned char c = (unsigned char)data;
    write(genieFd, &c, 1);
}

void GeniePi::genieReplyListener(void)
{
    unsigned int totalLength = 0, readLength;
    int byteData[100];

    while (genieFd == -1)
        delay(1);

    while (listenerRunning.load())
    {
        int cmd;
        while ((cmd = genieGetchar()) == -1)
        {
            if (!listenerRunning.load())
                return;
        }

        if (cmd == GENIE_ACK) { genieAck.store(true); ackCv.notify_all(); continue; }
        if (cmd == GENIE_NAK) { genieNak.store(true); ackCv.notify_all(); continue; }

        unsigned char csum = (unsigned char)cmd;

        int object = genieGetchar();
        if (object == -1) { ++genieTimeouts; continue; }
        csum ^= (unsigned char)object;

        int index = genieGetchar();
        if (index == -1) { ++genieTimeouts; continue; }
        csum ^= (unsigned char)index;

        int msb = 0, lsb = 0;
        bool timedOut = false;

        if (cmd == GENIE_REPORT_MAGIC_BYTES || cmd == GENIE_REPORT_DOUBLE_BYTES)
        {
            totalLength = (unsigned int)index;
            if (cmd == GENIE_REPORT_DOUBLE_BYTES)
                totalLength = (unsigned int)index * 2;

            for (readLength = 0; readLength < totalLength; readLength++)
            {
                byteData[readLength] = genieGetchar();
                if (byteData[readLength] == -1) { ++genieTimeouts; timedOut = true; break; }
                csum ^= (unsigned char)byteData[readLength];
            }
            if (timedOut)
                continue;
        }
        else
        {
            msb = genieGetchar();
            if (msb == -1) { ++genieTimeouts; continue; }
            csum ^= (unsigned char)msb;

            lsb = genieGetchar();
            if (lsb == -1) { ++genieTimeouts; continue; }
            csum ^= (unsigned char)lsb;
        }

        int recvChecksum = genieGetchar();
        if (recvChecksum == -1 || (unsigned char)recvChecksum != csum)
        {
            ++genieChecksumErrors;
            continue;
        }

        if (cmd == GENIE_REPORT_MAGIC_BYTES || cmd == GENIE_REPORT_DOUBLE_BYTES)
        {
            genieMagicReplyStruct magicByteReply;
            magicByteReply.cmd    = cmd;
            magicByteReply.index  = object;
            magicByteReply.length = index;

            if (cmd == GENIE_REPORT_MAGIC_BYTES)
            {
                for (readLength = 0; readLength < totalLength; readLength++)
                    magicByteReply.data[readLength] = (unsigned int)byteData[readLength];
            }
            if (cmd == GENIE_REPORT_DOUBLE_BYTES)
            {
                for (readLength = 1; readLength < totalLength; readLength++)
                    magicByteReply.data[readLength] =
                        (unsigned int)((byteData[readLength * 2] << 8) |
                                        byteData[(readLength * 2) + 1]);
            }

            // Requirement 3: kuyruk doluysa event artik SESSIZCE degil,
            // GOZLEMLENEBILIR sekilde (droppedEvents sayaci) discard edilir.
            std::lock_guard<std::mutex> lock(replyQueueMutex);
            if (magicReplyQueue.size() < GENIE_QUEUE_CAPACITY)
                magicReplyQueue.push_back(magicByteReply);
            else
                droppedEvents.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            unsigned int data = (unsigned int)((msb << 8) | lsb);
            auto now = std::chrono::steady_clock::now();

            // Fiziksel dogrulama testi: "ham" sayac, debounce karari
            // verilmeden ONCE, her gecerli (checksum dogrulanmis)
            // GENIE_REPORT_EVENT icin artiyor - boylece debounce tarafindan
            // elenen bir event bile host tarafinda SAYILABILIYOR (sadece
            // event queue'ya girmiyor).
            if (cmd == GENIE_REPORT_EVENT)
                rawEventCount.fetch_add(1, std::memory_order_relaxed);



            // Requirement 1: debounce SADECE GENIE_REPORT_EVENT (button/
            // slider/switch/rockersw gibi widget event'leri) icin uygulanir;
            // GENIE_REPORT_OBJ (genieReadObj cevaplari) debounce'a TABI DEGIL.
            if (cmd == GENIE_REPORT_EVENT &&
                shouldDebounce(object, index, data, now))
            {
                 debouncedEventCount.fetch_add(1, std::memory_order_relaxed);

                continue; // duplicate rapid press -> discard, kuyruga hic girmiyor
            }

            genieReplyStruct reply;
            reply.cmd    = cmd;
            reply.object = object;
            reply.index  = index;
            reply.data   = data;

            std::unique_lock<std::mutex> lock(replyQueueMutex);
            if (replyQueue.size() < GENIE_QUEUE_CAPACITY)
            {
                replyQueue.push_back(reply);
                lock.unlock();
                replyQueueCv.notify_all(); // Requirement 2: bekleyen application thread'i uyandir
            }
            else
            {
                droppedEvents.fetch_add(1, std::memory_order_relaxed); // Requirement 3
            }
        }
    }
}

int GeniePi::genieReplyAvail(void)
{
    std::lock_guard<std::mutex> lock(replyQueueMutex);
    return !replyQueue.empty();
}

void GeniePi::genieGetReply(struct genieReplyStruct *reply)
{
    // Requirement 2: artik "while(!avail) delay(1)" ile busy-wait YOK.
    // condition_variable, listener push yaptiginda bizi dogrudan uyandirir.
    std::unique_lock<std::mutex> lock(replyQueueMutex);
    replyQueueCv.wait(lock, [this] { return !replyQueue.empty(); });
    *reply = replyQueue.front();
    replyQueue.pop_front();
}

int GeniePi::_genieReadObj(int object, int index)
{
    struct genieReplyStruct reply;
    unsigned int timeUp;
    unsigned char checksum;

    // Requirement 4: ESKI KOD burada "while (genieReplyAvail())
    // genieGetReply(&reply);" ile kuyrukta bekleyen HER SEYI (asenkron
    // button event'leri dahil) cagri basinda sessizce siliyordu. Bu iki
    // satir TAMAMEN KALDIRILDI - artik hicbir sey onceden temizlenmiyor.
    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_READ_OBJ); checksum  = GENIE_READ_OBJ;
    geniePutchar(object);         checksum ^= (unsigned char)object;
    geniePutchar(index);          checksum ^= (unsigned char)index;
    geniePutchar(checksum);

    // Requirement 4/6: kendi cevabimizi beklerken karsilastigimiz ILGISIZ
    // mesajlari (ozellikle GENIE_REPORT_EVENT) burada geciciye topluyoruz;
    // asla dogrudan atmiyoruz. Fonksiyon donmeden once bunlari SIRAYI
    // BOZMADAN kuyrugun basina geri koyacagiz (requeueFront).
    std::deque<genieReplyStruct> unrelated;

    for (timeUp = millis() + 50; millis() < timeUp; )
    {
        if (genieNak.load())
        {
            requeueFront(unrelated);
            return GENIE_ERROR_NAK;
        }

        if (genieReplyAvail())
        {
            genieGetReply(&reply);
            if ((reply.cmd == GENIE_REPORT_OBJ) && (reply.object == object) && (reply.index == index))
            {
                requeueFront(unrelated); // ilgisizleri geri koy, SONRA don
                return reply.data;
            }
            unrelated.push_back(reply); // ör. bir button event'i -> KAYBETME
        }
        delayMicroseconds(101);
    }
    requeueFront(unrelated);
    return GENIE_ERROR_TIMEOUT;
}

int GeniePi::genieReadObj(int object, int index)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieReadObj(object, index);
}

int GeniePi::_genieWriteObj(int object, int index, unsigned int data)
{
    unsigned char checksum, msb, lsb;

    lsb = (unsigned char)((data >> 0) & 0xFF);
    msb = (unsigned char)((data >> 8) & 0xFF);

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_WRITE_OBJ); checksum  = GENIE_WRITE_OBJ;
    geniePutchar(object);          checksum ^= (unsigned char)object;
    geniePutchar(index);           checksum ^= (unsigned char)index;
    geniePutchar(msb);             checksum ^= msb;
    geniePutchar(lsb);             checksum ^= lsb;
    geniePutchar(checksum);

    return waitForAck(); // Requirement 5: ACK->GENIE_OK, NAK->GENIE_ERROR_NAK, timeout->GENIE_ERROR_TIMEOUT
}

int GeniePi::genieWriteObj(int object, int index, unsigned int data)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteObj(object, index, data);
}

int GeniePi::genieWriteShortToIntLedDigits(int index, int16_t data)
{
    return genieWriteObj(GENIE_OBJ_ILED_DIGITS_L, index, data);
}

int GeniePi::genieWriteFloatToIntLedDigits(int index, float data)
{
    union FloatLongFrame frame;
    frame.floatValue = data;
    int retval = genieWriteObj(GENIE_OBJ_ILED_DIGITS_H, index, frame.wordValue[1]);
    if (retval != 1) return retval;
    return genieWriteObj(GENIE_OBJ_ILED_DIGITS_L, index, frame.wordValue[0]);
}

int GeniePi::genieWriteLongToIntLedDigits(int index, int32_t data)
{
    union FloatLongFrame frame;
    frame.longValue = data;
    int retval = genieWriteObj(GENIE_OBJ_ILED_DIGITS_H, index, frame.wordValue[1]);
    if (retval != 1) return retval;
    return genieWriteObj(GENIE_OBJ_ILED_DIGITS_L, index, frame.wordValue[0]);
}

int GeniePi::_genieWriteContrast(int value)
{
    unsigned char checksum;

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_WRITE_CONTRAST); checksum  = GENIE_WRITE_CONTRAST;
    geniePutchar(value);                checksum ^= (unsigned char)value;
    geniePutchar(checksum);

    return waitForAck();
}

int GeniePi::genieWriteContrast(int value)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteContrast(value);
}

int GeniePi::_genieWriteStr(int index, char *string)
{
    unsigned char checksum;
    int len = (int)strlen(string);

    if (len > 255)
        return -1;

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_WRITE_STR); checksum  = GENIE_WRITE_STR;
    geniePutchar(index);           checksum ^= (unsigned char)index;
    geniePutchar((unsigned char)len); checksum ^= (unsigned char)len;
    for (char *p = string; *p; ++p)
    {
        geniePutchar(*p);
        checksum ^= (unsigned char)*p;
    }
    geniePutchar(checksum);

    return waitForAck();
}

int GeniePi::genieWriteStr(int index, char *string)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteStr(index, string);
}

int GeniePi::_genieWriteStrU(int index, char *string)
{
    unsigned char checksum;
    int len = (int)strlen(string);

    if (len > 255)
        return -1;

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_WRITE_STRU); checksum  = GENIE_WRITE_STRU;
    geniePutchar(index);            checksum ^= (unsigned char)index;
    geniePutchar((unsigned char)len); checksum ^= (unsigned char)len;
    for (char *p = string; *p; ++p)
    {
        geniePutchar((*p) >> 8);   checksum ^= (unsigned char)((*p) >> 8);
        geniePutchar((*p) & 0xFF); checksum ^= (unsigned char)((*p) & 0xFF);
    }
    geniePutchar(checksum);

    return waitForAck();
}

int GeniePi::genieWriteStrU(int index, char *string)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteStrU(index, string);
}

int GeniePi::_genieMakeStr(int index, long n, int base)
{
    char buf[8 * sizeof(long) + 1];
    char *str = &buf[sizeof(buf) - 1];
    int neg = 0;
    if (n < 0) neg = 1;
    n = std::labs(n);

    *str = '\0';
    do {
        unsigned long m = n;
        n /= base;
        char c = (char)(m - base * n);
        *--str = c < 10 ? c + '0' : c + 'A' - 10;
    } while (n);
    if (neg) *--str = '-';

    // Kucuk ek duzeltme: bu fonksiyon _genieWriteStr'nin GERCEK ACK/NAK/
    // timeout sonucunu artik yutmuyor, oldugu gibi yukari tasiyor
    // (Requirement 5'in ruhuna uygun olsun diye).
    return _genieWriteStr(index, str);
}

int GeniePi::genieWriteStrHex(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeStr(index, n, 16);
}
int GeniePi::genieWriteStrOct(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeStr(index, n, 8);
}
int GeniePi::genieWriteStrBin(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeStr(index, n, 2);
}
int GeniePi::genieWriteStrBase(int index, long n, int base)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeStr(index, n, base);
}
int GeniePi::genieWriteStrDec(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeStr(index, n, 10);
}

int GeniePi::_genieWriteStrFloat(int index, float n, int precision)
{
    char str[64];
    std::snprintf(str, sizeof(str), "%.*g", precision, (double)n);
    return _genieWriteStr(index, str); // Requirement 5: gercek sonucu yukari tasi
}

int GeniePi::genieWriteStrFloat(int index, float n, int precision)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteStrFloat(index, n, precision);
}

int GeniePi::genieWriteInhLabelDefault(int index)
{
    return genieWriteObj(GENIE_OBJ_ILABELB, index, (unsigned int)-1);
}

int GeniePi::_genieWriteInhLabel(int index, char *string)
{
    unsigned char checksum;
    int len = (int)strlen(string);

    if (len > 255)
        return -1;

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_WRITE_INH_LABEL); checksum  = GENIE_WRITE_INH_LABEL;
    geniePutchar(index);                 checksum ^= (unsigned char)index;
    geniePutchar((unsigned char)len);    checksum ^= (unsigned char)len;
    for (char *p = string; *p; ++p)
    {
        geniePutchar(*p);
        checksum ^= (unsigned char)*p;
    }
    geniePutchar(checksum);

    return waitForAck();
}

int GeniePi::genieWriteInhLabel(int index, char *string)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteInhLabel(index, string);
}

int GeniePi::_genieMakeInhLabel(int index, long n, int base)
{
    char buf[8 * sizeof(long) + 1];
    char *str = &buf[sizeof(buf) - 1];
    int neg = 0;
    if (n < 0) neg = 1;
    n = std::labs(n);

    *str = '\0';
    do {
        unsigned long m = n;
        n /= base;
        char c = (char)(m - base * n);
        *--str = c < 10 ? c + '0' : c + 'A' - 10;
    } while (n);
    if (neg) *--str = '-';

    return _genieWriteInhLabel(index, str); // Requirement 5: gercek sonucu yukari tasi
}

int GeniePi::genieWriteInhLabelHex(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeInhLabel(index, n, 16);
}
int GeniePi::genieWriteInhLabelOct(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeInhLabel(index, n, 8);
}
int GeniePi::genieWriteInhLabelBin(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeInhLabel(index, n, 2);
}
int GeniePi::genieWriteInhLabelBase(int index, long n, int base)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeInhLabel(index, n, base);
}
int GeniePi::genieWriteInhLabelDec(int index, long n)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieMakeInhLabel(index, n, 10);
}

int GeniePi::_genieWriteInhLabelFloat(int index, float n, int precision)
{
    char str[64];
    std::snprintf(str, sizeof(str), "%.*g", precision, (double)n);
    return _genieWriteInhLabel(index, str); // Requirement 5: gercek sonucu yukari tasi
}

int GeniePi::genieWriteInhLabelFloat(int index, float n, int precision)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteInhLabelFloat(index, n, precision);
}

int GeniePi::_genieWriteMagicBytes(int magic_index, unsigned int *byteArray)
{
    unsigned int *p;
    unsigned char checksum;
    int len = 0;

    // byteArray null (0) ile biten bir dizi; gercek eleman sayisini
    // pointer boyutundan degil, diziyi gezerek buluyoruz.
    for (p = byteArray; *p; ++p)
        ++len;

    if (len > 255)
        return -1;

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_MAGIC_BYTES); checksum  = GENIE_MAGIC_BYTES;
    geniePutchar(magic_index);       checksum ^= (unsigned char)magic_index;
    geniePutchar((unsigned char)len); checksum ^= (unsigned char)len;
    for (p = byteArray; *p; ++p)
    {
        geniePutchar((int)*p);
        checksum ^= (unsigned char)*p;
    }
    geniePutchar(checksum);

    return waitForAck();
}

int GeniePi::genieWriteMagicBytes(int magic_index, unsigned int *byteArray)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteMagicBytes(magic_index, byteArray);
}

int GeniePi::_genieWriteDoubleBytes(int magic_index, unsigned int *doubleByteArray)
{
    unsigned int *p;
    unsigned char checksum;
    int len = 0;

    // doubleByteArray null (0) ile biten bir dizi; gercek eleman sayisini
    // pointer boyutundan degil, diziyi gezerek buluyoruz.
    for (p = doubleByteArray; *p; ++p)
        ++len;

    if (len > 255)
        return -1;

    genieAck.store(false);
    genieNak.store(false);

    geniePutchar(GENIE_DOUBLE_BYTES); checksum  = GENIE_MAGIC_BYTES;
    geniePutchar(magic_index);        checksum ^= (unsigned char)magic_index;
    geniePutchar((unsigned char)len); checksum ^= (unsigned char)len;
    for (p = doubleByteArray; *p; ++p)
    {
        unsigned char hi = (unsigned char)((*p) >> 8);
        unsigned char lo = (unsigned char)((*p) & 0xFF);
        geniePutchar(hi); checksum ^= hi;
        geniePutchar(lo); checksum ^= lo;
    }
    geniePutchar(checksum);

    return waitForAck();
}

int GeniePi::genieWriteDoubleBytes(int magic_index, unsigned int *doubleByteArray)
{
    std::lock_guard<std::mutex> lock(genieMutex);
    return _genieWriteDoubleBytes(magic_index, doubleByteArray);
}

int GeniePi::genieSetup(char *device, int baud)
{
    struct timeval tv;

    if ((genieFd = genieOpen(device, baud)) < 0)
        return -1;

    genieFlush(genieFd);

    gettimeofday(&tv, NULL);
    epoch = (tv.tv_sec * 1000000ULL + tv.tv_usec) / 1000;

    for (int i = 0; i < 10; ++i)
    {
        geniePutchar('X');
        if (genieGetchar() == GENIE_NAK)
            break;
    }

    listenerRunning.store(true);
    listenerThread = std::thread(&GeniePi::genieReplyListener, this);

    return 0;
}

// Requirement 8: genieSetup ile ayni sonucu (fd baglanmis + listener thread
// calisir durumda) verir, ama genieOpen()/handshake adimini atlar. Boylece
// stress testleri gercek /dev/ttyUSB0 olmadan, socketpair() ile
// olusturulmus bir fd uzerinden genieReplyListener'i UCTAN UCA calistirabilir.
void GeniePi::genieAttachFd(int fd)
{
    genieFd = fd;

    struct timeval tv;
    gettimeofday(&tv, NULL);
    epoch = (tv.tv_sec * 1000000ULL + tv.tv_usec) / 1000;

    listenerRunning.store(true);
    listenerThread = std::thread(&GeniePi::genieReplyListener, this);
}