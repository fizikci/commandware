"""Pixel-level virtual counterpart of the firmware OLED framebuffer."""
from dataclasses import dataclass, field
import math

from .oled_font import FONT


@dataclass
class OledDisplay:
    controller: str = "ssd1306"
    width: int = 128
    height: int = 64
    address: int = 0x3c
    ready: bool = False
    reserved: bool = False
    buffer: bytearray = field(default_factory=lambda: bytearray(1024))
    charts: dict = field(default_factory=dict)
    text_history: list = field(default_factory=list)
    chart_history: list = field(default_factory=list)

    def pixel(self, x, y, on=None):
        if not (0 <= x < 128 and 0 <= y < self.height):
            return False
        index, bit = (y // 8) * 128 + x, 1 << (y % 8)
        if on is not None:
            self.buffer[index] = self.buffer[index] | bit if on else self.buffer[index] & (255 ^ bit)
        return bool(self.buffer[index] & bit)

    def _clear_rect(self, x, y, width, height):
        for row in range(y, y + height):
            for col in range(x, x + width):
                self.pixel(col, row, False)

    @staticmethod
    def _numeric(value):
        try:
            result = float(value)
        except (ValueError, TypeError, OverflowError):
            raise ValueError("oled: value must be finite and numeric") from None
        if not math.isfinite(result):
            raise ValueError("oled: value must be finite and numeric")
        return result

    def command(self, args, event_value=None, has_value=False, hardware=None):
        if not args:
            raise ValueError("oled: use init, print, chart, or clear")
        action = str(args[0]).lower()
        allowed = {
            "init": {"--address", "--width", "--height"},
            "print": {"--x", "--y", "--value"},
            "chart": {"--x", "--y", "--width", "--height", "--min", "--max", "--value"},
            "clear": set(),
        }
        if action not in allowed:
            raise ValueError("oled: use init, print, chart, or clear")
        positional, options = [], {}
        i = 1
        while i < len(args):
            key = str(args[i])
            if key.startswith("--"):
                if key not in allowed[action] or i + 1 == len(args) or key in options:
                    raise ValueError(f"oled: invalid, duplicate, or missing option '{key}'")
                options[key] = args[i + 1]
                i += 2
            else:
                positional.append(args[i])
                i += 1

        def integer(key, default):
            value = self._numeric(options.get(key, default))
            if value != int(value) or not 0 <= value <= 128:
                raise ValueError(f"oled: {key} must be a nonnegative integer in range")
            return int(value)

        def initialize(controller, height, address):
            if hardware:
                if any(pin in hardware.pin_owners for pin in (4, 5)):
                    raise ValueError("oled: I2C pins are owned by a job")
                if hardware.lcd_ready and hardware.lcd_address == address:
                    raise ValueError("oled: address is already used by LCD")
            self.controller, self.height, self.address = controller, height, address
            self.ready = self.reserved = True
            self.buffer[:] = bytes(1024)
            self.charts.clear()

        if action == "init":
            if len(positional) > 1:
                raise ValueError("oled: init takes an optional SSD1306 or SH1106 controller")
            controller = str(positional[0]).lower() if positional else "ssd1306"
            if controller not in {"ssd1306", "sh1106"}:
                raise ValueError("oled: controller must be SSD1306 or SH1106")
            width, height, address = integer("--width", 128), integer("--height", 64), integer("--address", 0x3c)
            if width != 128 or height not in (32, 64):
                raise ValueError("oled: supported sizes are 128x32 and 128x64")
            if address not in (0x3c, 0x3d):
                raise ValueError("oled: address must be 0x3c or 0x3d")
            initialize(controller, height, address)
            return
        if action == "clear":
            if positional:
                raise ValueError("oled: clear takes no arguments")
            if not self.ready:
                initialize(self.controller, self.height, self.address)
            self.buffer[:] = bytes(1024)
            self.charts.clear()
            return
        x, y = integer("--x", 0), integer("--y", 0)
        if len(positional) >= 2:
            if "--x" in options or "--y" in options:
                raise ValueError("oled: use positional coordinates or --x/--y")
            px, py = self._numeric(positional[0]), self._numeric(positional[1])
            if px != int(px) or py != int(py) or not 0 <= px < 128 or not 0 <= py < self.height:
                raise ValueError("oled: coordinates are outside the display")
            x, y = int(px), int(py)
            positional = positional[2:]
        if x >= 128 or y >= self.height:
            raise ValueError("oled: coordinates are outside the display")
        if len(positional) > 1 or positional and "--value" in options:
            raise ValueError("oled: provide one value")
        if positional:
            value = positional[0]
        elif "--value" in options:
            value = options["--value"]
        elif has_value:
            value = event_value
        else:
            raise ValueError(f"oled: {action}: value is required through pipe or explicit parameter")
        if action == "chart":
            reading = self._numeric(value)
            low, high = self._numeric(options.get("--min", 0)), self._numeric(options.get("--max", 1023))
            if low >= high or not math.isfinite(high - low):
                raise ValueError("oled: chart min must be less than max")
            width, height = integer("--width", 128 - x), integer("--height", self.height - y)
            if width < 2 or height < 2 or x + width > 128 or y + height > self.height:
                raise ValueError("oled: chart rectangle must fit the display and be at least 2x2")
            key = (x, y, width, height)
            for cx, cy, cw, ch in self.charts:
                if key != (cx, cy, cw, ch) and x < cx + cw and cx < x + width and y < cy + ch and cy < y + height:
                    raise ValueError("oled: chart areas must not overlap; use oled clear to reset")
            if key not in self.charts and len(self.charts) >= 4:
                raise ValueError("oled: chart area limit is 4; use oled clear to reset")
        if not self.ready:
            initialize(self.controller, self.height, self.address)
        if action == "print":
            text = format(value, ".12g") if isinstance(value, (int, float)) else str(value)
            self.text_history.append((x, y, text))
            origin = x
            for char in text.encode("utf-8"):
                if char == 10:
                    x, y = origin, y + 8
                    continue
                if x + 6 > 128:
                    x, y = origin, y + 8
                if y + 8 > self.height:
                    break
                if not 32 <= char <= 126:
                    char = ord("?")
                for col in range(6):
                    bits = FONT[(char - 32) * 5 + col] if col < 5 else 0
                    for row in range(8):
                        self.pixel(x + col, y + row, bool(bits & (1 << row)))
                x += 6
        else:
            old = self.charts.get(key)
            if old is None or old[:2] != (low, high):
                self._clear_rect(*key)
                previous = None
            else:
                previous = old[2]
            clipped = min(high, max(low, reading))
            current = y + height - 1 - math.floor((clipped - low) / (high - low) * (height - 1) + 0.5)
            for row in range(y, y + height):
                for col in range(x, x + width - 1):
                    self.pixel(col, row, self.pixel(col + 1, row))
                self.pixel(x + width - 1, row, False)
            for row in range(min(current, previous if previous is not None else current), max(current, previous if previous is not None else current) + 1):
                self.pixel(x + width - 1, row, True)
            self.charts[key] = (low, high, current)
            self.chart_history.append((key, reading))
