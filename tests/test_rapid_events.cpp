/*
 * test_rapid_events.cpp
 *
 * Task'in "Stress Test" bolumunde istenen Test 1..6 senaryolarini,
 * repo'nun GERCEK GeniePi sinifi uzerinde, socketpair() ile simule
 * edilmis bir seri hat uzerinden, gercek listener thread'i calistirarak
 * dogrular.
 */
#include "GeniePiLib.h"

#include <sys/socket.h>
#include <unistd.h>
#include <vector>
#include <cstdio>
#include <thread>
#include <chrono>
#include <atomic>

using namespace std::chrono;

static int g_failures = 0;
#define CHECK(cond, msg) \
    do { if (cond) std::printf("  [OK]   %s\n", msg); \
         else      { std::printf("  [FAIL] %s\n", msg); ++g_failures; } } while (0)

static void writeFrame(int fd, std::vector<uint8_t> bytes) {
    ssize_t n = write(fd, bytes.data(), bytes.size()); (void)n;
}
static std::vector<uint8_t> reportEventFrame(uint8_t object, uint8_t index, uint16_t data) {
    uint8_t msb = data >> 8, lsb = data & 0xFF;
    uint8_t csum = GENIE_REPORT_EVENT ^ object ^ index ^ msb ^ lsb;
    return {GENIE_REPORT_EVENT, object, index, msb, lsb, csum};
}
static std::vector<uint8_t> reportObjFrame(uint8_t object, uint8_t index, uint16_t data) {
    uint8_t msb = data >> 8, lsb = data & 0xFF;
    uint8_t csum = GENIE_REPORT_OBJ ^ object ^ index ^ msb ^ lsb;
    return {GENIE_REPORT_OBJ, object, index, msb, lsb, csum};
}

