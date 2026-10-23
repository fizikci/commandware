// Execute the actual maintenance sketch with a fake serial port and filesystem.
#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <LittleFS.h>
#define F(x) x
struct {
    std::string input;
    std::ostringstream output;
    size_t position=0;
    void begin(int) {}
    bool available() { return position<input.size(); }
    int read() { return input[position++]; }
    template<typename T> void print(T value) { output<<value; }
    template<typename T> void println(T value) { output<<value<<'\n'; }
} Serial;
struct {
    uint32_t configured=1024*1024, detected=1024*1024;
    uint32_t getFlashChipRealSize() { return detected; }
    uint32_t getFlashChipSize() { return configured; }
} ESP;
static uint32_t partitionOffset=0xEB000, partitionSize=64*1024;
#define FS_PHYS_ADDR partitionOffset
#define FS_PHYS_SIZE partitionSize
void delay(unsigned long) {}
#include "provision-under-test.inc"
static void fresh() {
    LittleFS=FakeFS();finished=false;confirmationLength=0;overflow=false;
    ESP.configured=ESP.detected=1024*1024;partitionOffset=0xEB000;partitionSize=64*1024;
    Serial.input.clear();Serial.position=0;Serial.output.str("");Serial.output.clear();
    LittleFS.files["/scripts/keep.cw"]="precious";
}
static void send(const std::string &input) { Serial.input+=input;loop(); }
int main() {
    fresh();setup();loop();
    assert(LittleFS.formatCalls==0 && LittleFS.files.count("/scripts/keep.cw"));
    send("yes\r\nformat littlefs\nFORMAT LITTLEFS extra\n"+std::string(64,'x')+"FORMAT LITTLEFS\n");
    assert(LittleFS.formatCalls==0 && LittleFS.files.count("/scripts/keep.cw"));
    send(std::string("FORMAT LITTLEFS\0extra\n",22));
    assert(LittleFS.formatCalls==0);
    send("FORMAT LITTLEFS");assert(LittleFS.formatCalls==0);
    send("\r\n");
    assert(LittleFS.formatCalls==1 && LittleFS.files.empty() && LittleFS.directory);
    assert(!LittleFS.autoFormat);
    assert(Serial.output.str().find("PROVISIONED")!=std::string::npos);
    send("FORMAT LITTLEFS\n");assert(LittleFS.formatCalls==1);
    for(int scenario=0;scenario<4;++scenario) {
        fresh();
        if(scenario==0)partitionSize=0;
        if(scenario==1)ESP.configured=4*1024*1024;
        if(scenario==2)partitionOffset=ESP.detected;
        if(scenario==3)ESP.detected=0;
        send("FORMAT LITTLEFS\n");
        assert(LittleFS.formatCalls==0 && LittleFS.files.count("/scripts/keep.cw"));
        assert(Serial.output.str().find("REFUSED")!=std::string::npos);
    }
    fresh();LittleFS.formatFails=true;send("FORMAT LITTLEFS\n");
    assert(Serial.output.str().find("FAILED: explicit LittleFS format")!=std::string::npos);
    assert(LittleFS.files.count("/scripts/keep.cw"));
    fresh();LittleFS.mountFails=true;send("FORMAT LITTLEFS\n");
    assert(Serial.output.str().find("FAILED: formatted filesystem did not mount")!=std::string::npos);
    fresh();LittleFS.mkdirFails=true;send("FORMAT LITTLEFS\n");
    assert(Serial.output.str().find("FAILED: filesystem verification")!=std::string::npos);
    std::cout<<"Explicit provisioning checks passed\n";
}
