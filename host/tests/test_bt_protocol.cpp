// Build + run (Git Bash, project root):
// export ZIG_GLOBAL_CACHE_DIR=$PWD/host/.zig-cache/global ZIG_LOCAL_CACHE_DIR=$PWD/host/.zig-cache/local; zig c++ -target x86_64-windows-gnu -std=c++17 -O2 -Wall -Wextra -Ifirmware/main -Ihost -DHAL_HOST_NO_LVGL host/tests/test_bt_protocol.cpp host/hal_host.cpp firmware/main/core/settings.cpp -o host/bin/test_bt_protocol.exe && host/bin/test_bt_protocol.exe
// Tests the client of the Aurora "Pico Bluetooth bridge" UART protocol (firmware/main/hal/bt_protocol.h), the
// code hal/bt.cpp runs on the Tab5, and the host hal::bt* functions with the fake Pico (host/hal_host.cpp):
//  - LineAssembler: CR / LF / CRLF, 120-byte limit (STATUS 720, DEVICE 300), control characters, recovery;
//  - parse(): every line type (names with spaces and Polish UTF-8, negative RSSI, malformed fields);
//  - fuzz: > 5000 random / mutated lines into the parser and an awake link, invariants checked;
//  - the full timeline enable -> wake -> READY -> USE cadence -> LIST -> connect -> STREAM / ACK ->
//    INPUT_STALLED -> RECOVERY -> TRANSPORT_FAILED -> route off -> STOP -> disable -> SLEEP -> STANDBY;
//  - missing module (3 pulses, Polish notice, 15 s retries), silence re-wake, STANDBY timeout, 48 kHz;
//  - command queue order, the 80-character limit, VOLUME coalescing, commands held while the Pico restarts.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "hal/bt_protocol.h"
#include "hal_host.h"

using namespace hal::btp;
using hal::BtRemote;
static int fails = 0, checks = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++checks;                                                               \
        if (!(c)) {                                                             \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);             \
            ++fails;                                                            \
        }                                                                       \
    } while (0)

static uint32_t g_rng = 0x9E3779B9u;
static uint32_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

// ---------------------------------------------------------------- a link driven like the bridge task
struct Rig {
    Link link;
    uint32_t now = 0;
    Inputs in{false, false, 44100, 0};
    std::vector<std::string> sent;   // every line, in order
    std::vector<uint32_t> at;        // when
    std::string part;

    Rig() { link.reset(); }
    void pump()
    {
        char b[kTxMax];
        for (int n; (n = link.takeTx(b, (int)sizeof b)) > 0;)
            for (int i = 0; i < n; ++i) {
                if (b[i] != '\n') {
                    part += b[i];
                    continue;
                }
                sent.push_back(part);
                at.push_back(now);
                part.clear();
            }
    }
    void step()
    {
        link.step(now, in);
        pump();
    }
    void run(uint32_t ms)   // 20 ms ticks
    {
        for (uint32_t t = 0; t < ms; t += 20) {
            now += 20;
            step();
        }
    }
    void feed(const char* line)
    {
        std::string s = line;
        s += "\r\n";
        link.feed((const uint8_t*)s.data(), (int)s.size(), now);
        step();
    }
    int count(const char* cmd) const
    {
        int n = 0;
        for (const auto& s : sent) n += s == cmd;
        return n;
    }
    bool has(const char* cmd) const { return count(cmd) > 0; }
    void clear()
    {
        sent.clear();
        at.clear();
    }
    const hal::BtStatus& st() const { return link.status(); }
    // enable and answer the boot wait with READY
    void up()
    {
        in.enabled = true;
        step();
        run(200);
        feed("READY version=0.9.5 cap_airpods=1");
    }
};

// ---------------------------------------------------------------- line assembler
static void testAssembler()
{
    LineAssembler a;
    a.reset();
    a.rejected = 0;
    std::vector<std::string> got;
    auto put = [&](const std::string& s) {
        int rej = 0;
        for (unsigned char c : s) {
            const int r = a.put(c);
            if (r == 1) got.push_back(a.text());
            if (r < 0) ++rej;
        }
        return rej;
    };
    CHECK(put("USE\nSTATE connection=2 audio=0\r\nREADY\r\r\n\n") == 0);
    CHECK(got.size() == 3 && got[0] == "USE" && got[1] == "STATE connection=2 audio=0" && got[2] == "READY");
    got.clear();
    const std::string l120(120, 'x'), l121(121, 'y');
    CHECK(put(l120 + "\n") == 0 && got.size() == 1 && got[0] == l120);
    got.clear();
    CHECK(put(l121 + "\nOK\n") == 1 && got.size() == 1 && got[0] == "OK");   // rejected, the next line is fine
    got.clear();
    std::string st = "STATUS connection=2 audio=2";
    while (st.size() < 640) st += " k=1";
    CHECK(put(st + "\n") == 0 && got.size() == 1 && got[0] == st);
    got.clear();
    std::string st2 = "STATUS ";
    st2 += std::string(kStatusLineMax, 'z');
    CHECK(put(st2 + "\n") == 1 && got.empty());
    CHECK(put(std::string("DEV\x01ICE\n") + "A\x7f\n" + std::string("x\0y\n", 4)) == 3 && got.empty());
    CHECK(put("DEVICE 1 AA:BB:CC:DD:EE:FF -60 0 0 G\xc5\x82o\xc5\x9bnik\n") == 0 && got.size() == 1);   // UTF-8 passes
    CHECK(a.rejected == 5);
    got.clear();
    // A scan result carries the whole discovered name (Pico: name[249], "%s"): DEVICE lines up to 300 bytes.
    const std::string dev = "DEVICE 32 AA:BB:CC:DD:EE:FF -127 0 0 " + std::string(248, 'n');
    CHECK(dev.size() == 285 && put(dev + "\n") == 0 && got.size() == 1 && got[0] == dev);
    got.clear();
    const std::string dev300 = "DEVICE " + std::string(kDeviceLineMax - 7, 'd');
    CHECK(put(dev300 + "\n") == 0 && got.size() == 1 && got[0] == dev300);
    got.clear();
    CHECK(put(dev300 + "d\nOK\n") == 1 && got.size() == 1 && got[0] == "OK");   // 301: rejected
    got.clear();
    CHECK(put("DEVICES " + std::string(130, 'x') + "\n") == 1 && got.empty());   // only "DEVICE " gets the room
    CHECK(a.rejected == 7);
}

