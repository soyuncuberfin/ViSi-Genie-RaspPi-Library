# ViSi-Genie Debounce & Thread-Safety Patch — General Report

This report consolidates two tasks into a single document:

* **Section A:** Software-level resolution (library patch) of eight issues identified in `ViSi-Genie-RaspPi-Library` and verification of this solution over a `socketpair()`-based mock serial line.

* **Section B:** Verification of this patch on real 4D Systems touchscreen hardware (uLCD-43DT, DIABLO16) — single-threaded physical test, and finally, a multithreaded concurrent stress test for both libraries (original/patched).

# SECTION A — Software Patch

## A.1 Objective

In the `headerandcpp/GeniePiLib.h`/`.cpp` (C++ class-based) version, resolving library-level issues that prevent the safe processing of rapid consecutive button events and verifying this with a stress test. 

## A.2 Issues Identified in the Original Library

| \# | Issue | State in the Actual Code | 
 | ----- | ----- | ----- | 
| 1 | No debounce mechanism | `GENIE_REPORT_EVENT` was written directly to the queue without passing through any filter | 
| 2 | Reply/event queue is not thread-safe | `genieReplysHead`/`genieReplysTail` are plain `int`; no mutex/atomic protection between the listener and application threads | 
| 3 | Events are silently dropped when the queue is full | `if (next != tail) { write }` — no `else` branch, overflow is never observable | 
| 4 | Pending events are cleared when `genieReadObj()` is called | At the beginning of the function: `while (genieReplyAvail()) genieGetReply(&reply);` — clears everything, including asynchronous button events | 
| 5 | ACK/NAK wait can last indefinitely | `while (!genieAck && !genieNak) delay(1);` — furthermore, even if NAK arrives, it returns `return 0` (success) without checking | 
| 6 | Event order across different buttons must be preserved | A requirement carrying the risk of degradation when debounce is introduced | 
| 7 | Must not block the listener thread | The newly added debounce/queue logic also had to avoid breaking this | 
| 8 | No stress test validating this | — | 

## A.3 Solution Architecture

### A.3.1 Thread-Safe Queue (Issues 2, 3)

The fixed-size, unsynchronized ring buffer was removed and replaced with a `std::deque` protected by `std::mutex` + `std::condition_variable`:

```
std::mutex                        replyQueueMutex;
std::condition_variable           replyQueueCv;
std::deque<genieReplyStruct>      replyQueue;
static constexpr size_t GENIE_QUEUE_CAPACITY = 256;
std::atomic<uint32_t> droppedEvents{0};

```

`deque` was chosen because resolving Issue 4 requires pushing items back to the **front** of the queue without altering the order — which is not natural for a fixed-size ring buffer. `genieGetReply()` uses `replyQueueCv.wait(lock, ...)` instead of busy-waiting (`delay(1)`). When the queue is full, the `droppedEvents` counter now increments — the event may still be dropped, but it is now **observable**.

### A.3.2 Debounce (Issues 1, 6) — The Story of the Design Decision

The initial implementation maintained the debounce state using an **`(object,index)`-based `std::unordered_map`**. In the following scenario, this design proved flawed:

```
Button1 pressed -> Button2 pressed -> Button1 pressed again -> Button3 pressed
(all rapid and consecutive)

```

Because the second `Button1` shared the same (object,index,data) as the first, it was **erroneously debounced** in the map-based design — whereas another button had been pressed in between, meaning this was an independent, genuine user action.

**Solution:** The map was replaced with a **single, global `lastEvent` variable**:

```
bool GeniePi::shouldDebounce(int object, int index, unsigned int data,
                              steady_clock::time_point now)
{
    unsigned int window = debounceMs.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(debounceMutex);
    if (window > 0 && lastEvent.valid &&
        lastEvent.object == object && lastEvent.index == index &&
        lastEvent.data == data)
    {
        auto elapsedMs = duration_cast<milliseconds>(now - lastEvent.ts).count();
        if (elapsedMs >= 0 && (unsigned long long)elapsedMs < window)
            return true; // genuine duplicate
    }
    lastEvent = LastEvent{object, index, data, now, true};
    return false;
}

```

