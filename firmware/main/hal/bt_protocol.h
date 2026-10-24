#pragma once
// Portable client of the UART protocol of the Aurora Player "Pico Bluetooth bridge" (an M5Stamp Pico with
// firmware 0.9.x wired to the Tab5; aurora-player/pico-bridge/README.md, "USB console and UART protocol").
// Our own implementation of the documented text protocol, no ESP-IDF headers: used by hal/bt.cpp (Tab5),
// host/hal_host.cpp (PC, fake Pico) and host/tests/test_bt_protocol.cpp.
//  - LineAssembler: received bytes -> lines. CR, LF or CRLF end a line, empty lines are skipped; a line with a
//    control character or longer than 120 bytes is rejected. Exceptions: "STATUS ..." may be up to 720 bytes
//    (Pico 0.9.5 prints ~450-650 there) and "DEVICE ..." up to 300 (a scan result carries the whole discovered
//    name, up to 248 bytes, after ~37 bytes of fields). Bytes >= 0x80 pass (UTF-8 device names) and are
//    validated by parse().
//  - parse(): one line -> Msg. Every line of the README table plus the STATUS / AIRPODS / CODEC formats of
//    pico-bridge/src. Text fields are copied as valid UTF-8 (bad bytes -> '?').
//  - Link: the time-driven state machine. Inputs: now (ms), received bytes, enable / route flags and the codec
//    rate; outputs: command lines (takeTx), the wake pulse (wakeLow), whether the UART / bridge I2S are needed,
//    the hal::BtStatus, the device list and the headphone-button queue.
//    enable -> G6 low 120 ms -> boot wait (READY or any reply; 650 ms after the pulse USE + STATUS for a Pico
//    that was already awake; no reply in 3 s: pulse again; after 3 tries a Polish notice and one try every
//    15 s) -> awake: USE every 3 s, STATUS every 15 s (and as a probe after 5 s of silence), LIST after READY /
//    on connect / after SCAN_DONE (every 4 s while scanning), one API command per step; route + connected ->
//    STREAM <token> (new token per start, resent once without STREAM_ACK in 1 s, then a new try after 5 s),
//    INPUT_STALLED -> new STREAM, TRANSPORT_FAILED -> new STREAM after 5 s, route off / disconnect -> STOP;
//    12 s without a reply -> wake again; disable -> STOP, SLEEP, wait for STANDBY (<= 2.5 s), off.
//    After SLEEP nothing else is sent: any UART start bit would wake the sleeping Pico (EXT0 on G32).
//    Not thread-safe: the owner serialises feed / step / the API calls. Storage must start zeroed or reset().
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hal/hal.h"

