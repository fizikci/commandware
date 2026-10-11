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
    template<class T> void print(T value) { output << value; }
    template<class T> void println(T value) { output << value << '\n'; }
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
int analogRead(int) { return 512; }
struct { int getFreeHeap() { return 10000; } } ESP;
struct { int status() { return 3; } String localIP() { return "127.0.0.1"; } } WiFi;
struct Job;
static void handleCommand(const String &line);
void serviceWebServer() {}
void yield() {}
#include "scheduler-under-test.inc"

void fresh(uint64_t timeUs = 0) {
    for (auto &job : jobs) job = Job();
    for (auto &entry : scheduled) entry = ScheduledCommand();
    nextJobId = 1;
    fakeUs = timeUs;
    gpio.clear(); writes.clear();
    Serial.output.str(""); Serial.output.clear();
    pendingWritePulse = PendingWritePulse();
    Serial.input.clear(); Serial.position = 0;
    inputBuffer = ""; inputBraceDepth = 0;
}
int activeJobs() { int count = 0; for (const auto &job : jobs) count += job.active; return count; }
int queuedJobs() { int count = 0; for (const auto &entry : scheduled) count += entry.id != 0; return count; }
int totalJobs() { return activeJobs() + queuedJobs(); }
void at(uint64_t us) { fakeUs = us; serviceJobs(); }
void rejected(const char *command) {
    fresh(); handleCommand(command);
    assert(totalJobs() == 0);
    assert(writes.empty());
    assert(Serial.output.str().find("ERROR:") != std::string::npos);
}

int main(int argc, char **argv) {
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
}