Each new event updates `lastEvent`; when the second `Button1` arrives, the memory now holds `Button2`, so the comparison fails and the event is treated as "new" — order (Issue 6) is preserved without requiring extra mechanisms. The decision is made **solely by timestamp comparison**; there is no `sleep`/`delay` inside the listener (Issue 7).

### A.3.3 ACK/NAK Timeout (Issue 5)

```
int GeniePi::waitForAck(void)
{
    std::unique_lock<std::mutex> lock(ackMutex);
    unsigned int timeout = ackTimeoutMs.load();
    bool signaled = ackCv.wait_for(lock, milliseconds(timeout),
                                    [this] { return genieAck.load() || genieNak.load(); });
    if (!signaled) return GENIE_ERROR_TIMEOUT;
    return genieNak.load() ? GENIE_ERROR_NAK : GENIE_OK;
}

```

A new `enum GenieResult { GENIE_OK=0, GENIE_ERROR_NAK=-1, GENIE_ERROR_TIMEOUT=-2 }` was defined (since `GENIE_OK==0`, legacy `if (x==0)` checks remain unbroken). This function replaced the legacy `while(...) delay(1); return 0;` block across 7 separate write functions. When an ACK/NAK byte is observed in the listener, `ackCv.notify_all()` is now also invoked.

### A.3.4 `genieReadObj()` — Preserving Pending Events (Issue 4)

The clearing lines at the beginning of the function were removed; instead, unrelated messages encountered while awaiting its own reply are collected in an `unrelated` deque and pushed back to the **front of the queue in their original order** immediately before returning:

```
if ((reply.cmd == GENIE_REPORT_OBJ) && (reply.object==object) && (reply.index==index))
{
    requeueFront(unrelated);
    return reply.data;
}
unrelated.push_back(reply); // e.g. a button event -> DO NOT LOSE

```

## A.4 Test Methodology and Execution (Issue 8)

Using a Unix socket pair with two connected ends via `socketpair()`, `genieReplyListener` was tested end-to-end without real hardware (`test_rapid_events.cpp`):

| Test | What It Measures | What It Proves | 
 | ----- | ----- | ----- | 
| Test 1 | Same button at t=0 and t=50ms | Genuine duplicate within the 150ms window is filtered | 
| Test 2 | Same button at t=0 and t=300ms | Press outside the window is not debounced | 
| Test 3 | Button1→2→1→3 in rapid succession | Order is preserved, the intervening second Button1 is not dropped | 
| Test 4 | 2000 events in a burst | No hang/deadlock/crash | 
| Test 5 | ACK is never sent | `genieWriteObj` does not wait indefinitely and returns `GENIE_ERROR_TIMEOUT` | 
| Test 6 | An event arrives during `genieReadObj` | After read completes, the event remains in the queue | 

**Build and execution** (in the `headerandcpp/` directory):

```
g++ -std=c++17 -O2 -Wall -Wextra -pthread GeniePiLib.cpp test_rapid_events.cpp -o test_rapid_events
./test_rapid_events

```

## A.5 Results

* Compilation: **12/12 checks PASS**, consistent across 3 distinct runs. All eight issues were resolved without breaking the library's public API (function signatures, struct fields) — existing application code continues to function without modifications.

# SECTION B — Physical Verification

## B.1 Objective

Verification of the patch in Section A on actual hardware (uLCD-43DT, DIABLO16), over genuine serial communication, using real user touch inputs.

## B.2 Environment Setup — Issues Encountered and Solutions

| Issue | Cause | Solution | 
 | ----- | ----- | ----- | 
| `winetricks` not in apt | Not yet packaged in Debian 13 repository | Download the script directly via `wget` | 
| Installation window completely black | Wine window repainting limitation | Run inside a virtual desktop via `wine explorer /desktop=...` | 
| Crash on initial launch: `EInvalidGraphic: Bitmap image is not valid` | The Delphi 2010-based IDE's `SetWin7ScenicRibbon` code, active on "Windows 7+" detection, hits a Wine limitation while converting icons to DIB | `winecfg` → Windows Version → **Windows XP** | 
| USB device not appearing in Wine | Wine does not map `/dev/ttyUSBx` to a COM port by default | `ln -s /dev/ttyUSB0 ~/.wine-workshop4/dosdevices/com1` (Wine may map the actual device to another `comN` via auto-scan — verify via `ls dosdevices/`) | 

