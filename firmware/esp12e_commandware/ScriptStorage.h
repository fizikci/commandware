#pragma once

#include <LittleFS.h>
#include "ScriptRuntime.h"

namespace cw {
// One file per script; no index or second copy of the device's saved-script list.
class ScriptStorage {
    bool mounted = false;
    const char *initializationError = "storage has not been initialized";
    bool initializationFailed(const char *reason) { initializationError = reason; return false; }
    static std::string path(const std::string &name) { return "/scripts/" + name + ".cw"; }
    bool ready(std::string &error) const {
        if (!mounted) error = std::string("script: LittleFS unavailable: ") + initializationError;
        return mounted;
    }
    static bool fail(std::string &error, const char *message) { error = message; return false; }
public:
    static bool validName(const std::string &name) {
        if (name.empty() || name.size() > 28 || name.front() == ' ' || name.back() == ' ') return false;
        for (unsigned char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-')) return false;
        return true;
    }
    const char *mountError() const { return initializationError; }
    bool begin() {
        mounted = false;
#if defined(ESP8266)
        LittleFSConfig config;
        config.setAutoFormat(false);
        if (!LittleFS.setConfig(config)) return initializationFailed("filesystem configuration failed");
        bool success = LittleFS.begin();
#elif defined(ESP32)
        bool success = LittleFS.begin(false);
#else
#error ScriptStorage requires ESP8266 or ESP32 LittleFS
#endif
        if (!success) return initializationFailed("mount failed; check flash layout, then explicitly provision or repair LittleFS");
        if (!LittleFS.exists("/scripts") && !LittleFS.mkdir("/scripts"))
            return initializationFailed("mounted, but /scripts creation failed; check free space and filesystem health");
        auto directory = LittleFS.open("/scripts", "r");
        if (!directory) return initializationFailed("mounted, but /scripts could not be opened");
        if (!directory.isDirectory()) return initializationFailed("mounted, but /scripts is not a directory");
        mounted = true;
        initializationError = nullptr;
        return true;
    }
    bool list(Value &value, std::string &error) {
        if (!ready(error)) return false;
        auto directory = LittleFS.open("/scripts", "r");
        if (!directory || !directory.isDirectory()) return fail(error, "script: cannot open directory");
        value = Value(); value.kind = Value::LIST;
#if defined(ESP8266)
        auto dir = LittleFS.openDir("/scripts");
        while (dir.next()) {
            if (!dir.isFile()) continue;
            std::string name = dir.fileName().c_str();
#else
        for (auto file = directory.openNextFile(); file; file = directory.openNextFile()) {
            if (file.isDirectory()) continue;
            std::string name = file.name();
#endif
            auto slash = name.find_last_of('/');
            if (slash != std::string::npos) name.erase(0, slash + 1);
            if (name.size() < 4 || name.substr(name.size()-3) != ".cw") continue;
            name.resize(name.size()-3);
            if (!validName(name)) continue;
            if (value.list.size() == 32) return fail(error, "script: script count exceeds limit of 32");
            value.list.emplace_back(name);
        }
        std::sort(value.list.begin(), value.list.end(), [](const Value &a, const Value &b) { return a.text < b.text; });
        return true;
    }
    bool load(const std::string &name, Value &value, std::string &error) {
        if (!ready(error)) return false;
        auto filename = path(name);
        if (!LittleFS.exists(filename.c_str())) return fail(error, "script: script not found");
        auto file = LittleFS.open(filename.c_str(), "r");
        if (!file || file.isDirectory()) return fail(error, "script: cannot open file");
        size_t size = file.size();
        if (size > 4096) return fail(error, "script: source exceeds 4096 bytes");
        std::string source(size, '\0');
        if (size && file.read(reinterpret_cast<uint8_t*>(&source[0]), size) != size)
            return fail(error, "script: file read failed");
        std::vector<size_t> offsets;
        if (!textOffsets(source, offsets, error, false)) return false;
        value = Value(std::move(source)); return true;
    }
    bool save(const std::string &name, const Value &value, std::string &error) {
        if (!ready(error)) return false;
        if (value.kind != Value::TEXT) return fail(error, "script save requires a string input");
        std::vector<size_t> offsets;
        if (!textOffsets(value.text, offsets, error, false)) return false;
        Value names;
        if (!list(names, error)) return false;
        auto filename = path(name);
        if (!LittleFS.exists(filename.c_str()) && names.list.size() >= 32)
            return fail(error, "script: script limit is 32");
        const char *pending = "/scripts/.pending";
        if (LittleFS.exists(pending) && !LittleFS.remove(pending))
            return fail(error, "script: cannot remove incomplete save");
#if defined(ESP8266)
        FSInfo info;
        if (!LittleFS.info(info)) return fail(error, "script: cannot query storage capacity");
        size_t total = info.totalBytes, used = info.usedBytes;
#else
        size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
#endif
        if (used > total || value.text.size() > total - used)
            return fail(error, "script: insufficient free storage for safe replacement");
        auto file = LittleFS.open(pending, "w");
        if (!file) return fail(error, "script: cannot create temporary file");
        size_t written = file.write(reinterpret_cast<const uint8_t*>(value.text.data()), value.text.size());
        file.flush(); file.close();
        // Verify before replacing: Arduino's flush/close APIs do not return errors.
        bool complete = written == value.text.size();
        auto verify = LittleFS.open(pending, "r");
        complete = complete && verify && verify.size() == value.text.size();
        uint8_t buffer[128];
        for (size_t pos = 0; complete && pos < value.text.size();) {
            size_t count = std::min(sizeof(buffer), value.text.size()-pos);
            complete = verify.read(buffer, count) == count &&
                std::equal(reinterpret_cast<const char*>(buffer), reinterpret_cast<const char*>(buffer)+count, value.text.data()+pos);
            pos += count;
        }
        verify.close();
        if (!complete) { LittleFS.remove(pending); return fail(error, "script: write verification failed; previous script preserved"); }
        // LittleFS rename replaces atomically. Never delete the previous file first.
        if (!LittleFS.rename(pending, filename.c_str())) {
            LittleFS.remove(pending);
            return fail(error, "script: replacement failed; previous script preserved");
        }
        return true;
    }
    bool remove(const std::string &name, std::string &error) {
        if (!ready(error)) return false;
        auto filename = path(name);
        if (!LittleFS.exists(filename.c_str())) return fail(error, "script: script not found");
        return LittleFS.remove(filename.c_str()) || fail(error, "script: delete failed");
    }
    bool command(const std::vector<Value> &args, Event &event, std::string &error) {
        if (args.empty() || args[0].kind != Value::TEXT) return fail(error, "script requires list, save, load, or delete");
        auto action = lower(args[0].text);
        if (action == "list" && args.size() == 1) {
            if (!list(event.value, error)) return false;
            event.hasValue = true; return true;
        }
        if (args.size() != 2 || (action != "save" && action != "load" && action != "delete"))
            return fail(error, "invalid script arguments");
        if (args[1].kind != Value::TEXT || !validName(args[1].text))
            return fail(error, "script: name must be 1-28 ASCII letters, digits, spaces, _ or -; no surrounding spaces");
        const auto &name = args[1].text;
        if (action == "save") {
            if (!event.hasValue) return fail(error, "script save requires a string input");
            return save(name, event.value, error);
        }
        if (action == "delete") return remove(name, error);
        if (!load(name, event.value, error)) return false;
        event.hasValue = true; return true;
    }
};
} // namespace cw
