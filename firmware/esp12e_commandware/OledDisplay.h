#pragma once

#include "ScriptRuntime.h"
#include "OledFont.h"
#include <array>

namespace cw {
// One 128x64 framebuffer; no sample history allocation. Wire is supplied by the
// sketch so native tests exercise the same controller commands and rendering.
class OledDisplay {
    struct Chart {
        bool used = false;
        int x = 0, y = 0, w = 0, h = 0, previous = -1;
        double min = 0, max = 1023;
    };
    std::array<Chart, 4> charts;
    uint8_t dirty = 0;
    std::array<uint16_t, 8> lines = {{1,1,1,1,1,1,1,1}};
    int page = -1, column = -1;
    int nextPage = 0;
    uint16_t transferLine = 1;
    uint32_t powerReadyAt = 0;
    bool displayOn = false, faulted = false;
    int lastPacketStatus = 0;

    bool packet(uint8_t control, const uint8_t *data, size_t count, std::string &error) {
        const int status = lastPacketStatus = send ? send(address, control, data, count) : -1;
        if (status == 0) return true;
        ready = false; faulted = true; dirty = 0; page = -1;
        const char *reason = status == 1 ? "transmit buffer full" :
            status == 2 ? "address not acknowledged" : status == 3 ? "data not acknowledged" :
            status == 4 ? "bus busy" : status == -1 ? "transport unavailable" : "bus error";
        const char *hex = "0123456789abcdef";
        error = "oled: I2C communication failed (code " + std::to_string(status) + ": " + reason +
            ", address 0x" + std::string(1, hex[address >> 4]) + std::string(1, hex[address & 15]) +
            "); check wiring/address and run oled init";
        return false;
    }
    static bool numeric(const Value &value, double &result) {
        if (value.kind == Value::NUMBER) result = value.number;
        else if (value.kind == Value::TEXT && !value.text.empty()) {
            char *end; result = strtod(value.text.c_str(), &end);
            if (*end || end == value.text.c_str()) return false;
        } else return false;
        return std::isfinite(result);
    }
    bool initializeOnce(bool sh, int newHeight, int newAddress, uint16_t line, uint32_t now, std::string &error) {
        lastPacketStatus = 0;
        if (begin && !begin(newAddress, error)) return false;
        ready = false;
        address = (uint8_t)newAddress; height = newHeight; sh1106 = sh;
        uint8_t commands[] = {0xae, 0xd5, 0x80, 0xa8, (uint8_t)(height-1),
            0xd3, 0x00, 0x40, 0xa1, 0xc8, 0xda, (uint8_t)(height == 64 ? 0x12 : 0x02),
            0x81, 0x7f, 0xd9, (uint8_t)(sh ? 0x22 : 0xf1), 0xdb, (uint8_t)(sh ? 0x35 : 0x40),
            0xa4, 0xa6};
        if (!packet(0x00, commands, sizeof(commands), error)) return false;
        // SSD1306 page addressing and charge pump; SH1106 has its own DC/DC.
        uint8_t ssd[] = {0x20, 0x02, 0x8d, 0x14, 0x2e};
        uint8_t shc[] = {0xad, 0x8b};
        if (!packet(0x00, sh ? shc : ssd, sh ? sizeof(shc) : sizeof(ssd), error)) return false;
        reserved = true; ready = true; faulted = false; displayOn = false; powerReadyAt = now + 100;
        buffer.fill(0); charts = {}; page = -1; column = -1;
        dirty = (uint8_t)((1 << (height/8))-1); lines.fill(line);
        return true;
    }
    bool initialize(bool sh, int newHeight, int newAddress, uint16_t line, uint32_t now, std::string &error) {
        if (initializeOnce(sh, newHeight, newAddress, line, now, error)) return true;
        if (lastPacketStatus != 2) return false;
        // A display may miss the first address acknowledgement. Retry the
        // entire initialization once; drawing and GPIO commands are not replayed.
        delay(25);
        error.clear();
        return initializeOnce(sh, newHeight, newAddress, line, now + 25, error);
    }
    void pixel(int x, int y, bool on, uint16_t line) {
        if (x < 0 || x >= 128 || y < 0 || y >= height) return;
        int index = (y/8)*128+x; uint8_t bit = (uint8_t)(1 << (y%8));
        uint8_t value = on ? buffer[index] | bit : buffer[index] & ~bit;
        if (value != buffer[index]) { buffer[index] = value; dirty |= (1 << (y/8)); lines[y/8] = line; }
    }
    void clearRect(int x, int y, int w, int h, uint16_t line) {
        for (int row = y; row < y+h; ++row)
            for (int col = x; col < x+w; ++col) pixel(col, row, false, line);
    }
public:
    bool ready = false, reserved = false, sh1106 = false;
    int height = 64;
    uint8_t address = 0x3c;
    std::array<uint8_t, 1024> buffer = {};
    std::function<bool(int, std::string &)> begin;
    // Wire status: zero means success; nonzero retains the hardware error code.
    std::function<int(uint8_t, uint8_t, const uint8_t *, size_t)> send;