// ---------------------------------------------------------------- parser
static Msg P(const char* s)
{
    Msg m;
    parse(s, m);
    return m;
}

static void testParse()
{
    Msg m = P("READY version=0.9.5 cap_airpods=1");
    CHECK(m.kind == Kind::Ready && !strcmp(m.version, "0.9.5") && m.cap_airpods == 1);
    m = P("READY version=0.9.3 cap_airpods=0");
    CHECK(m.kind == Kind::Ready && !strcmp(m.version, "0.9.3") && m.cap_airpods == 0);
    m = P("READY");
    CHECK(m.kind == Kind::Ready && !m.version[0] && m.cap_airpods == -1);
    CHECK(P("READYX").kind == Kind::Unknown);
    m = P("READY version=0.9.5-airpods-candidate-very-long cap_airpods=7");
    CHECK(m.kind == Kind::Ready && strlen(m.version) == 15 && m.cap_airpods == -1);

    m = P("STATE connection=2 audio=2");
    CHECK(m.kind == Kind::State && m.connection == 2 && m.audio == 2);
    m = P("STATE connection=0 audio=0");
    CHECK(m.kind == Kind::State && m.connection == 0 && m.audio == 0);
    CHECK(P("STATE audio=2").kind == Kind::Malformed);
    CHECK(P("STATE connection=x audio=2").kind == Kind::Malformed);
    CHECK(P("STATE connection=99 audio=2").kind == Kind::Malformed);
    CHECK(P("STATE connection=2x audio=2").kind == Kind::Malformed);
    CHECK(P("STATE connection=-1 audio=2").kind == Kind::Malformed);
    m = P("STATE connection=1 audio=abc");
    CHECK(m.kind == Kind::State && m.connection == 1 && m.audio == -1);

    // the 0.9.5 STATUS line (pico-bridge/src/main.cpp) with realistic values
    const char* status =
        "STATUS connection=2 audio=2 target=selected stream=1 i2s_error=0 blocks=123456 buffered=2048 dropped=0 "
        "empty=12 heap=98765 volume=64 scan=0 saved=2 selected=1 standby=0 scandone=0 version=0.9.5 underruns=3 "
        "remote_key=68 remote_release=68 remote_presses=4 remote_releases=4 remote_accepted=4 rx_blocks=99999 "
        "callbacks=88888 consumed=4294967295 recoveries=1 input_stalls=2 flushes=0 rx_age=12 cb_age=8 "
        "stream_token=7 codec=SBC cap_airpods=1";
    CHECK(strlen(status) > (size_t)kLineMax && strlen(status) < (size_t)kStatusLineMax);
    m = P(status);
    CHECK(m.kind == Kind::Status && m.connection == 2 && m.audio == 2 && m.volume == 64 && m.scan == 0 &&
          m.i2s_error == 0 && m.cap_airpods == 1 && !strcmp(m.version, "0.9.5"));
    CHECK(m.buffered == 2048 && m.underruns == 3 && m.dropped == 0);   // 0.4.4: the Pico's buffer counters
    m = P("STATUS connection=2 audio=2 buffered=x underruns=4294967296 dropped=4294967295");
    CHECK(m.kind == Kind::Status && m.buffered == -1 && m.underruns == -1 && m.dropped == 4294967295ll);
    m = P("STATUS connection=0 audio=0 i2s_error=1 volume=101 scan=1");
    CHECK(m.buffered == -1 && m.underruns == -1 && m.dropped == -1);
    CHECK(m.kind == Kind::Status && m.i2s_error == 1 && m.volume == -1 && m.scan == 1 && m.cap_airpods == -1);
    CHECK(P("STATUS volume=3").kind == Kind::Malformed);
    m = P("STATUS connection=2 audio=0 xconnection=0 audio_age=7");   // keys match whole words only
    CHECK(m.connection == 2 && m.audio == 0);

    m = P("AIRPODS connected=1 mode=2 left=80 right=-1 case=55 ears=2 auto_pause=0");
    CHECK(m.kind == Kind::Airpods && m.ap_connected == 1 && m.ap_mode == 2 && m.ap_left == 80 && m.ap_right == -1 &&
          m.ap_case == 55);
    m = P("AIRPODS connected=0 mode=-1 left=-1 right=-1 case=-1 ears=-1 auto_pause=1");
    CHECK(m.kind == Kind::Airpods && m.ap_connected == 0 && m.ap_mode == -1);
    m = P("AIRPODS connected=1 mode=9 left=300 right=50 case=x");
    CHECK(m.kind == Kind::Airpods && m.ap_mode == -1 && m.ap_left == -1 && m.ap_right == 50 && m.ap_case == -1);
    CHECK(P("AIRPODS mode=2").kind == Kind::Malformed);
    CHECK(P("AIRPODS_PENDING connect").kind == Kind::AirpodsNote);
    m = P("AIRPODS_ERROR not_connected");
    CHECK(m.kind == Kind::AirpodsError && !strcmp(m.text, "not_connected"));
    m = P("CODEC type=SBC rate=44100 channels=2 bitpool_min=2 bitpool_max=53");
    CHECK(m.kind == Kind::Codec && m.rate == 44100);
    m = P("CODEC type=SBC rate=-1 channels=-1 bitpool_min=-1 bitpool_max=-1");
    CHECK(m.kind == Kind::Codec && m.rate == -1);
    CHECK(P("CODEC type=SBC rate=abc").kind == Kind::Malformed);
    CHECK(P("CODEC type=SBC").kind == Kind::Malformed);

    m = P("DEVICE 1 aa:bb:cc:dd:ee:0f -127 1 1 Sony WH-1000XM4");
    CHECK(m.kind == Kind::Device && !strcmp(m.dev.mac, "AA:BB:CC:DD:EE:0F") && m.dev.rssi == 0 && m.dev.saved == 1 &&
          m.dev.selected == 1 && !strcmp(m.dev.name, "Sony WH-1000XM4"));
    m = P("DEVICE 12 11:22:33:44:55:66 -58 0 0 G\xc5\x82o\xc5\x9bnik \xc5\x81\xc3\xb3\xc5\xbc\xc4\x99 \xc5\x9a\xc4\x87");
    CHECK(m.kind == Kind::Device && m.dev.rssi == -58 && !m.dev.saved &&
          !strcmp(m.dev.name, "G\xc5\x82o\xc5\x9bnik \xc5\x81\xc3\xb3\xc5\xbc\xc4\x99 \xc5\x9a\xc4\x87"));
    m = P("DEVICE 3 AA:BB:CC:DD:EE:01 5 0 0");
    CHECK(m.kind == Kind::Device && m.dev.rssi == 5 && !strcmp(m.dev.name, "AA:BB:CC:DD:EE:01"));
    m = P("DEVICE 4 AA:BB:CC:DD:EE:02 -90 0 0    ");   // only spaces -> the MAC
    CHECK(m.kind == Kind::Device && !strcmp(m.dev.name, "AA:BB:CC:DD:EE:02"));
    m = P("DEVICE 5 AA:BB:CC:DD:EE:03 -90 0 0 Bad\xff\xfe UTF \xc5 x \xe2\x82");
    CHECK(m.kind == Kind::Device && !strcmp(m.dev.name, "Bad?? UTF ? x ??"));
    {   // the Pico cuts names at 79 bytes, BtDevice keeps 47: never half a UTF-8 sequence
        std::string name;
        for (int i = 0; i < 40; ++i) name += "\xc5\x82";   // 80 bytes of 'ł'
        std::string line = "DEVICE 6 AA:BB:CC:DD:EE:04 -70 0 0 " + name.substr(0, 79);
        m = P(line.c_str());
        CHECK(m.kind == Kind::Device && strlen(m.dev.name) == 46 && (uint8_t)m.dev.name[45] == 0x82);
        // a scan result keeps the whole discovered name (up to 248 bytes): cut to 47 on a UTF-8 boundary too
        std::string longName;
        for (int i = 0; i < 82; ++i) longName += "\xe6\x97\xa5";   // 246 bytes of CJK
        line = "DEVICE 9 AA:BB:CC:DD:EE:05 -66 0 0 " + longName;
        m = P(line.c_str());
        CHECK(line.size() > (size_t)kLineMax && m.kind == Kind::Device && m.dev.rssi == -66 && strlen(m.dev.name) == 45 &&
              !memcmp(m.dev.name, longName.data(), 45));
    }
    CHECK(P("DEVICE 0 AA:BB:CC:DD:EE:FF -60 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 33 AA:BB:CC:DD:EE:FF -60 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE -60 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FG -60 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA-BB-CC-DD-EE-FF -60 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FF -128 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FF 21 0 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FF -60 2 0 x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FF -60 0").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FF").kind == Kind::Malformed);
    CHECK(P("DEVICE x").kind == Kind::Malformed);
    CHECK(P("DEVICE 1 AA:BB:CC:DD:EE:FF --6 0 0 x").kind == Kind::Malformed);

    CHECK(P("END_LIST").kind == Kind::EndList);
    CHECK(P("END_LIST x").kind == Kind::Unknown);
    m = P("SELECTED Sony WH-1000XM4; restarting to connect");
    CHECK(m.kind == Kind::Selected && m.flag && !strcmp(m.text, "Sony WH-1000XM4"));
    m = P("SELECTED Already connected");
    CHECK(m.kind == Kind::Selected && !m.flag && !strcmp(m.text, "Already connected"));
    m = P("RESTART scanning; saved target preserved");
    CHECK(m.kind == Kind::Restart && m.flag);
    m = P("RESTART reconnecting to saved device");
    CHECK(m.kind == Kind::Restart && !m.flag);
    CHECK(P("SCAN_DONE").kind == Kind::ScanDone);
    m = P("VOLUME 55");
    CHECK(m.kind == Kind::Volume && m.volume == 55);
    CHECK(P("VOLUME 101").kind == Kind::Malformed && P("VOLUME abc").kind == Kind::Malformed &&
          P("VOLUME 5 6").kind == Kind::Malformed && P("VOLUME -1").kind == Kind::Malformed);
    m = P("STREAM_ACK 7");
    CHECK(m.kind == Kind::StreamAck && m.token == 7);
    m = P("STREAM_ACK 4294967295");
    CHECK(m.kind == Kind::StreamAck && m.token == 4294967295u);
    CHECK(P("STREAM_ACK 4294967296").kind == Kind::Malformed && P("STREAM_ACK 12345678901").kind == Kind::Malformed &&
          P("STREAM_ACK").kind == Kind::Unknown && P("STREAM_ACK 7 x").kind == Kind::Malformed);
    m = P("STREAM_FAULT 7 INPUT_STALLED");
    CHECK(m.kind == Kind::StreamFault && m.token == 7 && m.fault == Fault::InputStalled);
    m = P("STREAM_FAULT 9 TRANSPORT_FAILED");
    CHECK(m.kind == Kind::StreamFault && m.token == 9 && m.fault == Fault::TransportFailed);
    m = P("STREAM_FAULT 9 SOMETHING_NEW");
    CHECK(m.kind == Kind::StreamFault && m.fault == Fault::Other);
    CHECK(P("STREAM_FAULT x INPUT_STALLED").kind == Kind::Malformed && P("STREAM_FAULT 7").kind == Kind::Malformed);
    m = P("RECOVERY 7 TRANSPORT_STALLED");
    CHECK(m.kind == Kind::Recovery && m.token == 7);
    CHECK(P("RECOVERY x").kind == Kind::Malformed);
    CHECK(P("SILENT").kind == Kind::Silent && P("STANDBY").kind == Kind::Standby);
    CHECK(P("TEST START: LEFT / silence / RIGHT").kind == Kind::TestStart);
    CHECK(P("REMOTE PLAY").remote == BtRemote::Play && P("REMOTE PAUSE").remote == BtRemote::Pause &&
          P("REMOTE NEXT").remote == BtRemote::Next && P("REMOTE PREVIOUS").remote == BtRemote::Previous);
    CHECK(P("REMOTE PLAY").kind == Kind::Remote && P("REMOTE STOP").kind == Kind::Malformed);
    m = P("ERROR Headphones not connected");
    CHECK(m.kind == Kind::Error && !strcmp(m.text, "Headphones not connected"));
    m = P("ERROR");
    CHECK(m.kind == Kind::Error && !m.text[0]);
    CHECK(P("ERRORS x").kind == Kind::Unknown);
    CHECK(P("").kind == Kind::Unknown && P(nullptr).kind == Kind::Unknown && P("hello world").kind == Kind::Unknown &&
          P("STREAM ON: Tab5 I2S to Bluetooth").kind == Kind::Unknown && P("TEST DONE; silent").kind == Kind::Unknown);
    CHECK(macValid("aa:BB:cc:DD:ee:FF") && !macValid("AA:BB:CC:DD:EE:F") && !macValid("AA:BB:CC:DD:EE:FFF") &&
          !macValid(nullptr) && !macValid("AA:BB:CC:DD:EE;FF"));
}

// ---------------------------------------------------------------- fuzz
static bool terminated(const char* s, size_t cap) { return memchr(s, 0, cap) != nullptr; }

static void testFuzz()
{
    static const char* seeds[] = {
        "READY version=0.9.5 cap_airpods=1", "STATE connection=2 audio=2", "STATUS connection=2 audio=2 volume=40",
        "AIRPODS connected=1 mode=2 left=80 right=70 case=60 ears=2 auto_pause=0",
        "CODEC type=SBC rate=44100 channels=2 bitpool_min=2 bitpool_max=53",
        "DEVICE 1 AA:BB:CC:DD:EE:FF -60 1 1 Name", "END_LIST", "SELECTED X; restarting to connect",
        "RESTART scanning; saved target preserved", "SCAN_DONE", "VOLUME 40", "STREAM_ACK 1",
        "STREAM_FAULT 1 INPUT_STALLED", "RECOVERY 1 TRANSPORT_STALLED", "SILENT", "STANDBY", "REMOTE PLAY",
        "ERROR x", "AIRPODS_ERROR y", "TEST START: LEFT / silence / RIGHT"};
    Rig r;
    r.up();
    int lines = 0;
    for (int i = 0; i < 6000; ++i) {
        std::string s;
        const uint32_t mode = rnd() % 4;
        if (mode == 0) {   // random bytes, sometimes very long
            const int n = (int)(rnd() % (rnd() % 8 == 0 ? 900 : 100));
            for (int k = 0; k < n; ++k) s += (char)(rnd() & 0xFF);
        } else {           // a mutated protocol line
            s = seeds[rnd() % (sizeof seeds / sizeof seeds[0])];
            const int edits = 1 + (int)(rnd() % 4);
            for (int k = 0; k < edits && !s.empty(); ++k) {
                const size_t at = rnd() % s.size();
                switch (rnd() % 4) {
                case 0: s[at] = (char)(rnd() & 0xFF); break;
                case 1: s.erase(at, 1 + rnd() % 3); break;
                case 2: s.insert(at, 1, (char)(' ' + rnd() % 95)); break;
                default: s.insert(at, std::to_string((int)(rnd() % 200000) - 100000)); break;
                }
            }
        }
        Msg m;
        std::string z = s.substr(0, s.find('\0'));
        parse(z.c_str(), m);   // the parser alone, also on what the assembler would reject
        CHECK(terminated(m.text, sizeof m.text) && terminated(m.version, sizeof m.version) &&
              terminated(m.dev.name, sizeof m.dev.name));
        s += (rnd() & 1) ? "\n" : "\r\n";
        r.link.feed((const uint8_t*)s.data(), (int)s.size(), r.now);
        ++lines;
        if (i % 7 == 0) {
            r.in.route = (rnd() & 3) != 0;
            r.run(20 * (1 + rnd() % 30));
        }
        if (i % 500 == 0) r.link.popRemote();
    }
    r.step();
    const hal::BtStatus& st = r.st();
    CHECK(lines >= 5000);
    CHECK(st.device_count <= kMaxDevices && terminated(st.message, sizeof st.message) &&
          terminated(st.version, sizeof st.version) && terminated(st.connected_name, sizeof st.connected_name));
    hal::BtDevice d[kMaxDevices + 4];
    const int n = r.link.devices(d, kMaxDevices + 4);
    CHECK(n == st.device_count);
    for (int i = 0; i < n; ++i) CHECK(terminated(d[i].name, sizeof d[i].name) && macValid(d[i].mac));
    bool short_lines = true;
    for (const auto& s : r.sent) short_lines = short_lines && !s.empty() && s.size() <= (size_t)kCmdMax;
    CHECK(short_lines);
    CHECK(r.link.phase() != Phase::Off);
    // after the storm a clean READY still brings the link back
    r.feed("READY version=0.9.5 cap_airpods=1");
    CHECK(r.link.phase() == Phase::Online && !strcmp(r.st().version, "0.9.5"));
}

// ---------------------------------------------------------------- the full timeline
static void testTimeline()
{
    Rig r;
    CHECK(!r.link.uartOn() && r.st().revision == 0);
    r.step();
    CHECK(r.sent.empty() && r.link.phase() == Phase::Off);
    // enable -> wake pulse
    r.in.enabled = true;
    r.step();
    CHECK(r.link.phase() == Phase::Wake && r.link.wakeLow() && r.link.uartOn() && r.st().enabled && !r.st().module);
    r.run(100);
    CHECK(r.link.wakeLow());
    r.run(20);
    CHECK(!r.link.wakeLow() && r.link.phase() == Phase::Boot && r.sent.empty());
    // the probe for an already awake Pico 650 ms after the pulse
    r.run(620);
    CHECK(r.sent.empty());
    r.run(40);
    CHECK(r.sent.size() == 2 && r.sent[0] == "USE" && r.sent[1] == "STATUS");
    r.clear();
    // READY (any reply would do)
    const uint32_t rev = r.st().revision;
    r.feed("READY version=0.9.5 cap_airpods=1");
    CHECK(r.link.phase() == Phase::Online && r.st().module && r.st().awake && !strcmp(r.st().version, "0.9.5"));
    CHECK(r.st().revision > rev);
    CHECK(r.sent.size() == 2 && r.sent[0] == "USE" && r.sent[1] == "LIST");
    r.clear();
    // STATUS 300 ms after READY, USE every 3 s
    r.run(300);
    CHECK(r.has("STATUS"));
    r.clear();
    r.feed("DEVICE 1 AA:BB:CC:DD:EE:01 -127 1 1 Sony WH-1000XM4");
    r.feed("DEVICE 2 AA:BB:CC:DD:EE:02 -64 0 0 G\xc5\x82o\xc5\x9bnik");
    r.feed(("DEVICE 3 AA:BB:CC:DD:EE:03 -71 0 0 " + std::string(200, 'L')).c_str());   // long scan name: listed
    CHECK(r.st().device_count == 0);   // committed on END_LIST
    r.feed("END_LIST");
    hal::BtDevice d[4];
    CHECK(r.st().device_count == 3 && r.link.devices(d, 4) == 3 && d[0].saved && d[0].selected && d[1].rssi == -64);
    CHECK(!strcmp(d[2].mac, "AA:BB:CC:DD:EE:03") && strlen(d[2].name) == 47 && d[2].rssi == -71);
    CHECK(!r.st().connected && !r.st().connected_name[0]);
    for (int i = 0; i < 4; ++i) {   // keep talking so no probe interferes: STATE every 2 s
        r.run(2000);
        r.feed("STATE connection=0 audio=0");
    }
    std::vector<uint32_t> uses;
    for (size_t i = 0; i < r.sent.size(); ++i)
        if (r.sent[i] == "USE") uses.push_back(r.at[i]);
    CHECK(uses.size() >= 2);
    for (size_t i = 1; i < uses.size(); ++i) CHECK(uses[i] - uses[i - 1] == 3000);
    r.clear();
    // connect -> LIST again, connected name from the selected row
    r.feed("STATE connection=2 audio=0");
    CHECK(r.st().connected && r.has("LIST") && !strcmp(r.st().connected_name, "Sony WH-1000XM4"));
    r.feed("DEVICE 1 AA:BB:CC:DD:EE:01 -127 1 1 Sony WH-1000XM4");
    r.feed("END_LIST");
    r.clear();
    // route on -> STREAM 1, resent once after 1 s without ACK
    r.in.route = true;
    r.step();
    CHECK(r.has("STREAM 1") && r.link.streamOn() && !r.link.streamAcked());
    r.clear();
    r.run(1000);
    CHECK(r.count("STREAM 1") == 1);
    r.feed("STREAM_ACK 99");   // stale token: ignored
    CHECK(!r.link.streamAcked());
    r.feed("STREAM_ACK 1");
    CHECK(r.link.streamAcked() && !r.st().streaming);
    r.feed("STATE connection=2 audio=2");
    CHECK(r.st().streaming);
    r.clear();
    // INPUT_STALLED -> a new STREAM with a new token
    r.feed("STREAM_FAULT 1 INPUT_STALLED");
    CHECK(r.has("STREAM 2") && r.st().faults == 1 && !r.link.streamAcked() && r.link.streamOn());
    r.feed("STREAM_FAULT 1 INPUT_STALLED");   // late duplicate of the old stream: counted, no new STREAM
    CHECK(r.st().faults == 2 && !r.has("STREAM 3"));
    r.feed("STREAM_ACK 2");
    CHECK(r.link.streamAcked());
    // RECOVERY: noted
    r.feed("RECOVERY 2 TRANSPORT_STALLED");
    CHECK(r.st().recoveries == 1 && r.link.streamAcked() && strstr(r.st().message, "odnawia"));
    // TRANSPORT_FAILED: notice, next STREAM after 5 s
    r.clear();
    r.feed("STREAM_FAULT 2 TRANSPORT_FAILED");
    CHECK(r.st().faults == 3 && !r.link.streamAcked() && strstr(r.st().message, "5 s"));
    for (int i = 0; i < 4; ++i) {
        r.run(1000);
        r.feed("STATE connection=2 audio=0");
    }
    CHECK(!r.has("STREAM 3"));
    r.run(1000);
    CHECK(r.has("STREAM 3"));
    r.feed("STREAM_ACK 3");
    CHECK(r.link.streamAcked());
    // route off -> STOP
    r.clear();
    r.in.route = false;
    r.step();
    CHECK(r.sent.size() == 1 && r.sent[0] == "STOP" && !r.link.streamOn());
    r.feed("SILENT");
    // headphone buttons
    r.feed("REMOTE PLAY");
    r.feed("REMOTE NEXT");
    CHECK(r.link.popRemote() == BtRemote::Play && r.link.popRemote() == BtRemote::Next &&
          r.link.popRemote() == BtRemote::None);
    // route on again, then disable while streaming -> STOP, SLEEP, wait for STANDBY
    r.in.route = true;
    r.step();
    r.feed("STREAM_ACK 4");
    CHECK(r.link.streamAcked() && r.link.token() == 4);
    r.clear();
    r.in.enabled = false;
    r.step();
    CHECK(r.sent.size() == 2 && r.sent[0] == "STOP" && r.sent[1] == "SLEEP");
    CHECK(r.link.phase() == Phase::Stopping && r.link.uartOn() && !r.st().connected && !r.link.streamOn());
    r.clear();
    r.run(1000);
    CHECK(r.sent.empty());   // nothing after SLEEP: a start bit would wake the Pico again
    r.feed("STANDBY");
    CHECK(r.link.phase() == Phase::Off && !r.link.uartOn() && !r.st().awake && !r.st().enabled);
    r.run(20000);
    CHECK(r.sent.empty());
    // idle steps do not touch the revision
    const uint32_t rv = r.st().revision;
    r.run(1000);
    CHECK(r.st().revision == rv);
}

// ---------------------------------------------------------------- missing module, silence, standby timeout
static void testRecovery()
{
    {   // nobody answers: pulses at 0, 3 s, 6 s, then the notice and one try every 15 s
        Rig r;
        r.in.enabled = true;
        std::vector<uint32_t> pulses;
        bool low = false;
        for (int i = 0; i < 2500; ++i) {   // 50 s
            r.step();
            if (r.link.wakeLow() && !low) pulses.push_back(r.now);
            low = r.link.wakeLow();
            r.now += 20;
        }
        CHECK(pulses.size() == 5);
        if (pulses.size() == 5) {
            CHECK(pulses[0] == 0 && pulses[1] == 3000 && pulses[2] == 6000);
            CHECK(pulses[3] == 9000 + kAbsentRetryMs && pulses[4] == pulses[3] + 3000 + kAbsentRetryMs);
        }
        CHECK(r.count("USE") == 5 && r.count("STATUS") == 5);   // one probe per pulse
        CHECK(!r.st().module && !strcmp(r.st().message, "Nie wykryto modułu Bluetooth (Pico) — sprawdź podłączenie"));
        CHECK(r.link.phase() == Phase::Absent || r.link.phase() == Phase::Boot || r.link.phase() == Phase::Wake);
        // the module shows up later (plugged in): any reply brings the link up
        while (r.link.phase() != Phase::Absent) r.run(20);
        r.feed("STATUS connection=0 audio=0 volume=31 version=0.9.5 cap_airpods=1");
        CHECK(r.link.phase() == Phase::Online && r.st().module && r.st().volume == 31);
        // disable while absent: straight off, nothing sent
        Rig q;
        q.in.enabled = true;
        q.step();
        q.run(10000);
        CHECK(q.link.phase() == Phase::Absent);
        q.clear();
        q.in.enabled = false;
        q.step();
        CHECK(q.link.phase() == Phase::Off && q.sent.empty() && !q.link.uartOn());
    }
    {   // awake, then the Pico falls silent: STATUS probes every 5 s, re-wake after 12 s
        Rig r;
        r.up();
        r.clear();
        r.run(11960);
        CHECK(r.count("STATUS") >= 2 && r.link.phase() == Phase::Online);
        r.run(60);
        CHECK(r.link.phase() == Phase::Wake && r.link.wakeLow() && !r.st().awake &&
              strstr(r.st().message, "nie odpowiada"));
        r.run(200);
        r.feed("READY version=0.9.5 cap_airpods=1");
        CHECK(r.link.phase() == Phase::Online);
    }
    {   // disable during the wake pulse: SLEEP only after the boot guard, STANDBY never comes -> off after 2.5 s
        Rig r;
        r.in.enabled = true;
        r.step();
        r.run(40);
        r.in.enabled = false;
        r.run(100);
        CHECK(r.sent.empty() && r.link.phase() == Phase::Boot);
        r.run(660);
        CHECK(r.sent.size() == 1 && r.sent[0] == "SLEEP" && r.link.phase() == Phase::Stopping);
        r.run(2460);
        CHECK(r.link.phase() == Phase::Stopping);
        r.run(40);
        CHECK(r.link.phase() == Phase::Off && strstr(r.st().message, "nie potwierdził"));
    }
    {   // the Pico goes to sleep on its own: woken again
        Rig r;
        r.up();
        r.feed("STANDBY");
        CHECK(r.link.phase() == Phase::Absent && !r.st().awake);
        r.run(620);
        CHECK(r.link.phase() == Phase::Wake);
    }
    {   // 48 kHz: no stream, a notice; 44.1 kHz streams
        Rig r;
        r.in.audio_rate = 48000;
        r.up();
        r.feed("STATE connection=2 audio=0");
        r.in.route = true;
        r.clear();
        r.run(100);
        CHECK(!r.link.streamOn() && !strcmp(r.st().message, "Bluetooth wymaga 44,1 kHz"));
        for (const auto& s : r.sent) CHECK(s.compare(0, 6, "STREAM") != 0);
        r.in.audio_rate = 44100;
        r.step();
        CHECK(r.has("STREAM 1") && r.link.streamOn());
        r.in.audio_rate = 48000;
        r.clear();
        r.step();
        CHECK(r.has("STOP") && !r.link.streamOn());
        r.in.audio_rate = 0;   // audio stopped: the bridge I2S runs on its own
        r.step();
        CHECK(r.has("STREAM 2"));
        // the headphones disconnect: STOP, nothing until they are back
        r.clear();
        r.feed("STATE connection=0 audio=0");
        CHECK(r.has("STOP") && !r.link.streamOn() && strstr(r.st().message, "rozłączone"));
        // a Pico restart (READY) drops the stream without a STOP
        r.feed("STATE connection=2 audio=0");
        CHECK(r.has("STREAM 3"));
        r.clear();
        r.feed("READY version=0.9.5 cap_airpods=1");
        CHECK(!r.has("STOP") && !r.link.streamOn() && !r.st().connected);
    }
    {   // AirPods: battery, mode; only with cap_airpods=1
        Rig r;
        r.up();
        r.feed("STATE connection=2 audio=0");
        r.feed("AIRPODS connected=1 mode=2 left=80 right=75 case=-1 ears=2 auto_pause=0");
        r.feed("CODEC type=SBC rate=44100 channels=2 bitpool_min=2 bitpool_max=53");
        CHECK(r.st().airpods && r.st().airpods_left == 80 && r.st().airpods_right == 75 && r.st().airpods_case == -1 &&
              r.st().airpods_mode == 2 && r.st().sbc_rate == 44100);
        r.feed("STATE connection=0 audio=0");
        CHECK(!r.st().airpods && r.st().airpods_left == -1 && r.st().airpods_mode == -1);
        Rig q;
        q.in.enabled = true;
        q.step();
        q.run(200);
        q.feed("READY version=0.9.3 cap_airpods=0");
        q.feed("AIRPODS connected=1 mode=2 left=80 right=75 case=-1 ears=2 auto_pause=0");
        CHECK(!q.st().airpods);
        q.feed("STATUS connection=0 audio=0 i2s_error=1");
        CHECK(strstr(q.st().message, "I2S"));
        q.feed("ERROR Saved-device list is full (8)");
        CHECK(!strcmp(q.st().message, "Saved-device list is full (8)"));
        q.feed("ERROR Invalid or oversized command");   // the wake pulse seen by an awake Pico: no notice
        CHECK(!strcmp(q.st().message, "Saved-device list is full (8)"));
        // an already awake Pico answers the boot probe with that ERROR first: still a reply
        Rig w;
        w.in.enabled = true;
        w.step();
        w.run(800);
        CHECK(w.has("USE") && w.has("STATUS") && w.link.phase() == Phase::Boot);
        w.feed("ERROR Invalid or oversized command");
        CHECK(w.link.phase() == Phase::Online && w.st().module && !strstr(w.st().message, "Invalid"));
    }
}

// ---------------------------------------------------------------- command queue
static void testQueue()
{
    Rig r;
    std::string c80(80, 'A'), c81(81, 'A');
    CHECK(r.link.queue(c80.c_str()) && !r.link.queue(c81.c_str()));
    CHECK(!r.link.queue("") && !r.link.queue(nullptr) && !r.link.queue("SCAN\n") && !r.link.queue("G\xc5\x82o"));
    r.link.reset();
    // queued before the link is up: kept, sent in order, one per step, once awake
    CHECK(r.link.queue("SCAN"));
    CHECK(cmdSelect(r.link, "aa:bb:cc:dd:ee:ff"));
    CHECK(!cmdSelect(r.link, "aa:bb:cc:dd:ee"));
    CHECK(r.link.queue("TEST"));
    CHECK(r.link.setVolume(40) && r.link.setVolume(55) && r.link.queued() == 4);   // coalesced
    CHECK(!cmdAirpodsMode(r.link, 2));   // no AirPods connected
    CHECK(r.link.queue("RECONNECT"));
    for (int i = 0; i < 3; ++i) CHECK(r.link.queue("TEST"));
    CHECK(!r.link.queue("TEST") && r.link.queued() == kQueueLen);   // full
    r.up();   // READY: the first command goes out with the step that follows it
    r.run(20 * kQueueLen + 20);
    std::vector<std::string> cmds;
    for (const auto& s : r.sent)
        if (s != "USE" && s != "STATUS" && s != "LIST") cmds.push_back(s);
    CHECK(cmds.size() == (size_t)kQueueLen);
    if (cmds.size() == (size_t)kQueueLen) {
        CHECK(cmds[0] == "SCAN" && cmds[1] == "SELECTMAC AA:BB:CC:DD:EE:FF" && cmds[2] == "TEST" &&
              cmds[3] == "VOLUME 55" && cmds[4] == "RECONNECT" && cmds[7] == "TEST");
    }
    for (const auto& s : r.sent) CHECK(s.size() <= (size_t)kCmdMax);
    // the Pico restarts (SCAN): commands wait for READY, the volume is sent again after it
    r.clear();
    r.feed("RESTART scanning; saved target preserved");
    CHECK(r.st().scanning && !r.st().connected);
    CHECK(r.link.queue("TEST"));
    r.run(200);
    CHECK(!r.has("TEST"));
    r.feed("READY version=0.9.5 cap_airpods=1");
    CHECK(r.st().scanning);
    r.run(100);
    CHECK(r.has("TEST") && r.has("VOLUME 55"));
    // while scanning: LIST every 4 s; SCAN_DONE ends it with one more LIST
    r.feed("END_LIST");
    r.clear();
    r.run(4100);
    CHECK(r.count("LIST") == 1);
    r.feed("END_LIST");
    r.clear();
    r.feed("SCAN_DONE");
    CHECK(!r.st().scanning && r.has("LIST") && strstr(r.st().message, "zakończone"));
    // AirPods mode only with AirPods connected
    r.feed("STATE connection=2 audio=0");
    r.feed("AIRPODS connected=1 mode=1 left=50 right=50 case=50 ears=2 auto_pause=0");
    CHECK(cmdAirpodsMode(r.link, 3) && !cmdAirpodsMode(r.link, 5) && !cmdAirpodsMode(r.link, 0));
    r.clear();
    r.run(40);
    CHECK(r.has("AIRPODS MODE 3"));
    // SELECTED ...; restarting holds the queue too, "Already connected" asks for the list
    r.feed("END_LIST");   // the LIST after SCAN_DONE
    r.feed("END_LIST");   // the LIST after the connect
    r.clear();
    r.feed("SELECTED Already connected");
    CHECK(r.has("LIST"));
    r.feed("SELECTED Sony; restarting to connect");
    CHECK(!strcmp(r.st().message, "Wybrano: Sony"));
    // disable clears the queue
    CHECK(r.link.queue("TEST"));
    r.in.enabled = false;
    r.step();
    CHECK(r.link.queued() == 0);
}

// ---------------------------------------------------------------- the host hal::bt* + fake Pico helpers
// 0.4.4: the Pico's buffer. While the stream is acknowledged STATUS goes out every 2 s; its buffered= samples give
// the floor (lowest of the last 6); a floor above kResyncFloor in a silent moment -> a new STREAM token (the Pico
// empties its ring), at most every 20 s, the local output staying muted until the new ACK.
static void testBuffer()
{
    Rig r;
    r.up();
    r.feed("STATE connection=2 audio=0");
    r.in.route = true;
    r.step();
    CHECK(r.has("STREAM 1"));
    r.feed("STREAM_ACK 1");
    r.feed("STATE connection=2 audio=2");
    CHECK(r.link.streamAcked() && r.st().streaming && r.st().pico_buffered == -1 && r.st().pico_floor == -1);
    auto status = [&](int buffered, int under) {
        char s[200];
        snprintf(s, sizeof s,
                 "STATUS connection=2 audio=2 stream=1 i2s_error=0 buffered=%d dropped=7 underruns=%d volume=64 scan=0 "
                 "version=0.9.5",
                 buffered, under);
        r.feed(s);
    };
    // the first STATUS of the stream (old cadence), then every 2 s
    r.clear();
    for (int i = 0; i < 16 && !r.has("STATUS"); ++i) {
        r.run(1000);
        r.feed("STATE connection=2 audio=2");   // (keeps the link from going silent)
    }
    CHECK(r.has("STATUS"));
    const int32_t samples[6] = {1500, 2600, 1400, 2300, 1300, 2500};
    for (int i = 0; i < 6; ++i) {
        r.clear();
        status(samples[i], 2);
        CHECK(r.st().pico_buffered == samples[i]);
        CHECK(r.st().pico_floor == (i < 5 ? -1 : 1300));   // the floor after 6 samples
        r.run(2000);
        CHECK(r.count("STATUS") == 1);   // every 2 s while streaming
    }
    CHECK(r.st().pico_underruns == 2 && r.st().pico_overflows == 7);
    // not silent: no re-sync even with a high floor (and none before 20 s of stream)
    r.clear();
    r.run(8000);
    CHECK(!r.has("STREAM 2") && r.st().resyncs == 0);
    // silent: a re-sync now, the local output stays muted until its ACK
    r.in.quiet_ms = 700;
    r.step();
    CHECK(r.has("STREAM 2") && r.st().resyncs == 1 && r.link.streamAcked() && r.st().pico_floor == -1);
    status(4000, 2);   // a sample before the ACK belongs to the old ring: ignored
    CHECK(r.st().pico_buffered == -1);
    r.feed("STREAM_ACK 2");
    CHECK(r.link.streamAcked());
    // high again at once: the next re-sync waits 20 s after the last one
    for (int i = 0; i < 6; ++i) {
        status(1800, 2);
        r.run(2000);
    }
    CHECK(r.st().pico_floor == 1800 && !r.has("STREAM 3"));
    r.clear();
    for (int i = 0; i < 6 && !r.has("STREAM 3"); ++i) {
        status(1800, 2);
        r.run(2000);
    }
    CHECK(r.has("STREAM 3") && r.st().resyncs == 2);
    r.feed("STREAM_ACK 3");
    // a low floor (the ring between pulls near empty): no re-sync
    r.clear();
    const int32_t low[6] = {200, 1500, 300, 1400, 250, 1450};
    for (int i = 0; i < 18; ++i) {   // 36 s (replies keep the link from going silent)
        status(low[i % 6], 3);
        r.run(2000);
    }
    CHECK(r.st().pico_floor == 200 && !r.has("STREAM 4") && r.st().resyncs == 2);
    // route off: the counters of the ring are gone with the stream
    r.in.route = false;
    r.step();
    CHECK(r.has("STOP") && !r.link.streamAcked() && r.st().pico_floor == 200);   // (kept until a new stream / drop)
}

static void testHost()
{
    char b[128];
    halHostBtReset();
    hal::BtStatus st;
    hal::btStatus(st);
    CHECK(!st.enabled && !st.module && halHostBtTakeSent(b, sizeof b) == 0);
    CHECK(!hal::btScan());   // disabled
    CHECK(hal::btEnable(true) && halHostBtWakeLow());
    halHostBtAdvance(800);
    CHECK(halHostBtNow() == 800 && !halHostBtWakeLow());
    CHECK(halHostBtTakeSent(b, sizeof b) == 3 && !strcmp(b, "USE"));
    CHECK(halHostBtTakeSent(b, sizeof b) == 6 && !strcmp(b, "STATUS"));
    CHECK(halHostBtTakeSent(b, sizeof b) == 0);
    halHostBtFeed("READY version=0.9.5 cap_airpods=1");
    CHECK(halHostBtTakeSent(b, sizeof b) == 3 && !strcmp(b, "USE"));
    CHECK(halHostBtTakeSent(b, sizeof b) == 4 && !strcmp(b, "LIST"));
    halHostBtFeed("DEVICE 1 AA:BB:CC:DD:EE:01 -127 1 1 Sony WH-1000XM4");
    halHostBtFeed("END_LIST");
    halHostBtFeed("STATE connection=2 audio=0");
    hal::btStatus(st);
    CHECK(st.enabled && st.module && st.awake && st.connected && !strcmp(st.connected_name, "Sony WH-1000XM4"));
    hal::BtDevice d[2];
    CHECK(hal::btDevices(d, 2) == 1 && !strcmp(d[0].mac, "AA:BB:CC:DD:EE:01"));
    while (halHostBtTakeSent(b, sizeof b)) {}
    CHECK(hal::btScan() && hal::btSelect("11:22:33:44:55:66") && hal::btSetVolume(70) && hal::btTest() &&
          hal::btReconnect() && !hal::btSelect("bad") && !hal::btAirpodsMode(2));
    halHostBtAdvance(100);
    std::vector<std::string> got;
    while (halHostBtTakeSent(b, sizeof b)) got.push_back(b);
    CHECK(got.size() == 5);
    if (got.size() == 5)
        CHECK(got[0] == "SCAN" && got[1] == "SELECTMAC 11:22:33:44:55:66" && got[2] == "VOLUME 70" && got[3] == "TEST" &&
              got[4] == "RECONNECT");
    // the route: STREAM, and after the ACK the local output is silenced (local = false)
    hal::btSetAudioRoute(true, false);
    CHECK(halHostBtTakeSent(b, sizeof b) && !strcmp(b, "STREAM 1"));
    struct Tone {
        static void render(int16_t* lr, int frames, void*)
        {
            for (int i = 0; i < frames * 2; ++i) lr[i] = 1000;
        }
    };
    CHECK(hal::audioStart(44100, 64, &Tone::render, nullptr));
    int16_t buf[256 * 2];
    halHostRenderAudio(256, buf);
    CHECK(buf[0] == 1000 && halHostBtBridgeFrames() == 256);   // copied, not yet acknowledged: local still plays
    halHostBtFeed("STREAM_ACK 1");
    halHostRenderAudio(256, buf);
    CHECK(buf[0] == 0 && buf[511] == 0 && halHostBtBridgeFrames() == 512);
    hal::btSetAudioRoute(true, true);
    halHostRenderAudio(64, buf);
    CHECK(buf[0] == 1000);
    // 48 kHz: the host stops routing like the Tab5
    hal::audioStop();
    CHECK(hal::audioStart(48000, 64, &Tone::render, nullptr));
    halHostBtAdvance(20);
    hal::btStatus(st);
    CHECK(!strcmp(st.message, "Bluetooth wymaga 44,1 kHz"));
    const uint64_t frames = halHostBtBridgeFrames();
    halHostRenderAudio(64, buf);
    CHECK(halHostBtBridgeFrames() == frames && buf[0] == 1000);
    hal::audioStop();
    halHostBtFeed("REMOTE PAUSE");
    CHECK(hal::btPopRemote() == BtRemote::Pause && hal::btPopRemote() == BtRemote::None);
    // truncation of a long sent line and pop without a buffer
    while (halHostBtTakeSent(b, sizeof b)) {}
    hal::btSelect("AA:BB:CC:DD:EE:FF");
    halHostBtAdvance(20);
    char small[8];
    std::vector<std::string> rest;
    for (int n; (n = halHostBtTakeSent(small, sizeof small)) > 0;) rest.push_back(small);
    bool trunc = false;
    for (const auto& s : rest) trunc = trunc || s == "SELECTM";
    CHECK(trunc);
    // disable: STOP? (not streaming now), SLEEP, STANDBY
    while (halHostBtTakeSent(b, sizeof b)) {}
    hal::btEnable(false);
    got.clear();
    while (halHostBtTakeSent(b, sizeof b)) got.push_back(b);
    CHECK(!got.empty() && got.back() == "SLEEP");
    halHostBtFeed("STANDBY");
    hal::btStatus(st);
    CHECK(!st.enabled && !st.awake && !strcmp(st.message, "Bluetooth wyłączony"));
    halHostBtReset();
    hal::btStatus(st);
    CHECK(st.revision == 0 && !st.device_count && halHostBtNow() == 0);
}

int main()
{
    testAssembler();
    testParse();
    testFuzz();
    testTimeline();
    testRecovery();
    testQueue();
    testBuffer();
    testHost();
    if (fails) std::printf("test_bt_protocol: %d of %d checks FAILED\n", fails, checks);
    else std::printf("test_bt_protocol: all %d checks passed\n", checks);
    return fails ? 1 : 0;
}