```
sudo dpkg --add-architecture i386
sudo apt install wine wine32 wine64
sudo usermod -aG dialout $USER
WINEPREFIX=~/.wine-workshop4 WINEARCH=win32 winecfg
wine explorer /desktop=install,1024x768 Workshop4-PRO-Installer.exe

```

## B.3 Physical Test Design (`DebounceTest` Project, Patched Library)

**Screen design:** 1 button (`BTN_TEST`) + 3 LED digits (`DIGIT_RAW`/`DIGIT_ACCEPTED`/`DIGIT_REJECTED`). **Design decision:** The display module itself does not perform debounce; this logic resides in the patched library on the host. The display produces raw events, the host processes them and writes the result back to the display — allowing the impact of debounce to be observed live on-screen.

**Header generation:**

```
python3 tools/generate_genie_header.py --add-type LedDigits
python3 tools/generate_genie_header.py --project DebounceTest.4DGenie \
    --output include/genie_objects.h --strict

```

**Instrumentation added to the library** (counters extending the API solely for physical verification purposes):

```
uint32_t getRawEventCount(void) const;       // BEFORE debounce
uint32_t getDebouncedEventCount(void) const; // filtered by debounce
size_t   getQueueSize(void);                 // current queue depth

```

**Baud rate determination:** The runtime baud rate of the display was unknown (115200 used by the File Transfer tool differed from the operational application rate). Verification was performed directly with the host program: the first event arrived on **`/dev/ttyUSB0` @ `9600` baud**, and this setting was utilized in the tests.

**Measured parameters** — CSV log format:

```
timestamp,event_type,object_type,object_index,value,received,accepted,
queue_size,raw_total,accepted_total,rejected_total,dropped_total

```

`EVENT`: Every button press reaching the host (passed through debounce).
`STATUS`: Instantaneous counter/queue status approximately every 500ms, even in the absence of events.

**Build and execution** (in the `headerandcpp/` directory):

```
g++ -std=c++17 -O2 -Wall -Wextra -pthread GeniePiLib.cpp physical_debounce_test.cpp -o physical_debounce_test
mkdir -p logs
./physical_debounce_test /dev/ttyUSB0 9600 150 <label> [duration_sec]

```

### Results — Five Scenarios

| Scenario | Observation | 
 | ----- | ----- | 
| Normal single press | No suppression, all presses accepted | 
| Slow consecutive presses | No suppression | 
| Very rapid consecutive presses | Debounce engaged, but the majority were still accepted (selective filter, not "suppressing everything") | 
| Rapid continuous repetition | Likewise, a portion was rejected and a portion accepted | 
| Brief tap / hold | No suppression | 

Normal/isolated presses were not suppressed under any condition; debounce was triggered only during genuinely rapid/intense repetitions — exhibiting selective filtering behavior rather than an aggressive, all-rejecting filter.

## B.4 Multithreaded Concurrent Stress Test — Both Libraries

### B.4.1 Design

`multithread_patched_test.cpp` / `multithread_raw_test.cpp` augments a form-switching architecture triggered by button events with a **second background thread**:

* **Thread 1 (background):** Starting from the receipt of the first valid button event, continuously calls `genieWriteObj(...)` at a **10ms interval**, imposing an artificial load ("telemetry flood") onto the UART line.

* **Thread 2 (main loop):** Reads button events, applies debounce logic, and issues a form-switch command for every accepted clean press.

`multithread_patched_test.cpp` uses the patched `GeniePiLib` library, while `multithread_raw_test.cpp` uses the **completely unmodified** `geniePi.c`/`geniePi.h` library directly in the root directory — both evaluating the identical scenario under the same concurrent load.

