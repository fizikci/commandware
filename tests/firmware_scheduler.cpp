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
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}
struct FakeWire {
    int error = 0;
    int sda = -1, scl = -1, address = 0;
    std::vector<uint8_t> bytes;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> transaction;
    void begin(int a, int b) { sda = a; scl = b; }
    void setClock(int) {}
    void beginTransmission(int a) { address = a; transaction.clear(); }
    size_t write(uint8_t value) { bytes.push_back(value); transaction.push_back(value); return 1; }
    int endTransmission() { packets.push_back(transaction); return error; }
} Wire;
struct { int getFreeHeap() { return 10000; } } ESP;
struct { int status() { return 3; } String localIP() { return "127.0.0.1"; } } WiFi;
struct Job;
static void handleCommand(const String &line, uint16_t sourceLine = 1);
void serviceWebServer() {}
void yield() {}
#include "scheduler-under-test.inc"

void fresh(uint64_t timeUs = 0) {
    oledDisplay = cw::OledDisplay();
    configureScriptRuntime();
    scriptRuntime.reset(); scriptRuntime.functions.clear();
    scriptPinOwners.clear(); scriptOutputPins.clear();
    for (auto &job : jobs) job = Job();
    for (auto &entry : scheduled) entry = ScheduledCommand();
    nextJobId = 1;
    fakeUs = timeUs;
    gpio.clear(); writes.clear();
    Serial.output.str(""); Serial.output.clear();
    pendingWritePulse = PendingWritePulse();
    Serial.input.clear(); Serial.position = 0;
    inputBuffer = ""; inputBraceDepth = 0;
    inputQuoted = inputEscaped = false;
    inputQuoteChar = 0; inputComment = false;
    inputAfterCR = false; inputLine = 1;
    lcdReady = false; lcdAddress = 0x27; Wire = FakeWire(); adcValue = 512;
}
int activeJobs() { int count = 0; for (const auto &job : jobs) count += job.active; return count; }
int queuedJobs() { int count = 0; for (const auto &entry : scheduled) count += entry.id != 0; return count; }
int totalJobs() { return activeJobs() + queuedJobs(); }
void at(uint64_t us) { fakeUs = us; serviceJobs(); scriptRuntime.tick((uint32_t)millis(), (uint32_t)micros()); }
void rejected(const char *command) {
    fresh(); handleCommand(command);
    assert(totalJobs() == 0);
    assert(writes.empty());
    assert(Serial.output.str().find("ERROR:") != std::string::npos);
}

