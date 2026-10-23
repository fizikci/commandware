// Separate ESP8266 maintenance sketch. Formatting requires a fresh serial
// confirmation after every boot. Never include this in normal Commandware.
#include <Arduino.h>
#include <LittleFS.h>
#include <flash_hal.h>
#include <cstring>

static bool finished = false;
static char confirmation[32];
static size_t confirmationLength = 0;
static bool overflow = false;

static void formatFilesystem()
{
    finished = true;
    const uint32_t detected = ESP.getFlashChipRealSize();
    const uint32_t configured = ESP.getFlashChipSize();
    const uint32_t offset = FS_PHYS_ADDR, size = FS_PHYS_SIZE;
    if (!detected || configured > detected || !size || offset > detected || size > detected - offset) {
        Serial.println(F("REFUSED: flash layout is empty or exceeds detected flash. Check the FQBN and flash-size options."));
        return;
    }
    LittleFSConfig config;
    config.setAutoFormat(false);
    if (!LittleFS.setConfig(config)) { Serial.println(F("FAILED: filesystem configuration")); return; }
    LittleFS.end();
    if (!LittleFS.format()) { Serial.println(F("FAILED: explicit LittleFS format")); return; }
    if (!LittleFS.begin()) { Serial.println(F("FAILED: formatted filesystem did not mount")); return; }
    bool directoryReady = LittleFS.mkdir("/scripts");
    auto directory = LittleFS.open("/scripts", "r");
    directoryReady = directoryReady && directory && directory.isDirectory();
    directory.close();
    FSInfo info;
    bool infoReady = LittleFS.info(info);
    LittleFS.end();
    if (!directoryReady || !infoReady) { Serial.println(F("FAILED: filesystem verification; check flash layout and hardware")); return; }
    Serial.print(F("PROVISIONED: LittleFS ready; total bytes: ")); Serial.println(info.totalBytes);
    Serial.println(F("Close the monitor and restore Commandware using the SAME FQBN/layout and wipe=none."));
    Serial.println(F("This sketch will not format again during this boot."));
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("Commandware LittleFS provisioning utility (ESP8266)"));
    Serial.print(F("Flash configured bytes: ")); Serial.println(ESP.getFlashChipSize());
    Serial.print(F("Flash detected bytes: ")); Serial.println(ESP.getFlashChipRealSize());
    Serial.print(F("LittleFS partition offset bytes: ")); Serial.println((unsigned long)FS_PHYS_ADDR);
    Serial.print(F("LittleFS partition size bytes: ")); Serial.println((unsigned long)FS_PHYS_SIZE);
    Serial.println(F("WARNING: formatting erases ALL files in the selected filesystem partition."));
    Serial.println(F("Check this layout against your Commandware build before continuing."));
    Serial.println(F("To erase and initialize LittleFS, send exactly: FORMAT LITTLEFS"));
}

void loop()
{
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (finished) continue;
        if (c == '\r' || c == '\n') {
            if (overflow || confirmationLength) {
                confirmation[confirmationLength] = '\0';
                if (!overflow && confirmationLength == sizeof("FORMAT LITTLEFS")-1 &&
                    strcmp(confirmation, "FORMAT LITTLEFS") == 0) formatFilesystem();
                else Serial.println(F("No changes made. Send exactly FORMAT LITTLEFS to confirm erasure."));
            }
            confirmationLength = 0; overflow = false;
        } else if (confirmationLength < sizeof(confirmation)-1) confirmation[confirmationLength++] = c;
        else overflow = true;
    }
    delay(1);
}
