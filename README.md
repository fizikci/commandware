# Commandware

Commandware is a command shell for ESP8266 / ESP-12E boards. Read sensors,
control GPIO, build signal pipelines, and schedule commands over serial or a
local web interface. A Python simulator is included for experimenting without
hardware.

## Getting started

Install Arduino CLI and the ESP8266 board core. For Wi-Fi, copy
[`wifi_config.example.h`](firmware/esp12e_commandware/wifi_config.example.h) to
`wifi_config.h` in the same directory and fill in your credentials. Git ignores
the local configuration; without it, the serial console still works.

Build, upload, and monitor (replace `COM3` with your board's port):

```bash
arduino-cli compile --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
arduino-cli upload -p COM3 --fqbn esp8266:esp8266:generic firmware/esp12e_commandware
arduino-cli monitor -p COM3 --config baudrate=115200
```

The web interface fills the viewport with command samples (20%), an editor
(55%), and output plus running jobs (25%). Each pane scrolls independently;
drag the dividers or focus them and use the arrow keys to resize. Click a
sample to insert it at the cursor without replacing existing text, then use
Run or Ctrl/Cmd+Enter to execute it. Samples are grouped by task, with OLED
controller/address/size options, text, charts, GPIO, sensors, and scripts.
Filter by title, description, or command. Sample groups start collapsed; click
a group title to expand or collapse its samples.
Filtering expands matching groups, and clearing the filter restores the prior
expansion choices. The editor
loads CodeMirror from a pinned CDN URL for line numbers and syntax highlighting.
If the CDN is unavailable or takes more than six seconds, the basic text editor
remains usable, including Tab indentation and the Run shortcut.

Use New to start an unnamed script, Save to give it a name, and Open to choose
from saved scripts. Saving a named script updates its stored contents. New and
Open prompt before discarding unsaved edits. Scripts are stored in localStorage
for the current browser and device address and remain available after browser
restarts. Both editor modes support these actions.

## Documentation

- [Scripting language](docs/language.md): variables, functions, sequential
  iteration, periodic jobs, OLED text/charts, and working example programs.
- [Tutorial](docs/tutorial.md): setup, explained commands, pipelines, scheduling,
  and a complete Happy Birthday melody.
- [Product requirements](docs/PRD.md): project goals, command model, and design.