int main(int argc, char **argv) {
    fresh(); handleCommand("lcd print \"Hello | World!\"");
    assert(lcdReady && Wire.sda == 4 && Wire.scl == 5 && Wire.address == 0x27);
    // Decode the real driver's RS data writes, including padding on both rows.
    std::string displayed;
    for (size_t i = 0; i + 5 < Wire.bytes.size(); i += 3)
        if (Wire.bytes[i] & 1) {
            displayed += char((Wire.bytes[i] & 0xf0) | (Wire.bytes[i + 3] >> 4));
            i += 3;
        }
    assert(displayed == "Hello | World!                  ");
    handleCommand("write 4 1"); assert(writes.empty());
    Wire.bytes.clear(); handleCommand("adc A0 | lcd print");
    assert(Serial.output.str().find("pipeline stage failure") == std::string::npos);
    assert(Wire.bytes.size() == 34 * 6);

    fresh(); handleCommand("lcd init 0x3f"); assert(lcdAddress == 0x3f && lcdReady);
    handleCommand("lcd clear");
    fresh(); Wire.error = 2; handleCommand("lcd print \"Hello\"");
    assert(!lcdReady && Serial.output.str().find("ERROR:") != std::string::npos);
    for (const char *invalid : {"lcd print", "lcd print \"oops", "lcd print \"ok\" extra", "lcd init 0", "lcd init 0x27 junk", "adc A0 --watch extra | lcd print"})
        rejected(invalid);

    fresh(); handleCommand("adc A0 --watch | lcd print");
    assert(activeJobs() == 1); at(0); assert(lcdReady);
    size_t written = Wire.bytes.size();
    at(100000); assert(Wire.bytes.size() == written); // Unchanged ADC suppressed.
    adcValue = 7; at(200000); assert(Wire.bytes.size() == written + 34 * 6);
    handleCommand("jobs"); assert(Serial.output.str().find("1 RUNNING  adc A0 --watch") != std::string::npos);
    handleCommand("kill 1"); adcValue = 8; at(300000); assert(!activeJobs());
    assert(Wire.bytes.size() == written + 34 * 6);
    fresh(); handleCommand("adc A0 --watch | print"); at(0); at(100000);
    adcValue = 1; at(200000); handleCommand("reset"); assert(!activeJobs());
    assert(Serial.output.str().find("512\n1\n") != std::string::npos);
    fresh(); handleCommand("clock 1 | write 4"); handleCommand("lcd init");
    assert(!lcdReady && activeJobs() == 1);
    fresh(); handleCommand("schedule { 0 { lcd print \"a|{b}\" } }");
    assert(queuedJobs() == 1); at(0); assert(lcdReady);
    fresh(); Serial.input = "lcd print \"{Hello}\"\n"; loop(); assert(lcdReady);

    fresh();
    Serial.input = "schedule {\n 1000 {clock 1000 --dur 500 | write 15}\n";
    loop(); assert(!activeJobs());
    Serial.input += " 1500 {clock 1400 --dur 1000 | write 15}\n}\n";
    loop(); assert(totalJobs() == 2);
    at(999000); assert(writes.empty());
    at(1000000); at(1000500); assert(gpio[15] == HIGH);
    at(1500000); assert(jobs[0].id == 2 && jobs[0].started);
    at(2500000); assert(!totalJobs() && gpio[15] == LOW);

    fresh();
    handleCommand("clock 1000 --dur 2 | write 15");
    at(0); at(500); assert(gpio[15] == HIGH);
    at(1000); assert(gpio[15] == LOW);
    at(1500); assert(gpio[15] == HIGH);
    at(2000); assert(gpio[15] == LOW && activeJobs() == 0);

    fresh(); handleCommand("clock 1000 15 --dur 2");
    at(0); at(500); assert(gpio[15] == HIGH); // Legacy pin shorthand.
    handleCommand("kill 1"); assert(gpio[15] == LOW && !activeJobs());

    fresh(); handleCommand("clock 1000 | write 15");
    at(0); at(500); at(2000000); assert(activeJobs() == 1);
    handleCommand("reset"); assert(gpio[15] == LOW && !activeJobs());

    fresh();
    handleCommand("SCHEDULE {\n 2 { CLOCK 1000 --dur 1 | WRITE 15 }\n 0 { clock 1000 --dur 2 | write 15 }\n 0 { clock 1000 --dur 1 | write 5 }\n}");
    assert(queuedJobs() == 3 && !activeJobs());
    at(0); at(500); assert(gpio[15] == HIGH && gpio[5] == HIGH);
    at(1000); assert(activeJobs() == 1 && gpio[5] == LOW);
    at(1500); assert(gpio[15] == HIGH);
    at(2000); assert(jobs[0].id == 1 && jobs[0].started && gpio[15] == LOW);
    at(2500); assert(gpio[15] == HIGH);
    at(3000); assert(!totalJobs() && gpio[15] == LOW);

    fresh(); handleCommand("schedule { 1000 { clock 1000 --dur 500 | write 15 } }");
    handleCommand("kill 1"); at(2000000); assert(writes.empty());

    fresh(); handleCommand("schedule { 2 { write 15 0 } 1 { write 15 1 } }");
    at(0); assert(writes.empty()); at(1000); assert(gpio[15] == HIGH);
    at(2000); assert(gpio[15] == LOW && !totalJobs());

    const uint64_t base = 4294967294ULL * 1000;
    fresh(base); handleCommand("schedule { 1 { clock 1000 --dur 2 | write 15 } }");
    at(base); assert(writes.empty()); at(base + 1000); at(base + 1500);
    assert(gpio[15] == HIGH);
    at(base + 3000); assert(gpio[15] == LOW && !totalJobs());

    for (const char *invalid : {
        "clock 1000 --dur 0 | write 15", "clock 1000 --dur -1 | write 15",
        "clock 1000 --dur 1.5 | write 15", "clock 1000 --dur | write 15",
        "clock 1000 --dur 2147483648 | write 15", "clock nan | write 15",
        "clock 1000junk | write 15", "clock 1000 --dur 2 --dur 3 | write 15",
        "clock 1000 |", "schedule {}", "schedule { 0 {} }",
        "schedule { -1 { clock 1000 | write 15 } }",
        "schedule { 0 { clock 1000 | write 15 } } trailing",
        "schedule { 0 { clock 1000 | write 15 }",
        "schedule { 0 { clock 1000 --dur 5 | write 15 } 4 { clock 1000 --dur 5 | write 15 } }",
        "schedule { 2147483647 { clock 1000 --dur 1 | write 15 } }",
        "schedule { 0 { read 4 | bogus } }"
    }) rejected(invalid);

    fresh(); handleCommand("clock 1000 | write 15");
    handleCommand("schedule { 1 { clock 1000 --dur 2 | write 15 } }");
    assert(totalJobs() == 1);

    fresh();
    for (int i = 0; i < 8; ++i) handleCommand("clock 1000 | print");
    handleCommand("schedule { 0 { clock 1000 --dur 1 | print } }");
    assert(totalJobs() == 8);

    fresh();
    for (int i = 0; i < 8; ++i) handleCommand("clock 1000 --dur 10 | print");
    handleCommand("schedule { 20 { clock 1000 --dur 1 | write 15 } }");
    assert(activeJobs() == 8 && queuedJobs() == 1);
    at(0); at(10000); assert(!activeJobs() && queuedJobs() == 1);
    at(20000); at(20500); assert(activeJobs() == 1 && gpio[15] == HIGH);
    at(21000); assert(!totalJobs() && gpio[15] == LOW);

    fresh();
    handleCommand("schedule { 100 { clock 1000 --dur 1 | write 15 } }");
    handleCommand("clock 1000 | write 15");
    assert(!activeJobs() && queuedJobs() == 1); // Direct clocks respect future notes.

    fresh();
    std::string sequence = "schedule {";
    for (int i = 0; i < 32; ++i)
        sequence += std::to_string(i) + " { clock 1000 --dur 1 | write 15 } ";
    sequence += "}";
    handleCommand(sequence);
    assert(queuedJobs() == 32 && !activeJobs());
    handleCommand("schedule { 100 { print 1 } }");
    assert(queuedJobs() == 32); // Queue overflow is atomic.
    handleCommand("jobs");
    assert(Serial.output.str().find("32 RUNNING  [queued]") != std::string::npos);
    handleCommand("kill 25");
    assert(queuedJobs() == 31);
    for (uint64_t us = 0; us <= 32000; us += 500) { at(us); assert(activeJobs() <= 1); }
    assert(!totalJobs() && gpio[15] == LOW);

    fresh();
    sequence = "schedule {";
    for (int i = 0; i < 9; ++i) sequence += "0 { clock 1000 --dur 1 | print } ";
    sequence += "}";
    rejected(sequence.c_str());
    handleCommand("schedule { 100 { clock 1000 --dur 1 | write 15 } }");
    handleCommand("reset");
    at(101000); assert(!totalJobs() && writes.empty());

    assert(argc == 2);
    std::ifstream tune(argv[1]);
    std::stringstream contents; contents << tune.rdbuf();
    assert(!contents.str().empty());
    fresh(); handleCommand(contents.str());
    assert(queuedJobs() == 25);
    for (uint64_t us = 0; us <= 12500000; us += 500) { at(us); assert(activeJobs() <= 1); }
    assert(!totalJobs() && gpio[15] == LOW);
    std::cout << "PASS native firmware scheduling, duration, cleanup, validation, capacity, rollover\n";

    fresh();
    handleCommand("var count 0");
    handleCommand("set count $count + 2 * 3");
    handleCommand("print $count");
    assert(scriptRuntime.globals->vars.at("count").number == 6);
    assert(Serial.output.str().find("6\n") != std::string::npos);
    handleCommand("set missing 1");
    assert(Serial.output.str().find("invalid variable") != std::string::npos);
    assert(scriptRuntime.tasks.empty());

    fresh(); adcValue = 512;
    handleCommand("do {\n var samples 0\n every 100 | adc A0 --out raw | map 0 1023 0 100 --value $raw --out percent | set samples $samples + 1 | print \"$samples:$raw:$percent\"\n }");
    assert(scriptRuntime.tasks.size() == 1);
    at(0); at(100000); adcValue = 1023; at(200000);
    assert(Serial.output.str().find("1:512:50\n2:512:50\n3:1023:100\n") != std::string::npos);
    assert(!scriptRuntime.globals->vars.count("samples"));
    handleCommand("reset"); assert(scriptRuntime.tasks.empty());

    fresh();
    handleCommand("func notes str {\n split \" \" --value $str | each --out note {\n split \":\" --value $note --out pitch,dur | tone 15 --hz notehz($pitch) --dur int($dur)\n }\n }");
    assert(scriptRuntime.functions.count("notes"));
    handleCommand("do {\n every 50 | toggle | write 12\n call notes \"C4:200 E4:300\"\n print \"melody finished\"\n }");
    assert(scriptRuntime.tasks.size() == 2);
    at(0); assert(gpio[12] == HIGH);
    at(2000); assert(gpio[15] == HIGH);
    at(50000); assert(gpio[12] == LOW);
    handleCommand("status"); assert(Serial.output.str().find("ESP8266 Commandware ready") != std::string::npos);
    at(199000); assert(Serial.output.str().find("melody finished") == std::string::npos);
    at(200000); // Second tone begins exactly after the first completes.
    assert(scriptRuntime.tasks[0]->tonePin == 15);
    assert(scriptRuntime.tasks[0]->wake == 500);
    at(499000); assert(Serial.output.str().find("melody finished") == std::string::npos);
    at(500000); assert(Serial.output.str().find("melody finished\n") != std::string::npos);
    assert(scriptRuntime.tasks.size() == 1 && gpio[15] == LOW);
    unsigned long blinkId = scriptRuntime.tasks[0]->id;
    handleCommand((std::string("kill ") + std::to_string(blinkId)).c_str());
    assert(scriptRuntime.tasks.empty() && gpio[12] == LOW && scriptPinOwners.empty());

    fresh();
    handleCommand("split \" \" --value \"A B C\" | each --out item | print $item | sleep 10");
    assert(Serial.output.str().find("A\n") != std::string::npos);
    assert(Serial.output.str().find("B\n") == std::string::npos);
    at(10000); assert(Serial.output.str().find("B\n") != std::string::npos);
    at(20000); assert(Serial.output.str().find("C\n") != std::string::npos);
    at(30000); assert(scriptRuntime.tasks.empty());

    fresh();
    handleCommand("do {\n var offset 10\n schedule { $offset { clock 1000 --dur 2 | write 15 } 20 { write 12 1 } }\n print \"scheduled\"\n }");
    assert(queuedJobs() == 2 && scriptRuntime.tasks.empty());
    at(10000); at(10500); assert(gpio[15] == HIGH);
    at(12000); assert(gpio[15] == LOW);
    at(20000); assert(gpio[12] == HIGH);

    fresh();
    Serial.input = "do {\n # ignored { brace and quote \"\n var count 0\n every 10 |\n set count $count + 1 | print 'Count: $count'\n}\n";
    loop(); assert(scriptRuntime.tasks.size() == 1);
    assert(Serial.output.str().find("Count: 1\n") != std::string::npos);
    handleCommand("reset");

    fresh();
    handleCommand("every 10 | tone 15 --hz 440 --dur 20"); at(0);
    handleCommand("clock 1000 | write 15"); assert(!activeJobs());
    size_t writeCount = writes.size(); handleCommand("write 15 1"); assert(writes.size() == writeCount);
    handleCommand("lcd init"); assert(lcdReady); // Disjoint pins still work.
    handleCommand("reset"); assert(scriptRuntime.tasks.empty() && scriptPinOwners.empty() && gpio[15] == LOW);
    fresh(); handleCommand("clock 1000 | write 15");
    handleCommand("tone 15 --hz 440 --dur 20");
    assert(scriptRuntime.tasks.empty() && activeJobs() == 1);
    fresh(); handleCommand("every 10 | write 4 1"); at(0);
    handleCommand("lcd init"); assert(!lcdReady);
    handleCommand("reset"); assert(gpio[4] == LOW);

    // Parsing/unknown commands fail before any statement runs. Runtime errors
    // stop the current job and release every pin it acquired.
    fresh(); handleCommand("do { write 12 1; bogus }"); assert(writes.empty());
    handleCommand("do { write 12 1; set absent 2 }");
    assert(scriptRuntime.tasks.empty() && gpio[12] == LOW && scriptPinOwners.empty());
    handleCommand("split \":\" --value \"C4:200:extra\" --out pitch,dur | print $pitch");
    assert(Serial.output.str().find("output count mismatch") != std::string::npos);
    handleCommand("do { var raw 0; adc A0 --out raw }");
    assert(Serial.output.str().find("duplicate output name") != std::string::npos);
    handleCommand("func recurse { call recurse }"); handleCommand("call recurse");
    assert(scriptRuntime.tasks.empty() && Serial.output.str().find("depth exceeds 8") != std::string::npos);

    fresh();
    handleCommand("func pair str { split \":\" --value $str }");
    handleCommand("call pair \"C4:200\" --out pitch,dur | print \"$pitch $dur\"");
    assert(Serial.output.str().find("C4 200\n") != std::string::npos);
    handleCommand("do { write 12 1; tone 15 --dur 200 }");
    assert(writes.empty()); // Argument shape errors are caught before execution.
    handleCommand("do { clock 1000 --dur 2 | write 15; print \"started\" }");
    assert(activeJobs() == 1 && scriptRuntime.tasks.empty());
    at(0); at(500); assert(gpio[15] == HIGH);
    at(2000); assert(!activeJobs() && gpio[15] == LOW);
    fresh(); handleCommand("do { adc A0 --watch | print; print \"watching\" }");
    at(0); assert(activeJobs() == 1 && Serial.output.str().find("watching\n512\n") != std::string::npos);

    fresh();
    Serial.input = "do { var pitch C#4; print $pitch }\n"; loop();
    assert(Serial.output.str().find("C#4\n") != std::string::npos);
    fresh();
    for (int i = 0; i < 8; ++i) handleCommand("sleep 100");
    assert(scriptRuntime.tasks.size() == 8);
    handleCommand("sleep 1"); assert(scriptRuntime.tasks.size() == 8);
    handleCommand("func available { print \"defined at capacity\" }");
    assert(scriptRuntime.functions.count("available"));
    handleCommand("reset"); assert(scriptRuntime.tasks.empty());

    fresh(base); handleCommand("do { sleep 3; print \"awake\" }");
    at(base + 2000); assert(Serial.output.str().find("awake") == std::string::npos);
    at(base + 3000); assert(Serial.output.str().find("awake\n") != std::string::npos);
    fresh(); handleCommand("every 10 | sleep 20 | print \"sample\""); at(0); at(1000000);
    assert(Serial.output.str().find("sample\n") != std::string::npos);
    assert(scriptRuntime.tasks[0]->work.empty()); // No queued catch-up burst.

    std::cout << "PASS scripts: records, scopes, functions, sequential notes, concurrency, serial, schedule, cancellation, rollover\n";

    fresh();
    handleCommand("var bit 0\nevery 500 | set bit !bit | write 5");
    assert(scriptRuntime.tasks.empty() && scriptRuntime.globals->vars.empty() && writes.empty());
    assert(Serial.output.str().find("ERROR: invalid expression: read variable values with $name") != std::string::npos);
    fresh(); handleCommand("var bit 0");
    handleCommand("every 500 | set bit !$bit | write 5 $bit");
    at(0); assert(gpio[5] == HIGH);
    at(500000); assert(gpio[5] == LOW);
    at(1000000); assert(gpio[5] == HIGH);
    handleCommand("reset"); assert(gpio[5] == LOW);
    fresh(); handleCommand("var bit !0 + 2");
    assert(scriptRuntime.globals->vars.at("bit").number == 3);
    handleCommand("set bit !!$bit"); assert(scriptRuntime.globals->vars.at("bit").number == 1);
    handleCommand("every 500 | print $missing"); at(0);
    assert(scriptRuntime.tasks.empty() && scriptRuntime.failures.size() == 1);
    handleCommand("jobs");
    assert(Serial.output.str().find("FAILED  every 500: unknown name 'missing'") != std::string::npos);
    handleCommand("var valid 1");
    assert(scriptRuntime.failures.size() == 1);
    handleCommand("reset"); assert(scriptRuntime.failures.empty());
    std::cout << "PASS boolean expressions, early syntax errors, retained background failures\n";

    for (const char *source : {
        "split \" \" \"A B\" | each | print",
        "split \" \" --value \"A B\" | each | print",
        "adc A0 | split \" \" \"A B\" | each | print",
        "adc A0 | split \" \" --value \"A B\" | each | print"
    }) {
        fresh(); handleCommand(source);
        assert(Serial.output.str().find("A\nB\n") != std::string::npos);
    }
    fresh(); handleCommand("adc A0 | split \"1\" | each | print");
    assert(Serial.output.str().find("5\n2\n") != std::string::npos);
    fresh(); handleCommand("split \":\" 0 | each | print");
    assert(Serial.output.str().find("0\n") != std::string::npos);
    fresh(); handleCommand("split \":\" \"\" | each | split \":\" | each | print");
    assert(scriptRuntime.failures.empty() && scriptRuntime.tasks.empty());
    assert(Serial.output.str().find("ERROR:") == std::string::npos);
    fresh(); handleCommand("do {\n print \"ready\"\n\n # comment\n split \" \"\n}");
    assert(Serial.output.str().find("split: value is required through pipe or explicit parameter --value at line 5\n") != std::string::npos);
    fresh(); handleCommand("\n\nunknown");
    assert(Serial.output.str().find("at line 3\n") != std::string::npos);
    fresh(); handleCommand("do {\n sleep 10\n print $missing\n}"); at(10000);
    assert(Serial.output.str().find("unknown name 'missing' at line 3\n") != std::string::npos);
    fresh(); handleCommand("do {\n every 10 |\n print $missing\n}"); at(0);
    handleCommand("jobs");
    assert(scriptRuntime.failures[0].message.find("at line 3") != std::string::npos);
    fresh(); handleCommand("func broken {\n sleep 10\n print $missing\n}");
    handleCommand("call broken"); at(10000);
    assert(Serial.output.str().find("unknown name 'missing' at line 3\n") != std::string::npos);
    fresh(); handleCommand("do {\n lcd init 0\n}");
    assert(Serial.output.str().find("at line 2\n") != std::string::npos);
    fresh(); handleCommand("do {\n sleep 10\n lcd init 0\n}"); at(10000);
    std::istringstream delayedErrors(Serial.output.str());
    std::string errorLine;
    while (std::getline(delayedErrors, errorLine))
        if (errorLine.find("ERROR:") == 0) assert(errorLine.find("at line 3") != std::string::npos);
    fresh(); handleCommand("read");
    assert(Serial.output.str().find("ERROR: read requires PIN at line 1\n") != std::string::npos);
    fresh(); handleCommand("do {\n clock 1000 | lcd print\n}");
    Wire.error = 1; at(0); at(500);
    assert(Serial.output.str().find("ERROR: LCD I2C communication failed; check wiring and address at line 2\n") != std::string::npos);
    fresh(); handleCommand("schedule {\n 10 {\n lcd init 0\n }\n}"); at(10000);
    assert(Serial.output.str().find("at line 3\n") != std::string::npos);
    fresh(); handleCommand("do {\n schedule {\n 10 {\n lcd init 0\n }\n }\n}"); at(10000);
    assert(Serial.output.str().find("at line 4\n") != std::string::npos);
    fresh(); Serial.input = "do {\r\n print \"ready\"\r\n split \" \"\r\n}\r\n"; loop();
    assert(Serial.output.str().find("--value at line 3\n") != std::string::npos);
    std::cout << "PASS positional/piped split, absent vs zero/empty input, source lines, delayed and legacy failures, serial CRLF\n";

    fresh(); handleCommand("var count 10; var count 0; print $count");
    assert(scriptRuntime.globals->vars.at("count").number == 0 && scriptRuntime.failures.empty());
    handleCommand("var count \"reset\"");
    assert(scriptRuntime.globals->vars.at("count").str() == "reset");
    handleCommand("do { var count 10; var count $count + 1; print $count }");
    assert(Serial.output.str().find("11\n") != std::string::npos);
    assert(scriptRuntime.globals->vars.at("count").str() == "reset");
    handleCommand("func reset_count count { var count 0; print $count }; call reset_count 10");
    assert(scriptRuntime.failures.empty());
    fresh();
    for (int i = 0; i < 32; ++i)
        handleCommand((std::string("var v") + std::to_string(i) + " 0").c_str());
    handleCommand("var v0 99");
    assert(scriptRuntime.globals->vars.size() == 32 && scriptRuntime.globals->vars.at("v0").number == 99);
    assert(scriptRuntime.failures.empty());
    handleCommand("var extra 0");
    assert(Serial.output.str().find("variable limit is 32") != std::string::npos);
    fresh(); handleCommand("adc A0 --out raw | var raw 0");
    assert(Serial.output.str().find("invalid variable declaration/assignment 'raw'") != std::string::npos);
    std::cout << "PASS var redeclaration, persisted values, local scopes, capacity and event-field conflicts\n";

    for (const char *controller : {"ssd1306", "sh1106"}) {
        fresh(); handleCommand((std::string("oled init ")+controller).c_str());
        assert(oledDisplay.ready && oledDisplay.reserved && Wire.sda == 4 && Wire.scl == 5);
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
    handleCommand("do { var temp 23; var humidity 48; every 100 | oled chart $temp --min 0 --max 50 --height 32 | oled chart $humidity --min 0 --max 100 --y 32 --height 32 }");
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
    fresh(); handleCommand("clock 1 | write 4"); handleCommand("oled init");
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
    fresh(); handleCommand("do {\n oled print \"Hello\"\n}");
    Wire.error = 2; std::string transferError;
    assert(!oledDisplay.service(100, transferError) && transferError.find("at line 2") != std::string::npos);
    assert(!oledDisplay.ready && oledDisplay.reserved);
    handleCommand("oled print \"retry\""); assert(!oledDisplay.ready);
    Wire.error = 0; handleCommand("oled init"); assert(oledDisplay.ready);
    fresh(); handleCommand("oled init; lcd init; oled print \"OLED\"; lcd print \"LCD\"");
    assert(lcdReady && oledDisplay.ready && scriptRuntime.failures.empty());
    std::cout << "PASS OLED controllers/protocols, text/coordinates, charts, cooperative transfer, errors and pin ownership\n";
}
