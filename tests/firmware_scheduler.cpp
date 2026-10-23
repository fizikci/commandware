// Native tests execute the actual sketch scheduler with a fake clock and GPIO.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <map>
#include <new>
#include <sstream>
#include <string>
#include <vector>
using std::max;
using std::isfinite;
#define F(x) x
#define INPUT 0
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define A0 17

class String {
    std::string value;
public:
    String(const char *s = "") : value(s) {}
    String(std::string s) : value(s) {}
    String(int n) : value(std::to_string(n)) {}
    unsigned int length() const { return (unsigned int)value.size(); }
    char operator[](size_t i) const { return value[i]; }
    const char *c_str() const { return value.c_str(); }
    int indexOf(char c, int start = 0) const { auto p = value.find(c, start); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const String &s) const { auto p = value.find(s.value); return p == std::string::npos ? -1 : (int)p; }
    String substring(int start) const { return value.substr(std::min((size_t)start, value.size())); }
    String substring(int start, int end) const { return value.substr(start, end - start); }
    void remove(int start, int count) { value.erase(start, count); }
    void toLowerCase() { for (char &c : value) c = (char)std::tolower((unsigned char)c); }
    bool equalsIgnoreCase(String s) const { String copy = *this; copy.toLowerCase(); s.toLowerCase(); return copy == s; }
    bool startsWith(const String &s) const { return value.compare(0, s.length(), s.value) == 0; }
    bool endsWith(const String &s) const { return value.size() >= s.length() && value.compare(value.size() - s.length(), s.length(), s.value) == 0; }
    long toInt() const { return std::strtol(value.c_str(), nullptr, 10); }
    bool operator==(const String &s) const { return value == s.value; }
    bool operator!=(const String &s) const { return !(*this == s); }
    String operator+(const String &s) const { return value + s.value; }
    String &operator+=(char c) { value += c; return *this; }
    friend std::ostream &operator<<(std::ostream &out, const String &s) { return out << s.value; }
};
struct Print {
    std::ostringstream output;
    std::string input;
    size_t position = 0;
    bool available() { return position < input.size(); }
    char read() { return input[position++]; }
    virtual size_t write(uint8_t value) { output << (char)value; return 1; }
    template<class T> void print(T value) { std::ostringstream text; text << value; for (char c : text.str()) write((uint8_t)c); }
    template<class T> void println(T value) { print(value); write('\n'); }
} Serial;
static Print *commandOutput = &Serial;
static uint64_t fakeUs;
static std::map<int, int> gpio;
static std::vector<std::pair<int, int>> writes;
unsigned long millis() { return (unsigned long)(fakeUs / 1000); }
unsigned long micros() { return (unsigned long)fakeUs; }
void pinMode(int, int) {}
void digitalWrite(int pin, int value) { gpio[pin] = value; writes.emplace_back(pin, value); }
int digitalRead(int pin) { return gpio[pin]; }
static int adcValue = 512;
int analogRead(int) { return adcValue; }
static std::vector<unsigned long> delays;
void delay(unsigned long ms) { delays.push_back(ms); }
void delayMicroseconds(unsigned int) {}
struct FakeWire {
    int error = 0;
    int sda = -1, scl = -1, address = 0, clock = 0;
    bool writeFails = false;
    std::vector<int> responses;
    size_t responseIndex = 0;
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> transaction;
    void begin(int a, int b) { sda = a; scl = b; }
    void setClock(int frequency) { clock = frequency; }
    void beginTransmission(int a) { address = a; transaction.clear(); }
    size_t write(uint8_t value) { if (writeFails) return 0; bytes.push_back(value); transaction.push_back(value); return 1; }
    int endTransmission() { packets.push_back(transaction); return responseIndex < responses.size() ? responses[responseIndex++] : error; }
} Wire;
struct { int getFreeHeap() { return 10000; } } ESP;
struct { int status() { return 3; } String localIP() { return "127.0.0.1"; } } WiFi;
static void handleCommand(const String &line, uint16_t sourceLine = 1);
static bool scriptRequest(const String &, const String *, String &);
void serviceWebServer() {}
void yield() {}
#include "scheduler-under-test.inc"