struct FakeLink {
    int displayFd;
    GeniePi genie;
    FakeLink() {
        int fds[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        displayFd = fds[0];
        genie.genieAttachFd(fds[1]);
        genie.setDebounceTime(150);
        genie.setAckTimeout(500);
    }
    ~FakeLink() { genie.genieClose(); close(displayFd); }
};

// Requirement 2'nin sureli-bekleme yardimcisi genieGetReply'de yok (o
// sonsuza kadar bekler), bu yuzden testte kucuk bir polling sarmalayici
// kullaniyoruz - kutuphanenin kendisi hala tam thread-safe/cv tabanli.
static bool getReplyWithTimeout(GeniePi &g, genieReplyStruct &out, int timeoutMs) {
    auto deadline = steady_clock::now() + milliseconds(timeoutMs);
    while (steady_clock::now() < deadline) {
        if (g.genieReplyAvail()) { g.genieGetReply(&out); return true; }
        std::this_thread::sleep_for(milliseconds(2));
    }
    return false;
}

static void test1_sameButtonDoublePress() {
    std::printf("Test 1: Same Button Double Press\n");
    FakeLink link;
    writeFrame(link.displayFd, reportEventFrame(10, 1, 1));
    std::this_thread::sleep_for(milliseconds(50));
    writeFrame(link.displayFd, reportEventFrame(10, 1, 1));
    std::this_thread::sleep_for(milliseconds(100));

    int count = 0; genieReplyStruct r;
    while (getReplyWithTimeout(link.genie, r, 20)) if (r.object == 10 && r.index == 1) ++count;
    CHECK(count == 1, "150ms icinde ayni event (t=0,t=50) sadece 1 kez application'a ulasti");
}

static void test2_sameButtonSlowPress() {
    std::printf("Test 2: Same Button Slow Press\n");
    FakeLink link;
    writeFrame(link.displayFd, reportEventFrame(11, 1, 1));
    std::this_thread::sleep_for(milliseconds(300));
    writeFrame(link.displayFd, reportEventFrame(11, 1, 1));
    std::this_thread::sleep_for(milliseconds(100));

    int count = 0; genieReplyStruct r;
    while (getReplyWithTimeout(link.genie, r, 20)) if (r.object == 11 && r.index == 1) ++count;
    CHECK(count == 2, "300ms arayla gelen ayni event'in HER IKISI de alindi");
}

static void test3_differentButtons() {
    std::printf("Test 3: Different Buttons (order preservation)\n");
    FakeLink link;
    writeFrame(link.displayFd, reportEventFrame(20, 1, 1));
    writeFrame(link.displayFd, reportEventFrame(21, 1, 1));
    writeFrame(link.displayFd, reportEventFrame(20, 1, 1));
    writeFrame(link.displayFd, reportEventFrame(22, 1, 1));
    std::this_thread::sleep_for(milliseconds(100));

    std::vector<int> order; genieReplyStruct r;
    while (getReplyWithTimeout(link.genie, r, 20)) order.push_back(r.object);

    CHECK(order.size() == 4, "4 event de kayipsiz ulasti");
    bool orderOk = order.size() == 4 && order[0]==20 && order[1]==21 && order[2]==20 && order[3]==22;
    CHECK(orderOk, "Event sirasi korunmus (1 -> 2 -> 1 -> 3)");
}

static void test4_burst() {
    std::printf("Test 4: Burst Test (1000+ events)\n");
    FakeLink link;
    const int N = 2000;
    std::atomic<bool> writerDone{false};
    std::thread writer([&]() {
        for (int i = 0; i < N; ++i)
            writeFrame(link.displayFd, reportEventFrame(30, 1, (uint16_t)i));
        writerDone.store(true);
    });

    auto start = steady_clock::now();
    int received = 0;
    while (steady_clock::now() - start < seconds(5)) {
        genieReplyStruct r;
        if (getReplyWithTimeout(link.genie, r, 20)) ++received;
        else if (writerDone.load() && !link.genie.genieReplyAvail()) break;
    }
    writer.join();
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    uint32_t dropped = link.genie.getDroppedEventCount();

    CHECK(elapsed < 5000, "Burst 5 saniye icinde tamamlandi (hang/deadlock yok)");
    CHECK(received + (int)dropped > 0, "Application crash olmadan calisti");
    std::printf("  bilgi: received=%d dropped=%u\n", received, dropped);
}

static void test5_missingAck() {
    std::printf("Test 5: Missing ACK\n");
    FakeLink link;
    link.genie.setAckTimeout(200);

    auto start = steady_clock::now();
    int result = link.genie.genieWriteObj(0, 0, 123);
    auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();

    CHECK(result == GENIE_ERROR_TIMEOUT, "ACK gelmeyince GENIE_ERROR_TIMEOUT dondu");
    CHECK(elapsed >= 190 && elapsed < 1000, "Fonksiyon SURESIZ degil, ~200ms icinde dondu");
}

static void test6_concurrentReadEvent() {
    std::printf("Test 6: Concurrent Read/Event\n");
    FakeLink link;
    writeFrame(link.displayFd, reportEventFrame(41, 1, 7));
    std::this_thread::sleep_for(milliseconds(20));

    std::thread responder([&]() {
        uint8_t buf[3];
        ssize_t n = read(link.displayFd, buf, sizeof(buf)); (void)n;
        std::this_thread::sleep_for(milliseconds(5));
        writeFrame(link.displayFd, reportObjFrame(40, 5, 999));
    });

    int value = link.genie.genieReadObj(40, 5);
    responder.join();
    CHECK(value == 999, "genieReadObj beklenen GENIE_REPORT_OBJ cevabini dogru okudu");

    genieReplyStruct r{};
    bool got = getReplyWithTimeout(link.genie, r, 200);
    bool eventPreserved = got && r.cmd == GENIE_REPORT_EVENT && r.object == 41 && r.index == 1;
    CHECK(eventPreserved, "genieReadObj sirasinda gelen event kaybolmadi");
}

int main() {
    std::printf("=== GeniePi - Rapid Consecutive Button Event Handling Stress Suite ===\n\n");
    test1_sameButtonDoublePress();
    test2_sameButtonSlowPress();
    test3_differentButtons();
    test4_burst();
    test5_missingAck();
    test6_concurrentReadEvent();
    std::printf("\n=== Sonuc: %s (%d basarisiz kontrol) ===\n",
                g_failures == 0 ? "TUM TESTLER GECTI" : "BAZI TESTLER BASARISIZ", g_failures);
    return g_failures == 0 ? 0 : 1;
}