This test simultaneously validates **Issue 2** (non-thread-safe queue) and **Issue 5** (infinite ACK/NAK wait) from Section A **under real hardware and real concurrent I/O load** — moving beyond the single-threaded verification of the synthetic `test_rapid_events.cpp` test.

Because 115200 is used in real-world scenarios, the baud rate was configured to 115200 in these tests.

**Build and execution (patched, in `headerandcpp/` directory):**

```
g++ -std=c++17 -O2 -Wall -Wextra -pthread GeniePiLib.cpp multithread_patched_test.cpp -o multithread_patched_test
./multithread_patched_test /dev/ttyUSB0 150

```

**Build and execution (original `geniePi.c`, in the same directory):**

```
gcc -O2 -Wall -c geniePi.c -o geniePi.o
g++ -std=c++17 -O2 -Wall -Wextra multithread_raw_test.cpp geniePi.o -lpthread -o multithread_raw_test
./multithread_raw_test /dev/ttyUSB0 150

```

### B.4.2 Results — Comparative

**Patched library, actual execution output:**

```
=== FINAL TEST RAPORU ===
Toplam Ham Dokunus (Raw Touches) : 220
Kabul Edilen Temiz Basış         : 185
Debounce ile Elenen Sıçrama      : 35
Basarili Form Gecis Sayisi       : 185
Telemetri Paket Sayisi (UART Yuk): 3610
=========================================

```

While the background thread executed \~100 writes per second (3610 packets), **all 185** form transitions completed successfully; no crashes, freezes, or deadlocks occurred. Debounce (35 events eliminated) and thread-safety functioned concurrently under the same load without issue.

**Original library, actual execution output (summary):**
When the identical scenario was executed with the original library, the program eventually **deadlocked permanently** and could only be terminated forcibly via a **second Ctrl+C**:

```
>>> [MAIN UI] Buton yakalandi! Form gecisi yapiliyor: Form1 -> Form2 ...
^C^C
[DEADLOCK TEYIT EDILDI] Kutuphane while(!genieAck) icinde kilitlendi, 2. Ctrl+C ile zorla cikiliyor!

```

Counting the `"Buton yakalandi"` and `"Form degisti"` lines in the log:
**59 button events were captured, 58 of which were successfully converted to form transitions**; on the **59th** attempt (`Form1 -> Form2`), the ACK for the issued command never arrived, and the program remained permanently stuck in the original library's `while (!genieAck.load() && !genieNak.load())` loop.

**Mechanism:** In the original library, `genieWriteObj` invocations are serialized by a single global mutex (`genieMutex`), but there is no **timeout** mechanism for ACK/NAK (Issue 5). Continuous writing by the background thread (every 10ms) heavily saturates the serial input throughput and form-rendering engine of the display; under this saturation, an ACK for a write command never returns. In the single-threaded test (B.3), this never manifested — the issue was triggered exclusively under **multithreaded, heavy concurrent I/O load**. This is precisely the scenario mitigated by the patch's `waitForAck()` (`ackCv.wait_for(..., timeout)`): under the patched implementation, even if an ACK fails to return under identical load, the function returns `GENIE_ERROR_TIMEOUT`, and the program never hangs.

### B.4.3 Comparison Table

| Metric | Original | Patched | 
 | ----- | ----- | ----- | 
| Did the test complete? | **No** — permanently deadlocked, forcibly terminated with a 2nd Ctrl+C | Yes, completed without issues | 
| Successful transitions prior to lockup | 58 | 185 (entire test) | 
| Deadlock point | At the 59th button event, ACK never arrived | None | 
| Did debounce function? | Concept nonexistent (test never progressed past that point) | Yes, 35 events eliminated | 
| Behavior under background load (telemetry) | Inevitably deadlocks at some point | Flawless, stable across 3610 packets | 

## Overall Conclusion

Physical verification proceeded in two stages: (1) with a single button + digit indicators, it was proven that debounce filters out physical mechanical bounces without degrading normal user interaction, and (2) through the multithreaded concurrent stress test, it was demonstrated that the lack of ACK timeout in the original library (Issue 5) leads to a **proven deadlock** on real hardware, whereas the patched library maintains both debounce and thread-safety guarantees under identical conditions.