enum { HTTP_GET, HTTP_POST };
static const char WEB_PAGE[] = "";
struct FakeWebServer {
    std::map<std::pair<std::string, int>, std::function<void()>> handlers;
    int status = 0;
    std::string body, cacheControl;
    void on(const char *path, int method, std::function<void()> handler) { handlers[{path, method}] = handler; }
    void onNotFound(std::function<void()>) {}
    std::map<std::string, std::string> arguments;
    String arg(const char *name) { auto found=arguments.find(name); return found==arguments.end() ? String() : String(found->second); }
    bool hasArg(const char *name) { return arguments.count(name); }
    void send(int code, const char *, const String &text) { status = code; body = text.c_str(); }
    void send_P(int code, const char *type, const char *text) { send(code, type, text); }
    void sendHeader(const char *name, const char *value) { if (std::string(name) == "Cache-Control") cacheControl = value; }
} webServer;
#include "web-under-test.inc"

void fresh(uint64_t timeUs = 0) {
    oledDisplay = cw::OledDisplay();
    configureScriptRuntime();
    scriptRuntime.reset(); scriptRuntime.functions.clear();
    scriptPinOwners.clear(); scriptOutputPins.clear();
    nextJobId = 1;
    fakeUs = timeUs;
    gpio.clear(); writes.clear(); delays.clear();
    Serial.output.str(""); Serial.output.clear();
    Serial.input.clear(); Serial.position = 0;
    inputBuffer = ""; inputBraceDepth = 0;
    inputQuoted = inputEscaped = false;
    inputQuoteChar = 0; inputComment = false;
    inputAfterCR = false; inputLine = 1;
    lcdReady = false; lcdAddress = 0x27; Wire = FakeWire(); adcValue = 512;
}
int activeJobs() { int count = 0; for (const auto &task : scriptRuntime.tasks) count += !task->scheduled; return count; }
int queuedJobs() { int count = 0; for (const auto &task : scriptRuntime.tasks) count += task->scheduled; return count; }
int totalJobs() { return activeJobs() + queuedJobs(); }
void at(uint64_t us) { fakeUs = us; scriptRuntime.tick((uint32_t)millis(), (uint32_t)micros()); }
void rejected(const char *command) {
    fresh(); handleCommand(command);
    assert(totalJobs() == 0);
    assert(writes.empty());
    assert(Serial.output.str().find("ERROR:") != std::string::npos);
}