namespace hal::btp {

constexpr int kLineMax = 120;          // longest line accepted from the Pico ...
constexpr int kStatusLineMax = 720;    // ... except "STATUS ..."
constexpr int kDeviceLineMax = 300;    // ... and "DEVICE ..." (found devices: name[249] on the Pico)
constexpr int kCmdMax = 80;            // longest command line (the Pico rejects longer ones)
constexpr int kMaxDevices = 32;        // LIST: 8 saved + 24 found
constexpr int kQueueLen = 8;           // API commands waiting for the link
constexpr int kTxMax = 512;            // pending output bytes
constexpr int kRemoteLen = 8;          // headphone buttons not yet popped

constexpr uint32_t kWakePulseMs = 120;     // G6 low: wakes the Pico from deep sleep
constexpr uint32_t kBootGuardMs = 650;     // after the pulse, before the first command
constexpr uint32_t kWakeRetryMs = 3000;    // pulse start -> next pulse without any reply
constexpr int kWakeTries = 3;
constexpr uint32_t kAbsentRetryMs = 15000;
constexpr uint32_t kUseMs = 3000;          // keepalive (the Pico sleeps 15 s after the last USE)
constexpr uint32_t kStatusMs = 15000;
constexpr uint32_t kProbeMs = 5000;        // STATUS as a probe when the Pico was silent this long
constexpr uint32_t kSilenceMs = 12000;     // awake without a reply this long: wake again
constexpr uint32_t kAckMs = 1000;          // STREAM -> STREAM_ACK
constexpr uint32_t kStreamRetryMs = 5000;  // TRANSPORT_FAILED / never acknowledged -> next STREAM
constexpr uint32_t kStandbyMs = 2500;      // SLEEP -> STANDBY
constexpr uint32_t kListMs = 3000;         // LIST -> END_LIST
constexpr uint32_t kScanListMs = 4000;     // LIST cadence while scanning
constexpr uint32_t kRestartMs = 8000;      // RESTART / SELECTED -> READY (API commands held meanwhile)
// 0.4.4: the Pico's buffer. Its ring (pico-bridge 0.9.x: 4096 frames, start at 768) has no clock-drift correction
// and every underrun re-primes it, so the audio waiting there (latency) creeps up during a session. Bluedroid pulls
// ~1323 frames every 30 ms, so a STATUS sample lands anywhere between the floor (just after a pull) and the floor
// + 1323; the lowest of the last samples is the floor. A floor above kResyncFloor in a silent moment -> a new
// STREAM token: the Pico empties the ring (no A2DP restart) and starts again from its pre-roll.
constexpr uint32_t kStatusStreamMs = 2000;  // STATUS cadence while the stream is acknowledged
constexpr int kFloorSamples = 6;            // samples for the floor (12 s)
constexpr int32_t kResyncFloor = 1100;      // frames (~25 ms): more is drift / re-primed underruns
constexpr uint32_t kResyncQuietMs = 600;    // the audio to the bridge was silent this long
constexpr uint32_t kResyncGapMs = 20000;    // a re-sync at the earliest 20 s after the stream (re)started

inline bool due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
inline bool pre(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

// ---------------------------------------------------------------- line assembler
struct LineAssembler {
    char buf[kStatusLineMax + 1];
    int used;
    bool bad;
    uint32_t rejected;   // lines dropped (too long / control characters)

    void reset()
    {
        used = 0;
        bad = false;
    }
    // One byte. 1: text() holds a complete line (until the next put), -1: a line was rejected, 0: more.
    int put(uint8_t c)
    {
        if (c == '\n' || c == '\r') {
            const int n = used;
            const bool b = bad;
            reset();
            buf[b ? 0 : n] = 0;
            if (b) {
                ++rejected;
                return -1;
            }
            return n ? 1 : 0;
        }
        if (bad) return 0;
        const int limit = used < 7                      ? kLineMax
                          : !memcmp(buf, "STATUS ", 7) ? kStatusLineMax
                          : !memcmp(buf, "DEVICE ", 7) ? kDeviceLineMax
                                                       : kLineMax;
        if (c < 0x20 || c == 0x7F || used >= limit) bad = true;
        else buf[used++] = (char)c;
        return 0;
    }
    const char* text() const { return buf; }
};

// ---------------------------------------------------------------- parser
enum class Kind : uint8_t {
    Unknown,     // not a protocol line (ignored, does not count as a reply)
    Malformed,   // a known prefix with invalid fields (counts as a reply, otherwise ignored)
    Ready, State, Status, Airpods, AirpodsNote, AirpodsError, Codec, Device, EndList, Selected, Restart,
    ScanDone, Volume, StreamAck, StreamFault, Recovery, Silent, TestStart, Standby, Remote, Error
};
enum class Fault : uint8_t { Other, InputStalled, TransportFailed };

struct Msg {
    Kind kind;
    Fault fault;
    BtRemote remote;
    bool flag;                     // Selected: "...; restarting to connect"; Restart: "scanning"
    int32_t connection, audio;     // State / Status, -1 absent
    int32_t volume, scan, i2s_error, cap_airpods;   // Status / Ready / Volume, -1 absent
    int32_t ap_connected, ap_mode, ap_left, ap_right, ap_case;   // Airpods, -1 unknown
    int32_t rate;                  // Codec, -1 unknown
    int64_t buffered, underruns, dropped;   // Status: the Pico's ring now (frames) and its counters, -1 absent
    uint32_t token;                // StreamAck / StreamFault / Recovery
    char version[16];              // Ready / Status, "" absent
    char text[96];                 // Error / AirpodsError / Selected name
    BtDevice dev;                  // Device
};

// Decimal at p ('-' only when lo < 0), at most 10 digits, followed by ' ' or the end. nullptr: invalid.
inline const char* num(const char* p, int64_t lo, int64_t hi, int64_t& v)
{
    const bool neg = *p == '-' && lo < 0;
    if (neg) ++p;
    int64_t x = 0;
    int d = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        if (++d > 10) return nullptr;
        x = x * 10 + (*p - '0');
    }
    if (!d || (*p && *p != ' ')) return nullptr;
    if (neg) x = -x;
    if (x < lo || x > hi) return nullptr;
    v = x;
    return p;
}

// The value of "key=" (at the start or after a space), nullptr when absent.
inline const char* field(const char* s, const char* key)
{
    const size_t k = strlen(key);
    for (const char* p = s; p;) {
        if (!strncmp(p, key, k) && p[k] == '=') return p + k + 1;
        p = strchr(p, ' ');
        if (p) ++p;
    }
    return nullptr;
}

inline int32_t ifield(const char* s, const char* key, int64_t lo, int64_t hi)
{
    const char* v = field(s, key);
    int64_t x = -1;
    return v && num(v, lo, hi, x) ? (int32_t)x : -1;
}

// An unsigned 32-bit counter field, -1 absent or invalid.
inline int64_t ufield(const char* s, const char* key)
{
    const char* v = field(s, key);
    int64_t x = -1;
    return v && num(v, 0, 0xFFFFFFFFll, x) ? x : -1;
}

// src[0..len) -> dst[cap] as valid UTF-8: invalid bytes and control characters become '?', a sequence that
// does not fit is left out whole, trailing spaces are trimmed.
inline void copyText(char* dst, size_t cap, const char* src, size_t len)
{
    size_t o = 0;
    const uint8_t* s = (const uint8_t*)src;
    const uint8_t* e = s + len;
    while (s < e && *s) {
        const uint8_t c = *s;
        int n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        bool ok = n > 0 && c >= 0x20 && c != 0x7F && (size_t)(e - s) >= (size_t)n;
        for (int i = 1; ok && i < n; ++i) ok = (s[i] & 0xC0) == 0x80;
        if (ok && n >= 3)   // no overlong forms, surrogates or code points above U+10FFFF
            ok = !((c == 0xE0 && s[1] < 0xA0) || (c == 0xED && s[1] > 0x9F) || (c == 0xF0 && s[1] < 0x90) ||
                   (c == 0xF4 && s[1] > 0x8F));
        if (!ok) n = 1;
        if (o + (size_t)n >= cap) break;
        if (ok) memcpy(dst + o, s, (size_t)n);
        else dst[o] = '?';
        o += (size_t)n;
        s += n;
    }
    while (o && dst[o - 1] == ' ') --o;
    if (cap) dst[o] = 0;
}

inline void copyVersion(char* dst, const char* v)
{
    int i = 0;
    for (; v && i < 15; ++i) {
        const char c = v[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-' ||
              c == '_' || c == '+'))
            break;
        dst[i] = c;
    }
    dst[i] = 0;
}