    bool busy() const { return ready && (dirty || page >= 0 || !displayOn); }
    // At most one <=32-byte I2C transaction per call. Redirtying a page during
    // transfer queues another pass, so interleaved chart/text updates survive.
    bool service(uint32_t now, std::string &error) {
        if (!ready) return true;
        if (page < 0) {
            for (int i = 0; i < height/8; ++i) {
                int p = (nextPage+i) % (height/8);
                if (!(dirty & (1 << p))) continue;
                page = p; nextPage = (p+1) % (height/8); dirty &= ~(1 << p); column = -1; transferLine = lines[p]; break;
            }
        }
        if (page < 0) {
            if (!displayOn && (int32_t)(now-powerReadyAt) >= 0) {
                uint8_t on = 0xaf;
                if (!packet(0x00, &on, 1, error)) { error = located(error, transferLine); return false; }
                displayOn = true;
            }
            return true;
        }
        if (column < 0) {
            uint8_t commands[] = {(uint8_t)(0xb0+page), (uint8_t)(sh1106 ? 0x02 : 0x00), 0x10};
            if (!packet(0x00, commands, sizeof(commands), error)) { error = located(error, transferLine); return false; }
            column = 0;
        } else {
            size_t count = std::min(31, 128-column);
            if (!packet(0x40, buffer.data()+page*128+column, count, error)) { error = located(error, transferLine); return false; }
            column += (int)count; if (column == 128) page = -1;
        }
        return true;
    }