int main(int argc, char **argv) {
    fresh(); configureWebServer();
    assert(scriptStorage.begin());
    handleCommand("status");
    assert(Serial.output.str().find("LittleFS ready; used bytes:")!=std::string::npos);
    LittleFS.mountFails=true;assert(!scriptStorage.begin());
    Serial.output.str("");handleCommand("status");
    assert(Serial.output.str().find("mount failed")!=std::string::npos);
    assert(Serial.output.str().find("Filesystem was not formatted")!=std::string::npos);
    assert(LittleFS.formatCalls==0);
    LittleFS.mountFails=false;assert(scriptStorage.begin());fresh();
    auto scriptApi = webServer.handlers.at({"/api/command", HTTP_POST});
    webServer.arguments = {{"command", "script save demo"}, {"result", "script"}, {"source", "  print \"$literal é雪\"\n\n"}};
    scriptApi();
    assert(webServer.status == 200 && webServer.body.find("\"ok\":true") != std::string::npos);
    assert(LittleFS.files.at("/scripts/demo.cw") == "  print \"$literal é雪\"\n\n");
    webServer.arguments = {{"command", "script load demo"}, {"result", "script"}};
    scriptApi();
    assert(webServer.status == 200 && webServer.body == "{\"ok\":true,\"value\":\"  print \\\"$literal é雪\\\"\\u000a\\u000a\"}");
    webServer.arguments["command"] = "script list";scriptApi();
    assert(webServer.body == "{\"ok\":true,\"value\":[\"demo\"]}");
    webServer.arguments["command"] = "script delete demo";scriptApi();
    assert(webServer.status == 200 && !LittleFS.exists("/scripts/demo.cw"));
    scriptApi();assert(webServer.status == 400);
    webServer.arguments["command"] = "script list; print 123";scriptApi();
    assert(webServer.status == 400);
    webServer.arguments = {{"command", "script save full"}, {"result", "script"}, {"source", std::string(4096, 'x')}};
    scriptApi();assert(webServer.status == 200);
    webServer.arguments["command"] = "script load full";webServer.arguments.erase("source");
    scriptApi();assert(webServer.status == 200 && webServer.body.size() > 4096);
    webServer.arguments = {{"command", "script save full"}, {"result", "script"}, {"source", std::string(4097, 'x')}};
    scriptApi();assert(webServer.status == 400 && LittleFS.files.at("/scripts/full.cw").size() == 4096);
    webServer.arguments.clear();LittleFS.files.clear();fresh();
    auto pollJobs = webServer.handlers.at({"/api/jobs", HTTP_GET});
    for (int i = 0; i < 3; ++i) {
        pollJobs();
        assert(webServer.status == 200 && webServer.body.empty());
        assert(webServer.cacheControl == "no-store");
        assert(nextJobId == 1 && totalJobs() == 0 && commandOutput == &Serial);
    }
    handleCommand("every 100 | print");
    handleCommand("print $missing");
    auto idBeforePolling = nextJobId;
    auto jobsBeforePolling = totalJobs();
    for (int i = 0; i < 3; ++i) {
        pollJobs();
        assert(webServer.body.find("RUNNING  every\n") != std::string::npos);
        assert(webServer.body.find("FAILED  script:") != std::string::npos);
        assert(webServer.body.find("RUNNING  script") == std::string::npos);
        assert(nextJobId == idBeforePolling && totalJobs() == jobsBeforePolling);
        assert(commandOutput == &Serial);
    }
    fresh(); handleCommand("write 12 1");
    assert(gpio[12] == HIGH && scriptPinOwners.empty());
    handleCommand("reset"); assert(gpio[12] == HIGH);
    handleCommand("number 512 --out raw | write 12 $raw > 700 | print $raw | print");
    assert(gpio[12] == LOW && Serial.output.str().find("512\n512\n") != std::string::npos);
    handleCommand("map 0 10 (-100) 100 2.5 | print");
    assert(Serial.output.str().find("-50\n") != std::string::npos);
    fresh(); handleCommand("func Pair text { split \":\" $text }; Pair --text \"A:B\" --out a,b | print \"$a/$b\"");
    assert(Serial.output.str().find("A/B\n") != std::string::npos);
    handleCommand("number 9 | do { number 1; number 2 } | print");
    assert(Serial.output.str().find("2\n") != std::string::npos);
    handleCommand("number 9 | if 0 {} else { number 3 } | print");
    assert(Serial.output.str().find("3\n") != std::string::npos);
    handleCommand("split \":\" \"a:b\" | each { print } | len $value | print");
    assert(Serial.output.str().find("a\nb\n2\n") != std::string::npos);
    handleCommand("max 5 min 10 3 | print");
    assert(Serial.output.str().find("5\n") != std::string::npos);
    std::cout << "PASS unified command routing, values, direct functions, branches, blocks and iteration\n";

    fresh(); handleCommand("clock 1000 --dur 2 | write 15");
    at(0); assert(writes.empty());
    at(500); assert(gpio[15] == HIGH);
    at(1000); assert(gpio[15] == LOW);
    at(1500); assert(gpio[15] == HIGH);
    at(2000); assert(gpio[15] == HIGH && !totalJobs() && scriptPinOwners.empty());
    fresh(); handleCommand("every 10 | write 12 1"); at(0);
    assert(gpio[12] == HIGH && totalJobs() == 1);
    handleCommand("write 12 0"); assert(gpio[12] == HIGH && !scriptRuntime.failures.empty());
    handleCommand("reset"); assert(gpio[12] == HIGH && !totalJobs());
    fresh(); handleCommand("set n 1; schedule { 5 { print $n } }; set n 2");
    assert(queuedJobs() == 1); at(5000);
    assert(Serial.output.str().find("2\n") != std::string::npos);
    fresh(); handleCommand("func F p { schedule { 5 { print $p } } }; F 7"); at(5000);
    assert(Serial.output.str().find("7\n") != std::string::npos);
    fresh(); handleCommand("clock 100 --dur 25 | print"); at(5000); at(16000); at(20000); at(25000);
    assert(Serial.output.str().find("1\n0\n") != std::string::npos);
    fresh(); handleCommand("pulse 10 | write 12"); assert(gpio[12] == HIGH); at(10000);
    assert(gpio[12] == LOW && !totalJobs());
    fresh(); handleCommand("every 5 | pulse 10 | write 12"); at(0); at(5000); at(10000);
    assert(gpio[12] == HIGH); handleCommand("reset"); assert(gpio[12] == HIGH);
    fresh(); gpio[12] = HIGH; handleCommand("every 5 | read 12 | debounce 10 | print");
    at(0); at(5000); at(10000); assert(Serial.output.str().find("1\n") != std::string::npos);
    handleCommand("reset");
    fresh(); handleCommand("clock 100 --dur 21 | divide 2 | print");
    at(5000); at(10000); at(15000); at(20000); at(21000);
    assert(Serial.output.str().find("0\n1\n") != std::string::npos);
    std::cout << "PASS source phases, skipped ticks, ownership, preserved GPIO, schedule captures and signal timers\n";

    const uint64_t rollover = 4294967294ULL * 1000;
    fresh(rollover);
    handleCommand("schedule { 1 { write 12 1 } 3 { write 12 0 } }");
    at(rollover + 1000); assert(gpio[12] == HIGH);
    at(rollover + 3000); assert(gpio[12] == LOW && !totalJobs());
    fresh(rollover); handleCommand("clock 1000 --dur 2 | write 15");
    at(rollover + 500); assert(gpio[15] == HIGH);
    at(rollover + 1000); assert(gpio[15] == LOW);
    at(rollover + 1500); assert(gpio[15] == HIGH);
    at(rollover + 2000); assert(gpio[15] == HIGH && !totalJobs());
    std::cout << "PASS millisecond/microsecond rollover scheduling and clock completion\n";

    fresh(); handleCommand("number 5 | shiftout 12 13 14 --bits 8 | print");
    assert(gpio[14] == HIGH && Serial.output.str().find("5\n") != std::string::npos);
    gpio[12] = HIGH; handleCommand("shiftin 12 13 14 --bits 8 | print");
    assert(Serial.output.str().find("255\n") != std::string::npos);
    for(const char *source : {"write 12 1 100", "clock 10 12", "read D1", "read 1", "read 12.5", "var n 0", "call F", "each", "do", "func F", "write 12 1; max 5 -1", "write 12 1; if 0 { shiftin 1 }"}) rejected(source);
    fresh(); handleCommand("set n 0; set n $n + 1 | print");
    assert(scriptRuntime.globals->vars.at("n").number == 1);
    handleCommand("set n 1 / 0"); assert(scriptRuntime.globals->vars.at("n").number == 1);
    fresh(); handleCommand("write 12 1; sleep 10; print $missing"); at(10000);
    assert(gpio[12] == HIGH && scriptPinOwners.empty() && !scriptRuntime.failures.empty());
    handleCommand("jobs"); assert(Serial.output.str().find("FAILED") != std::string::npos);
    fresh(); handleCommand("reset; write 12 1"); assert(writes.empty());
    fresh(); handleCommand("kill 1; write 12 1"); assert(writes.empty());
    fresh(); Serial.input="set n 1\r\nprint $n\r\n"; loop();
    assert(Serial.output.str().find("1\n") != std::string::npos);
    std::cout << "PASS shift-register frames, early validation, failure retention, self cancellation and serial input\n";

    fresh(); handleCommand("lcd print \"Hello | World!\"");
    assert(lcdReady && Wire.sda == 4 && Wire.scl == 5 && Wire.address == 0x27);
    std::string displayed;
    for(size_t i=0;i+5<Wire.bytes.size();i+=3) if(Wire.bytes[i]&1) {
        displayed += char((Wire.bytes[i]&0xf0)|(Wire.bytes[i+3]>>4)); i+=3;
    }
    assert(displayed == "Hello | World!                  ");
    handleCommand("write 4 1"); assert(writes.empty());
    fresh(); handleCommand("lcd init 0x3f"); assert(lcdReady && lcdAddress==0x3f);
    handleCommand("lcd clear");
    fresh(); Wire.error=2; handleCommand("lcd print \"Hello\""); assert(!lcdReady);
    std::cout << "PASS LCD rendering, initialization, I2C reservations and errors\n";

    // Retain detailed controller/render/transport tests from the original suite.
    for (const char *controller : {"ssd1306", "sh1106"}) {
        fresh(); handleCommand((std::string("oled init ")+controller).c_str());
        assert(oledDisplay.ready && oledDisplay.reserved && Wire.sda == 4 && Wire.scl == 5);
        assert(Wire.clock == 100000);
        bool sh = std::string(controller) == "sh1106";
        assert(oledDisplay.sh1106 == sh && Wire.packets.size() == 2);
        assert(Wire.packets[1][1] == (sh ? 0xad : 0x20));
        assert(Wire.packets[1][2] == (sh ? 0x8b : 0x02));
        int iterations = 0;
        while (oledDisplay.busy()) {
            std::string error; size_t before = Wire.packets.size();
            assert(oledDisplay.service(100, error) && error.empty());
            assert(Wire.packets.size()-before <= 1);
            assert(++iterations < 100);
        }
        assert(Wire.packets[2] == std::vector<uint8_t>({0x00,0xb0,(uint8_t)(sh ? 2 : 0),0x10}));
        size_t dataBytes = 0;
        for (const auto &packet : Wire.packets) {
            assert(packet.size() <= 32);
            if (packet[0] == 0x40) dataBytes += packet.size()-1;
        }
        assert(dataBytes == 1024 && Wire.packets.back() == std::vector<uint8_t>({0x00,0xaf}));
    }
    fresh(); handleCommand("oled init SH1106 --height 32 --address 0x3d");
    assert(oledDisplay.height == 32 && oledDisplay.address == 0x3d);
    fresh(); handleCommand("oled print 2 2 \"hello\"");
    for (int col = 0; col < 5; ++col) for (int row = 0; row < 8; ++row)
        assert(oledDisplay.getPixel(2+col,2+row) == bool(oledFontByte(('h'-32)*5+col) & (1 << row)));
    auto positional = oledDisplay.buffer;
    fresh(); handleCommand("oled print \"hello\" --x 2 --y 2"); assert(oledDisplay.buffer == positional);
    fresh(); handleCommand("oled print \"hello\"");
    assert(oledDisplay.buffer[0] == oledFontByte(('h'-32)*5));
    handleCommand("oled clear");
    assert(std::all_of(oledDisplay.buffer.begin(), oledDisplay.buffer.end(), [](uint8_t v) { return v == 0; }));
    fresh(); handleCommand("adc A0 --out raw | oled print 2 2 | print $raw");
    assert(Serial.output.str().find("512\n") != std::string::npos && scriptRuntime.failures.empty());
    handleCommand("adc A0 | oled print \"override\" --x 8 --y 16");
    assert(oledDisplay.getPixel(9,17) == bool(oledFontByte(('o'-32)*5+1) & 2));

    fresh(); handleCommand("oled chart 0 --min 0 --max 10 --width 4 --height 4");
    assert(oledDisplay.getPixel(3,3));
    handleCommand("oled chart 10 --min 0 --max 10 --width 4 --height 4");
    assert(oledDisplay.getPixel(2,3));
    for (int row = 0; row < 4; ++row) assert(oledDisplay.getPixel(3,row));
    handleCommand("oled chart 100 --min 0 --max 10 --width 4 --height 4");
    assert(oledDisplay.getPixel(3,0) && !oledDisplay.getPixel(3,3));
    handleCommand("oled chart -10 --min 0 --max 10 --x 10 --y 10 --width 4 --height 4");
    assert(oledDisplay.getPixel(13,13));
    handleCommand("oled chart 1 --x 1 --width 8 --height 8");
    assert(Serial.output.str().find("chart areas must not overlap") != std::string::npos);
    handleCommand("oled clear");
    handleCommand("do { set temp 23; set humidity 48; every 100 | oled chart $temp --min 0 --max 50 --height 32 | oled chart $humidity --min 0 --max 100 --y 32 --height 32 }");
    at(0); assert(oledDisplay.getPixel(127,17) && oledDisplay.getPixel(127,48));
    at(100000); assert(oledDisplay.getPixel(126,17) && oledDisplay.getPixel(126,48));
    auto retained = oledDisplay.buffer;
    handleCommand("reset"); assert(oledDisplay.buffer == retained && oledDisplay.reserved);
    size_t writeCountBefore = writes.size(); handleCommand("write 4 1");
    assert(writes.size() == writeCountBefore);
    handleCommand("do { write 5 1 }"); assert(scriptRuntime.tasks.empty());
    handleCommand("pins"); assert(Serial.output.str().find("OLED I2C") != std::string::npos);

    fresh(); handleCommand("adc A0 --watch | oled print"); at(0);
    assert(activeJobs() == 1 && oledDisplay.ready);
    adcValue = 7; at(100000); assert(scriptRuntime.failures.empty());
    fresh(); handleCommand("clock 1 | oled print 2 2 \"Hello world\""); at(0); at(500000);
    assert(activeJobs() == 1 && oledDisplay.ready);
    fresh(); handleCommand("schedule { 10 { oled print 2 2 \"scheduled\" } }");
    at(10000); assert(oledDisplay.ready);
    fresh(); handleCommand("clock 1 | write 4"); at(500000); handleCommand("oled init");
    assert(!oledDisplay.ready && activeJobs() == 1);
    fresh(); handleCommand("every 10 | write 4 1"); at(0); handleCommand("oled init");
    assert(!oledDisplay.ready);

    for (const char *invalid : {"oled init unknown", "oled init --address 0", "oled init --height 48",
        "oled print", "oled chart", "oled print \"hi\" --x -1", "oled chart bad",
        "oled chart 1 --min 5 --max 5", "oled chart 1 --width 1", "oled clear extra",
        "oled print \"hi\" --x", "oled print \"hi\" --x 0 --x 1"}) {
        fresh(); handleCommand((std::string("do {\n # comment\n ")+invalid+"\n}").c_str());
        assert(!oledDisplay.ready && Wire.packets.empty());
        assert(Serial.output.str().find("at line 3") != std::string::npos);
    }
    fresh(); Wire.error = 2; handleCommand("do {\n oled init\n}");
    assert(!oledDisplay.ready && !oledDisplay.reserved && Serial.output.str().find("at line 2") != std::string::npos);
    assert(Serial.output.str().find("code 2: address not acknowledged, address 0x3c") != std::string::npos);
    assert(Wire.packets.size() == 2 && delays == std::vector<unsigned long>({25}));
    fresh(); Wire.error = 3; handleCommand("oled init --address 0x3d");
    assert(Serial.output.str().find("code 3: data not acknowledged, address 0x3d") != std::string::npos);
    assert(Wire.packets.size() == 1 && delays.empty());
    fresh(); Wire.error = 4; handleCommand("oled init");
    assert(Serial.output.str().find("code 4: bus busy") != std::string::npos);
    assert(Wire.packets.size() == 1 && delays.empty());
    fresh(); Wire.writeFails = true; handleCommand("oled init");
    assert(Serial.output.str().find("code 1: transmit buffer full") != std::string::npos);
    fresh();
    const char *twoLines = "do {\n oled init ssd1306\n oled clear\n oled print \"Commandware\"\n oled print 0 8 \"Ready to run\"\n}";
    Wire.error = 2; handleCommand(twoLines); at(0);
    assert(scriptRuntime.failures.size() == 1 && !oledDisplay.ready);
    Wire.error = 0; handleCommand(twoLines); at(0);
    assert(oledDisplay.ready && scriptRuntime.tasks.empty() && scriptRuntime.failures.size() == 1);
    assert(oledDisplay.buffer[0] == oledFontByte(('C'-32)*5));
    assert(oledDisplay.buffer[128] == oledFontByte(('R'-32)*5));
    while(oledDisplay.busy()) { std::string error; assert(oledDisplay.service(100, error)); }
    assert(scriptRuntime.failures[0].message.find("code 2: address not acknowledged") != std::string::npos);
    fresh(); Wire.responses = {2, 0, 0}; handleCommand("oled init ssd1306");
    assert(oledDisplay.ready && Wire.packets.size() == 3 && delays == std::vector<unsigned long>({25}));
    assert(scriptRuntime.failures.empty() && Serial.output.str().find("ERROR") == std::string::npos);
    fresh(); Wire.responses = {2, 0, 0}; handleCommand(twoLines); at(0);
    assert(oledDisplay.ready && scriptRuntime.failures.empty() && scriptRuntime.tasks.empty());
    assert(oledDisplay.buffer[0] == oledFontByte(('C'-32)*5));
    assert(oledDisplay.buffer[128] == oledFontByte(('R'-32)*5));
    assert(Wire.packets.size() == 3 && delays == std::vector<unsigned long>({25}));
    // An acknowledgement failure in the second packet restarts the whole
    // sequence and preserves the requested controller, address, and dimensions.
    fresh(); Wire.responses = {0, 2, 0, 0}; handleCommand("oled init sh1106 --address 0x3d --height 32");
    assert(oledDisplay.ready && oledDisplay.sh1106 && oledDisplay.height == 32 && oledDisplay.address == 0x3d);
    assert(Wire.packets.size() == 4 && Wire.packets[0] == Wire.packets[2] && Wire.packets[1] == Wire.packets[3]);
    assert(delays == std::vector<unsigned long>({25}) && Serial.output.str().find("ERROR") == std::string::npos);
    fresh(); Wire.responses = {2, 2, 0}; handleCommand(twoLines); at(0);
    assert(!oledDisplay.ready && Wire.packets.size() == 2 && scriptRuntime.failures.size() == 1);
    assert(std::all_of(oledDisplay.buffer.begin(), oledDisplay.buffer.end(), [](uint8_t v) { return v == 0; }));
    assert(delays == std::vector<unsigned long>({25}));
    // Automatic initialization used by the first drawing command also recovers.
    fresh(); Wire.responses = {2, 0, 0}; handleCommand("oled print \"Hello\"");
    assert(oledDisplay.ready && scriptRuntime.failures.empty() && Wire.packets.size() == 3);
    fresh(); handleCommand("do {\n oled print \"Hello\"\n}");
    Wire.error = 2; std::string transferError;
    assert(!oledDisplay.service(100, transferError) && transferError.find("at line 2") != std::string::npos);
    assert(transferError.find("code 2: address not acknowledged") != std::string::npos);
    assert(!oledDisplay.ready && oledDisplay.reserved);
    handleCommand("oled print \"retry\""); assert(!oledDisplay.ready);
    Wire.error = 0; handleCommand("oled init"); assert(oledDisplay.ready);
    fresh(); handleCommand("oled init; lcd init; oled print \"OLED\"; lcd print \"LCD\"");
    assert(lcdReady && oledDisplay.ready && scriptRuntime.failures.empty());
    std::cout << "PASS OLED controllers/protocols, text/coordinates, charts, cooperative transfer, errors and pin ownership\n";


    fresh(); handleCommand("number 7 --out raw | oled rect 2 7 3 4 | print $raw | print");
    assert(scriptRuntime.failures.empty() && Serial.output.str().find("7\n7\n") != std::string::npos);
    for (int y = 0; y < 64; ++y) for (int x = 0; x < 128; ++x)
        assert(oledDisplay.getPixel(x,y) == (x >= 2 && x < 5 && y >= 7 && y < 11));
    handleCommand("oled rect 3 8 1 2 --value 0");
    assert(!oledDisplay.getPixel(3,8) && !oledDisplay.getPixel(3,9) && oledDisplay.getPixel(2,8));
    handleCommand("number 0 | oled rect 127 63 1 1"); assert(oledDisplay.getPixel(127,63));
    for (const char *invalid : {"oled rect", "oled rect 0 0 1", "oled rect 0 0 1 1 1",
        "oled rect 0 0 0 1", "oled rect 127 63 2 1", "oled rect 0 63 1 2",
        "oled rect (-1) 0 1 1", "oled rect 0.5 0 1 1", "oled rect 0 0 1 1 --value 2",
        "oled rect 0 0 1 1 --value bad", "oled rect 0 0 1 1 --value",
        "oled rect 0 0 1 1 --value 0 --value 1", "oled rect 0 0 1 1 --x 0"}) {
        fresh(); handleCommand(invalid);
        assert(!oledDisplay.ready && Wire.packets.empty() && Serial.output.str().find("ERROR:") != std::string::npos);
    }
    fresh(); handleCommand("oled init sh1106 --height 32; oled rect 127 31 1 1");
    assert(oledDisplay.getPixel(127,31));
    auto beforeInvalid = oledDisplay.buffer; handleCommand("oled rect 0 31 1 2");
    assert(oledDisplay.buffer == beforeInvalid);
    std::cout << "PASS OLED filled rectangles, erasure, event preservation and geometry validation\n";

    assert(argc > 1);
    std::ifstream file(argv[1]); assert(file);
    std::string pong((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    fresh(); handleCommand(pong.c_str());
    // Multiple cooperative steps may be needed to complete one frame.
    auto frame = [&](unsigned ms) { for (int i=0;i<10;++i) at((uint64_t)ms*1000); assert(scriptRuntime.failures.empty()); };
    frame(0); assert(activeJobs() == 1 && oledDisplay.ready);
    auto &vars = scriptRuntime.globals->vars;
    gpio[12] = 0; gpio[14] = 1;
    vars["leftY"] = cw::Value(28); vars["rightY"] = cw::Value(28);
    frame(50); assert(vars["leftY"].number == 26 && vars["rightY"].number == 30);
    vars["waiting"] = cw::Value(0); vars["ballX"] = cw::Value(6); vars["ballY"] = cw::Value(30);
    vars["velocityX"] = cw::Value(-2); vars["velocityY"] = cw::Value(1);
    frame(100); assert(vars["ballX"].number == 4 && vars["velocityX"].number == 2);
    vars["waiting"] = cw::Value(0); vars["ballX"] = cw::Value(1); vars["ballY"] = cw::Value(55);
    vars["velocityX"] = cw::Value(-2);
    frame(150); assert(vars["rightScore"].number == 1 && vars["waiting"].number == 16);
    for (unsigned ms=200;ms<10000;ms+=50) frame(ms);
    assert(activeJobs() == 1);
    std::cout << "PASS complete Pong example, independent buttons, paddle collision, scoring and sustained play\n";
}