inline bool isHex(char c) { return (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f'); }

// "AA:BB:CC:DD:EE:FF" (either case) followed by `end`
inline bool macAt(const char* m, char end)
{
    for (int i = 0; i < 17; ++i)
        if (i % 3 == 2 ? m[i] != ':' : !isHex(m[i])) return false;
    return m[17] == end;
}

// "DEVICE <n> <MAC> <rssi> <saved> <selected> <name>"
inline bool parseDevice(const char* s, BtDevice& d)
{
    int64_t n = 0, rssi = 0, sv = 0, se = 0;
    const char* p = num(s + 7, 1, kMaxDevices, n);
    if (!p || *p != ' ' || !macAt(p + 1, ' ')) return false;
    const char* mac = p + 1;
    p = num(mac + 18, -127, 20, rssi);
    if (p && *p == ' ') p = num(p + 1, 0, 1, sv);
    if (p && *p == ' ') p = num(p + 1, 0, 1, se);
    else p = nullptr;
    if (!p) return false;
    memset(&d, 0, sizeof d);
    for (int i = 0; i < 17; ++i) d.mac[i] = mac[i] >= 'a' && mac[i] <= 'f' ? (char)(mac[i] - 32) : mac[i];
    d.rssi = rssi == -127 ? 0 : (int16_t)rssi;   // the Pico prints -127 for saved devices
    d.saved = (uint8_t)sv;
    d.selected = (uint8_t)se;
    if (*p == ' ') copyText(d.name, sizeof d.name, p + 1, strlen(p + 1));
    if (!d.name[0]) memcpy(d.name, d.mac, sizeof d.mac);
    return true;
}

inline void parse(const char* s, Msg& m)
{
    memset(&m, 0, sizeof m);
    m.connection = m.audio = m.volume = m.scan = m.i2s_error = m.cap_airpods = -1;
    m.ap_connected = m.ap_mode = m.ap_left = m.ap_right = m.ap_case = m.rate = -1;
    m.buffered = m.underruns = m.dropped = -1;
    if (!s || !*s) return;
    Kind k = Kind::Unknown;
    int64_t v = 0;
    const char* e = nullptr;
    if (pre(s, "READY") && (s[5] == ' ' || !s[5])) {
        k = Kind::Ready;
        copyVersion(m.version, field(s, "version"));
        m.cap_airpods = ifield(s, "cap_airpods", 0, 1);
    } else if (pre(s, "STATE ") || pre(s, "STATUS ")) {
        k = s[4] == 'E' ? Kind::State : Kind::Status;
        m.connection = ifield(s, "connection", 0, 5);
        m.audio = ifield(s, "audio", 0, 5);
        if (m.connection < 0) k = Kind::Malformed;
        if (k == Kind::Status) {
            m.volume = ifield(s, "volume", 0, 100);
            m.scan = ifield(s, "scan", 0, 1);
            m.i2s_error = ifield(s, "i2s_error", 0, 0x7FFFFFFF);
            m.cap_airpods = ifield(s, "cap_airpods", 0, 1);
            copyVersion(m.version, field(s, "version"));
            m.buffered = ufield(s, "buffered");
            m.underruns = ufield(s, "underruns");
            m.dropped = ufield(s, "dropped");
        }
    } else if (pre(s, "AIRPODS ")) {
        k = Kind::Airpods;
        m.ap_connected = ifield(s, "connected", 0, 9);
        m.ap_mode = ifield(s, "mode", -1, 4);
        m.ap_left = ifield(s, "left", -1, 100);
        m.ap_right = ifield(s, "right", -1, 100);
        m.ap_case = ifield(s, "case", -1, 100);
        if (m.ap_connected < 0) k = Kind::Malformed;
    } else if (pre(s, "AIRPODS_PENDING")) {
        k = Kind::AirpodsNote;
    } else if (pre(s, "AIRPODS_ERROR")) {
        k = Kind::AirpodsError;
        copyText(m.text, sizeof m.text, s[13] ? s + 14 : s + 13, strlen(s + 13));
    } else if (pre(s, "CODEC ")) {
        m.rate = ifield(s, "rate", -1, 192000);
        k = field(s, "rate") && (m.rate >= 0 || !strncmp(field(s, "rate"), "-1", 2)) ? Kind::Codec : Kind::Malformed;
    } else if (pre(s, "DEVICE ")) {
        k = parseDevice(s, m.dev) ? Kind::Device : Kind::Malformed;
    } else if (!strcmp(s, "END_LIST")) {
        k = Kind::EndList;
    } else if (pre(s, "SELECTED ")) {
        k = Kind::Selected;
        const char* r = strstr(s, "; restarting");
        m.flag = r != nullptr;
        copyText(m.text, sizeof m.text, s + 9, r ? (size_t)(r - (s + 9)) : strlen(s + 9));
    } else if (pre(s, "RESTART")) {
        k = Kind::Restart;
        m.flag = strstr(s, "scanning") != nullptr;
    } else if (!strcmp(s, "SCAN_DONE")) {
        k = Kind::ScanDone;
    } else if (pre(s, "VOLUME ")) {
        e = num(s + 7, 0, 100, v);
        k = e && !*e ? Kind::Volume : Kind::Malformed;
        m.volume = (int32_t)v;
    } else if (pre(s, "STREAM_ACK ")) {
        e = num(s + 11, 0, 0xFFFFFFFFll, v);
        k = e && !*e ? Kind::StreamAck : Kind::Malformed;
        m.token = (uint32_t)v;
    } else if (pre(s, "STREAM_FAULT ") || pre(s, "RECOVERY ")) {
        const bool fault = s[1] == 'T';
        e = num(s + (fault ? 13 : 9), 0, 0xFFFFFFFFll, v);
        k = e && (*e == ' ' || !fault) ? (fault ? Kind::StreamFault : Kind::Recovery) : Kind::Malformed;
        m.token = (uint32_t)v;
        if (e && *e == ' ') {
            m.fault = !strcmp(e + 1, "INPUT_STALLED")      ? Fault::InputStalled
                      : !strcmp(e + 1, "TRANSPORT_FAILED") ? Fault::TransportFailed
                                                           : Fault::Other;
        }
    } else if (!strcmp(s, "SILENT")) {
        k = Kind::Silent;
    } else if (pre(s, "TEST START")) {
        k = Kind::TestStart;
    } else if (!strcmp(s, "STANDBY")) {
        k = Kind::Standby;
    } else if (pre(s, "REMOTE ")) {
        const char* a = s + 7;
        m.remote = !strcmp(a, "PLAY")       ? BtRemote::Play
                   : !strcmp(a, "PAUSE")    ? BtRemote::Pause
                   : !strcmp(a, "NEXT")     ? BtRemote::Next
                   : !strcmp(a, "PREVIOUS") ? BtRemote::Previous
                                            : BtRemote::None;
        k = m.remote == BtRemote::None ? Kind::Malformed : Kind::Remote;
    } else if (pre(s, "ERROR") && (s[5] == ' ' || !s[5])) {
        k = Kind::Error;
        copyText(m.text, sizeof m.text, s[5] ? s + 6 : s + 5, strlen(s + 5));
    }
    m.kind = k;
}

inline bool macValid(const char* mac) { return mac && strlen(mac) == 17 && macAt(mac, 0); }

// ---------------------------------------------------------------- link state machine
struct Inputs {
    bool enabled;          // btEnable
    bool route;            // btSetAudioRoute(to_bridge)
    uint32_t audio_rate;   // codec sample rate, 0 = stopped (the bridge I2S then runs on its own at 44.1 kHz)
    uint32_t quiet_ms;     // the audio sent to the bridge has been silent this long (buffer re-sync only then)
};

enum class Phase : uint8_t { Off, Wake, Boot, Online, Absent, Stopping };

class Link {
public:
    void reset()
    {
        memset(this, 0, sizeof *this);
        want_volume_ = -1;
        clearAirpods();
        s_.pico_buffered = s_.pico_floor = -1;
        memcpy(&shown_, &s_, sizeof s_);
    }

    // ---- the bridge task, every ~20 ms: feed (received bytes), step, then takeTx and the outputs below
    void feed(const uint8_t* data, int n, uint32_t now)
    {
        for (int i = 0; i < n; ++i)
            if (in_.put(data[i]) == 1) line(in_.text(), now);
    }

    void line(const char* text, uint32_t now)
    {
        Msg m;
        parse(text, m);
        if (m.kind == Kind::Unknown || ph_ == Phase::Off) return;
        last_rx_ = now;
        if (ph_ == Phase::Stopping) {
            if (m.kind == Kind::Standby) {
                ph_ = Phase::Off;
                note("Bluetooth off");
            }
        } else {
            s_.module = 1;
            if (ph_ != Phase::Online) online(now, m.kind == Kind::Ready);
            handle(m, now);
        }
        commit();
    }

    void step(uint32_t now, const Inputs& in)
    {
        en_ = in.enabled;
        quiet_ = in.quiet_ms;
        const bool rate_ok = in.audio_rate == 0 || in.audio_rate == 44100;
        switch (ph_) {
        case Phase::Off:
            if (!in.enabled) qn_ = 0;
            else {
                s_.module = 0;
                s_.faults = s_.recoveries = s_.resyncs = 0;
                s_.version[0] = 0;
                attempts_ = 0;
                note("Waking the Bluetooth module (Pico)…");
                wake(now);
            }
            break;
        case Phase::Wake:
            if (due(now, t_ + kWakePulseMs)) {
                ph_ = Phase::Boot;
                guard_ = now + kBootGuardMs;
                probe_ = false;
                in_.reset();
            }
            break;
        case Phase::Boot:
            if (!due(now, guard_)) break;
            if (!in.enabled) {
                stopping(now);
                break;
            }
            if (!probe_) {   // a Pico that was already awake does not print READY
                probe_ = true;
                send("USE");
                send("STATUS");
            }
            if (due(now, t_ + kWakeRetryMs)) {
                if (attempts_ < kWakeTries) {
                    wake(now);
                } else {
                    ph_ = Phase::Absent;
                    retry_at_ = now + kAbsentRetryMs;
                    note("No Bluetooth module (Pico) found — check the connection");
                }
            }
            break;
        case Phase::Absent:
            if (!in.enabled) {   // not answering (or asleep): nothing to put to sleep, no UART traffic
                ph_ = Phase::Off;
                qn_ = 0;
                note("Bluetooth off");
            } else if (due(now, retry_at_)) {
                wake(now);
            }
            break;
        case Phase::Online:
            if (!in.enabled) stopping(now);
            else awake(now, in.route && rate_ok, in.route && !rate_ok);
            break;
        case Phase::Stopping:
            if (due(now, sleep_at_ + kStandbyMs)) {
                ph_ = Phase::Off;
                note("Bluetooth off (the Pico did not confirm sleep)");
            }
            break;
        }
        commit();
    }

    // bytes to write to the UART (whole '\n'-terminated lines), oldest first
    int takeTx(char* out, int max)
    {
        const int n = ntx_ < max ? ntx_ : (max > 0 ? max : 0);
        memcpy(out, tx_, (size_t)n);
        memmove(tx_, tx_ + n, (size_t)(ntx_ - n));
        ntx_ -= n;
        return n;
    }

    // ---- outputs
    bool wakeLow() const { return ph_ == Phase::Wake; }        // G6 as a GPIO driven low (wake pulse)
    bool uartOn() const { return ph_ != Phase::Off; }          // the UART must be installed
    bool streamOn() const { return ph_ == Phase::Online && st_ != kIdle; }   // bridge I2S runs + gets the blocks
    // (a buffer re-sync waiting for its STREAM_ACK still counts: the local output stays muted meanwhile)
    bool streamAcked() const { return ph_ == Phase::Online && (st_ == kAcked || (st_ == kSent && resync_)); }
    Phase phase() const { return ph_; }
    uint32_t token() const { return token_; }
    int queued() const { return qn_; }

    // ---- API (the owner checks btEnable first)
    // One command line: 1..80 printable ASCII characters. False: invalid or the queue is full.
    bool queue(const char* cmd)
    {
        const size_t n = cmd ? strlen(cmd) : 0;
        if (!n || n > (size_t)kCmdMax || qn_ == kQueueLen) return false;
        for (size_t i = 0; i < n; ++i)
            if ((uint8_t)cmd[i] < 0x20 || (uint8_t)cmd[i] > 0x7E) return false;
        memcpy(q_[(qh_ + qn_) % kQueueLen], cmd, n + 1);
        ++qn_;
        return true;
    }
    // VOLUME n, replacing one still waiting; sent again after every READY
    bool setVolume(int v)
    {
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        want_volume_ = v;
        char c[16];
        snprintf(c, sizeof c, "VOLUME %d", v);
        for (int i = 0; i < qn_; ++i) {
            char* q = q_[(qh_ + i) % kQueueLen];
            if (pre(q, "VOLUME ")) {
                memcpy(q, c, strlen(c) + 1);
                return true;
            }
        }
        return queue(c);
    }
    const BtStatus& status() const { return s_; }
    int devices(BtDevice* out, int max) const
    {
        const int n = !out || max < 0 ? 0 : nlist_ < max ? nlist_ : max;
        if (n) memcpy(out, list_, sizeof(BtDevice) * (size_t)n);
        return n;
    }
    BtRemote popRemote()
    {
        if (!rn_) return BtRemote::None;
        const BtRemote r = rq_[rh_];
        rh_ = (rh_ + 1) % kRemoteLen;
        --rn_;
        commit();
        return r;
    }
    void setDropped(uint32_t n) { s_.dropped_blocks = n; }

private:
    static constexpr uint8_t kIdle = 0, kSent = 1, kAcked = 2, kRetry = 3;   // stream handshake

    void send(const char* s)
    {
        const int n = (int)strlen(s);
        if (ntx_ + n + 1 > kTxMax) return;
        memcpy(tx_ + ntx_, s, (size_t)n);
        ntx_ += n;
        tx_[ntx_++] = '\n';
    }
    void note(const char* a, const char* b = "")
    {
        char t[160];
        snprintf(t, sizeof t, "%s%s", a, b);
        copyText(s_.message, sizeof s_.message, t, strlen(t));
    }
    void clearAirpods()
    {
        s_.airpods = 0;
        s_.airpods_left = s_.airpods_right = s_.airpods_case = s_.airpods_mode = -1;
    }
    // the link to the headphones is gone or unknown (no commands sent)
    void dropLink()
    {
        s_.connected = 0;
        s_.scanning = 0;
        s_.sbc_rate = 0;
        s_.pico_buffered = s_.pico_floor = -1;
        nfl_ = 0;
        resync_ = false;
        pico_audio_ = false;
        st_ = kIdle;
        listing_ = false;
        restarting_ = false;
        clearAirpods();
    }
    void wake(uint32_t now)
    {
        ph_ = Phase::Wake;
        t_ = now;
        last_rx_ = now;
        ++attempts_;
        dropLink();
    }
    void online(uint32_t now, bool ready)
    {
        ph_ = Phase::Online;
        attempts_ = 0;
        next_use_ = now;
        next_status_ = now + (ready ? 300 : kStatusMs);
        last_status_ = now;
        list_want_ = true;
    }
    void stopping(uint32_t now)
    {
        if (st_ != kIdle) send("STOP");
        send("SLEEP");
        ph_ = Phase::Stopping;
        sleep_at_ = now;
        qn_ = 0;
        dropLink();
        note("Putting the Pico to sleep…");
    }
    void restarting(uint32_t now)
    {
        dropLink();
        restarting_ = true;
        restart_at_ = now;
    }
    void startStream(uint32_t now, bool resync = false)
    {
        if (!++token_) token_ = 1;
        char c[24];
        snprintf(c, sizeof c, "STREAM %lu", (unsigned long)token_);
        send(c);
        st_ = kSent;
        st_at_ = now;
        resent_ = false;
        resync_ = resync;
        resync_at_ = now;   // a re-sync at the earliest kResyncGapMs after any start
        nfl_ = 0;   // a new ring: new samples
        s_.pico_buffered = s_.pico_floor = -1;
    }

    // A STATUS sample of the running stream's ring: the newest and the floor (lowest of the last kFloorSamples).
    void ringSample(int64_t frames)
    {
        fl_[nfl_ % kFloorSamples] = frames > 0x7FFFFFFF ? 0x7FFFFFFF : (int32_t)frames;
        if (++nfl_ >= 2 * kFloorSamples) nfl_ = kFloorSamples;   // (keeps the slot order: kFloorSamples divides it)
        s_.pico_buffered = (int32_t)(frames > 0x7FFFFFFF ? 0x7FFFFFFF : frames);
        if (nfl_ < kFloorSamples) return;
        int32_t f = fl_[0];
        for (int i = 1; i < kFloorSamples; ++i) f = fl_[i] < f ? fl_[i] : f;
        s_.pico_floor = f;
    }

    void awake(uint32_t now, bool route, bool rate_bad)
    {
        if (due(now, last_rx_ + kSilenceMs)) {
            note("Pico not answering — waking it again");
            attempts_ = 0;
            wake(now);
            return;
        }
        if (restarting_ && due(now, restart_at_ + kRestartMs)) restarting_ = false;
        if (due(now, next_use_)) {
            send("USE");
            next_use_ = now + kUseMs;
        }
        if (due(now, next_status_) || (now - last_rx_ >= kProbeMs && now - last_status_ >= kProbeMs)) {
            send("STATUS");
            last_status_ = now;
            next_status_ = now + (st_ == kAcked ? kStatusStreamMs : kStatusMs);   // streaming: the Pico's buffer
        }
        if (listing_ && due(now, list_at_ + kListMs)) listing_ = false;
        if (!listing_ && !restarting_ && (list_want_ || (s_.scanning && due(now, list_at_ + kScanListMs)))) {
            send("LIST");
            listing_ = true;
            list_want_ = false;
            list_at_ = now;
            npend_ = 0;
        }
        if (restarting_) return;   // the Pico is rebooting: no stream, API commands wait for READY
        const bool want = route && s_.connected;
        if (!want && st_ != kIdle) {
            send("STOP");
            st_ = kIdle;
        } else if (want && (st_ == kIdle || (st_ == kRetry && due(now, st_at_ + kStreamRetryMs)))) {
            startStream(now);
        } else if (st_ == kSent && due(now, st_at_ + kAckMs)) {
            if (!resent_) {   // same token: the Pico answers a retransmission with the same ACK
                char c[24];
                snprintf(c, sizeof c, "STREAM %lu", (unsigned long)token_);
                send(c);
                resent_ = true;
                st_at_ = now;
            } else {
                note("Pico does not confirm the stream — retry in 5 s");
                st_ = kRetry;
                st_at_ = now;
                resync_ = false;
            }
        } else if (st_ == kAcked && s_.pico_floor > kResyncFloor && quiet_ >= kResyncQuietMs &&
                   due(now, resync_at_ + kResyncGapMs)) {
            // the Pico's buffer grew (clock drift, re-primed underruns): a new token empties it, in silence
            ++s_.resyncs;
            startStream(now, true);
        }
        if (rate_bad && s_.connected) {
            if (!rate_note_) note("Bluetooth needs 44.1 kHz");
            rate_note_ = true;
        } else {
            rate_note_ = false;
        }
        if (qn_) {
            send(q_[qh_]);
            qh_ = (qh_ + 1) % kQueueLen;
            --qn_;
        }
    }

    void handle(const Msg& m, uint32_t now)
    {
        switch (m.kind) {
        case Kind::Ready:
            if (m.version[0]) memcpy(s_.version, m.version, sizeof s_.version);
            cap_airpods_ = m.cap_airpods == 1;
            dropLink();
            s_.scanning = scan_restart_;
            scan_restart_ = false;
            list_want_ = true;
            next_use_ = now;
            next_status_ = now + 300;
            if (want_volume_ >= 0) setVolume(want_volume_);
            note("Bluetooth module (Pico) ready, version ", s_.version[0] ? s_.version : "?");
            break;
        case Kind::State:
        case Kind::Status: {
            const uint8_t c = m.connection == 2;
            if (c != s_.connected) {
                s_.connected = c;
                if (c) {
                    list_want_ = true;
                    note("Connected to a Bluetooth device");
                } else {
                    clearAirpods();
                    note("Bluetooth device disconnected");
                }
            }
            if (m.audio >= 0) pico_audio_ = m.audio == 2;
            if (m.kind == Kind::State) break;
            if (m.volume >= 0) s_.volume = (uint8_t)m.volume;
            if (m.scan >= 0) s_.scanning = (uint8_t)m.scan;
            if (m.cap_airpods >= 0) cap_airpods_ = m.cap_airpods == 1;
            if (m.version[0]) memcpy(s_.version, m.version, sizeof s_.version);
            if (m.i2s_error > 0 && !i2s_note_) note("Pico I2S input error — check the wires G5, G18, G19");
            if (m.i2s_error >= 0) i2s_note_ = m.i2s_error > 0;
            if (m.underruns >= 0) s_.pico_underruns = (uint32_t)m.underruns;
            if (m.dropped >= 0) s_.pico_overflows = (uint32_t)m.dropped;
            if (m.buffered >= 0 && st_ == kAcked) ringSample(m.buffered);   // the running stream's ring
            break;
        }
        case Kind::Airpods:
            s_.airpods = cap_airpods_ && m.ap_connected == 1;
            s_.airpods_left = (int8_t)m.ap_left;
            s_.airpods_right = (int8_t)m.ap_right;
            s_.airpods_case = (int8_t)m.ap_case;
            s_.airpods_mode = (int8_t)(m.ap_mode >= 1 ? m.ap_mode : -1);
            break;
        case Kind::AirpodsError: note("AirPods: ", m.text); break;
        case Kind::Codec: s_.sbc_rate = m.rate > 0 ? (uint32_t)m.rate : 0; break;
        case Kind::Device:
            if (listing_) {
                int i = 0;
                while (i < npend_ && memcmp(pend_[i].mac, m.dev.mac, sizeof m.dev.mac)) ++i;
                if (i < kMaxDevices) {
                    pend_[i] = m.dev;
                    if (i == npend_) ++npend_;
                }
            }
            break;
        case Kind::EndList:
            if (!listing_) break;
            listing_ = false;
            if (nlist_ != npend_ || memcmp(list_, pend_, sizeof(BtDevice) * (size_t)npend_)) {
                memcpy(list_, pend_, sizeof(BtDevice) * (size_t)npend_);
                nlist_ = npend_;
                list_changed_ = true;
            }
            break;
        case Kind::Selected:
            if (m.flag) {
                restarting(now);
                note("Chosen: ", m.text);
            } else {
                list_want_ = true;   // "SELECTED Already connected"
            }
            break;
        case Kind::Restart:
            restarting(now);
            scan_restart_ = m.flag;
            s_.scanning = m.flag;
            note(m.flag ? "Searching for Bluetooth devices (20 s)…" : "Reconnecting to the saved device…");
            break;
        case Kind::ScanDone:
            s_.scanning = 0;
            list_want_ = true;
            note("Search finished — pick a device");
            break;
        case Kind::Volume: s_.volume = (uint8_t)m.volume; break;
        case Kind::StreamAck:
            if (st_ == kSent && m.token == token_) {
                st_ = kAcked;
                resync_ = false;
            }
            break;
        case Kind::StreamFault:
            ++s_.faults;
            if (m.token != token_ || (st_ != kSent && st_ != kAcked)) break;   // an earlier stream
            if (m.fault == Fault::InputStalled) {
                note("Pico gets no audio — restarting the stream");
                startStream(now);
            } else if (m.fault == Fault::TransportFailed) {
                note("Bluetooth stream lost — retry in 5 s");
                st_ = kRetry;
                st_at_ = now;
            }
            break;
        case Kind::Recovery:
            ++s_.recoveries;
            note("Pico is renewing the Bluetooth stream");
            break;
        case Kind::TestStart: note("Test: left channel, silence, right channel"); break;
        case Kind::Standby:   // asleep on its own (or someone else's SLEEP): wake it again shortly
            dropLink();
            ph_ = Phase::Absent;
            retry_at_ = now + 600;
            attempts_ = 0;
            note("Pico fell asleep — waking it again");
            break;
        case Kind::Remote:
            if (rn_ == kRemoteLen) {   // full: the oldest goes
                rh_ = (rh_ + 1) % kRemoteLen;
                --rn_;
            }
            rq_[(rh_ + rn_) % kRemoteLen] = m.remote;
            ++rn_;
            break;
        case Kind::Error:   // an awake Pico takes the wake pulse for a garbage byte: not worth a notice
            if (!pre(m.text, "Invalid or oversized")) note(m.text);
            break;
        default: break;
        }
    }

    // derived fields + revision
    void commit()
    {
        s_.enabled = en_;
        s_.awake = ph_ == Phase::Online;
        s_.streaming = streamAcked() && pico_audio_;
        s_.device_count = (uint8_t)nlist_;
        s_.connected_name[0] = 0;
        for (int i = 0; s_.connected && i < nlist_; ++i)
            if (list_[i].selected) memcpy(s_.connected_name, list_[i].name, sizeof s_.connected_name);
        s_.revision = shown_.revision;
        if (list_changed_ || memcmp(&s_, &shown_, sizeof s_)) {
            ++s_.revision;
            memcpy(&shown_, &s_, sizeof s_);
            list_changed_ = false;
        }
    }

    BtStatus s_, shown_;
    Phase ph_;
    bool en_, probe_, listing_, list_want_, list_changed_, restarting_, scan_restart_, pico_audio_, resent_;
    bool rate_note_, i2s_note_, cap_airpods_, resync_;
    uint8_t st_;
    int attempts_, want_volume_;
    uint32_t t_, guard_, retry_at_, last_rx_, next_use_, next_status_, last_status_, list_at_, st_at_;
    uint32_t restart_at_, sleep_at_, token_, quiet_, resync_at_;
    int32_t fl_[kFloorSamples];   // the Pico's ring: the last STATUS samples (frames)
    int nfl_;
    int nlist_, npend_, qh_, qn_, ntx_, rh_, rn_;
    BtDevice list_[kMaxDevices], pend_[kMaxDevices];
    char q_[kQueueLen][kCmdMax + 1];
    char tx_[kTxMax];
    BtRemote rq_[kRemoteLen];
    LineAssembler in_;
};

// ---------------------------------------------------------------- hal.h commands (shared by bt.cpp / hal_host.cpp)
inline bool cmdSelect(Link& l, const char* mac)
{
    if (!macValid(mac)) return false;
    char c[32] = "SELECTMAC ";
    for (int i = 0; i < 17; ++i) c[10 + i] = mac[i] >= 'a' && mac[i] <= 'f' ? (char)(mac[i] - 32) : mac[i];
    c[27] = 0;
    return l.queue(c);
}
inline bool cmdAirpodsMode(Link& l, int mode)
{
    if (mode < 1 || mode > 4 || !l.status().airpods) return false;
    char c[24];
    snprintf(c, sizeof c, "AIRPODS MODE %d", mode);
    return l.queue(c);
}

}  // namespace hal::btp