    bool command(const std::vector<Value> &args, const Event &event, uint16_t line, uint32_t now, std::string &error) {
        auto fail = [&](const std::string &message) { error = "oled: " + message; return false; };
        if (args.empty()) return fail("use init, print, chart, or clear");
        std::string action = lower(args[0].str());
        std::vector<Value> positional;
        std::map<std::string, Value> options;
        std::string allowed = action == "init" ? " --address --width --height " :
            action == "print" ? " --x --y --value " : action == "chart" ? " --x --y --width --height --min --max --value " : " ";
        for (size_t i = 1; i < args.size(); ++i) {
            std::string key = args[i].str();
            if (key.compare(0, 2, "--") == 0) {
                if (allowed.find(" " + key + " ") == std::string::npos || i+1 == args.size() || options.count(key))
                    return fail("invalid, duplicate, or missing option '" + key + "'");
                options[key] = args[++i];
            } else positional.push_back(args[i]);
        }
        auto integer = [&](const std::string &key, int fallback, int &result) {
            result = fallback;
            if (!options.count(key)) return true;
            double v;
            if (!numeric(options[key], v) || v != std::trunc(v) || v < 0 || v > 128) return false;
            result = (int)v; return true;
        };
        if (action == "init") {
            if (positional.size() > 1) return fail("init takes an optional SSD1306 or SH1106 controller");
            std::string controller = positional.empty() ? "ssd1306" : lower(positional[0].str());
            if (controller != "ssd1306" && controller != "sh1106") return fail("controller must be SSD1306 or SH1106");
            int w, h, a;
            if (!integer("--width", 128, w) || w != 128 || !integer("--height", 64, h) || (h != 32 && h != 64))
                return fail("supported sizes are 128x32 and 128x64");
            if (!integer("--address", 0x3c, a) || (a != 0x3c && a != 0x3d)) return fail("address must be 0x3c or 0x3d");
            return initialize(controller == "sh1106", h, a, line, now, error);
        }
        if (action == "clear") {
            if (!positional.empty()) return fail("clear takes no arguments");
            if (faulted) return fail("display transfer failed; run oled init");
            if (!ready && !initialize(sh1106, height, address, line, now, error)) return false;
            buffer.fill(0); charts = {}; dirty = (uint8_t)((1 << (height/8))-1); lines.fill(line);
            return true;
        }
        if (action != "print" && action != "chart") return fail("use init, print, chart, or clear");
        int x, y;
        if (!integer("--x", 0, x) || !integer("--y", 0, y)) return fail("x and y must be nonnegative integers");
        if (positional.size() >= 2) {
            double px, py;
            if (options.count("--x") || options.count("--y") || !numeric(positional[0], px) || !numeric(positional[1], py) ||
                px != std::trunc(px) || py != std::trunc(py) || px < 0 || px >= 128 || py < 0 || py >= height)
                return fail("use print [X Y] [TEXT] or chart [X Y] [VALUE]");
            x = (int)px; y = (int)py; positional.erase(positional.begin(), positional.begin()+2);
        }
        if (x >= 128 || y >= height) return fail("coordinates are outside the display");
        if (positional.size() > 1 || (!positional.empty() && options.count("--value"))) return fail("provide one value");
        Value value;
        if (!positional.empty()) value = positional[0];
        else if (options.count("--value")) value = options["--value"];
        else if (event.hasValue) value = event.value;
        else return fail(action + ": value is required through pipe or explicit parameter");
        int w = 0, h = 0; double low = 0, high = 1023, reading = 0;
        Chart *chart = nullptr;
        if (action == "chart") {
            if (!numeric(value, reading)) return fail("chart value must be finite and numeric");
            if (options.count("--min") && !numeric(options["--min"], low)) return fail("chart min must be finite and numeric");
            if (options.count("--max") && !numeric(options["--max"], high)) return fail("chart max must be finite and numeric");
            if (!(low < high) || !std::isfinite(high-low)) return fail("chart min must be less than max");
            if (!integer("--width", 128-x, w) || !integer("--height", height-y, h) || w < 2 || h < 2 || x+w > 128 || y+h > height)
                return fail("chart rectangle must fit the display and be at least 2x2");
            for (auto &c : charts) if (c.used) {
                if (c.x == x && c.y == y && c.w == w && c.h == h) chart = &c;
                else if (x < c.x+c.w && c.x < x+w && y < c.y+c.h && c.y < y+h) return fail("chart areas must not overlap; use oled clear to reset");
            }
            if (!chart) for (auto &c : charts) if (!c.used) { chart = &c; break; }
            if (!chart) return fail("chart area limit is 4; use oled clear to reset");
        }
        if (faulted) return fail("display transfer failed; run oled init");
        if (!ready && !initialize(sh1106, height, address, line, now, error)) return false;
        if (action == "print") {
            int origin = x;
            for (unsigned char ch : value.str()) {
                if (ch == '\n') { x = origin; y += 8; continue; }
                if (x+6 > 128) { x = origin; y += 8; }
                if (y+8 > height) break;
                if (ch < 32 || ch > 126) ch = '?';
                for (int col = 0; col < 6; ++col) {
                    uint8_t bits = col < 5 ? oledFontByte((ch-32)*5+col) : 0;
                    for (int row = 0; row < 8; ++row) pixel(x+col, y+row, bits & (1 << row), line);
                }
                x += 6;
            }
        } else {
            if (!chart->used || chart->min != low || chart->max != high) {
                clearRect(x, y, w, h, line);
                *chart = {true, x, y, w, h, -1, low, high};
            }
            reading = std::max(low, std::min(high, reading));
            int current = y+h-1-(int)std::floor((reading-low)/(high-low)*(h-1)+0.5);
            for (int row = y; row < y+h; ++row) {
                for (int col = x; col < x+w-1; ++col) pixel(col, row, getPixel(col+1, row), line);
                pixel(x+w-1, row, false, line);
            }
            int previous = chart->previous < 0 ? current : chart->previous;
            for (int row = std::min(previous, current); row <= std::max(previous, current); ++row) pixel(x+w-1, row, true, line);
            chart->previous = current;
        }
        return true;
    }
    bool getPixel(int x, int y) const {
        return x >= 0 && x < 128 && y >= 0 && y < height && (buffer[(y/8)*128+x] & (1 << (y%8)));
    }
};
} // namespace cw
