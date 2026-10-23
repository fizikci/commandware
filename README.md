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
Open prompt before discarding unsaved edits. Saved scripts live on the device in
LittleFS and are shared by all browsers. Both editor modes support these actions;
a connection to the device is required to save or open. The browser keeps only
the current editor buffer and its unsaved-change state. Previous browser saves
are no longer read or managed; no migration is performed.

The console uses the same storage commands:

```text
string 'print "Hello"' | script save demo
script list | print
script load demo | print
script load demo | script save backup
script delete backup
```

Loading retrieves source; use the editor's Run button to execute it. There is no
automatic boot script. See [script command semantics](docs/language-reference-draft.md#script)
for naming, limits, and the editor transport.

LittleFS must be explicitly provisioned once. Commandware **never formats on
mount failure**. Startup output and `status` report the configured and detected
flash sizes, filesystem partition offset/size, mount or directory setup errors,
and filesystem capacity when available. A mount failure alone cannot distinguish
an unformatted partition from corruption or a mismatched flash layout.

To provision an ESP8266, first copy any unsaved editor text somewhere safe and
check that the selected board/flash layout matches the existing firmware. The
utility defaults to the same `esp8266:esp8266:generic` board as the upload scripts
(the installed 3.1.2 core defaults to a 1 MB layout with a 64 KB filesystem).
Use `--fqbn` if you built Commandware with different board/flash options.
Formatting erases **all files in that filesystem partition**.

```bash
# Optional: compile the utility and Commandware without touching the device.
./provision-littlefs.sh --build-only

# Substitute your board's serial port. COM3 also works on Windows with Bash.
./provision-littlefs.sh /dev/cu.usbserial-0001
```

The script compiles both sketches with `wipe=none`, then asks you to type
`UPLOAD FORMATTER` before replacing the running application with the separate
maintenance sketch. It prints a serial-monitor command. Open that monitor at
115200 baud, verify the displayed layout, and send exactly `FORMAT LITTLEFS`
followed by Enter. Only that serial confirmation erases and initializes LittleFS.
There is no format on boot, invalid input, or mount failure. The utility refuses
empty filesystem layouts and layouts exceeding detected flash; it verifies that
the formatted filesystem mounts and `/scripts` can be created.

After `PROVISIONED`, close the serial monitor and run the exact restore command
printed by the script. This restores the already compiled Commandware firmware
with the **same FQBN and flash layout**. Reload the web editor and retry Save.
If provisioning fails, keep the serial diagnostics and check the layout before
retrying; formatting is not a repair for incorrect flash-size settings. The
standalone sketch is `tools/provision_littlefs/provision_littlefs.ino` if you use
the Arduino IDE instead. Its board/flash options must match Commandware too.

For subsequent updates, upload only firmware using the same flash layout and
`wipe=none` (Only Sketch, the current ESP8266 upload default). Saved scripts
survive reboots and these ordinary firmware updates. Full-chip erase, filesystem
image uploads, and partition changes can destroy saved scripts. Back up scripts
before those operations. Storage supports ESP8266 and ESP32 LittleFS; the full
application and this provisioning utility target ESP8266.

## Documentation

- [Language reference](docs/language-reference-draft.md): shared syntax, full
  command/value-function catalog, execution contracts, and initial limits.
- [Scripting language](docs/language.md): variables, functions, sequential
  iteration, periodic jobs, OLED text/charts, and working example programs.
- [Tutorial](docs/tutorial.md): setup, explained commands, pipelines, scheduling,
  and a complete Happy Birthday melody.
- [Implementation checklist](docs/language-implementation-checklist.md): completed
  software contracts and remaining physical-board verification.
- [Product requirements](docs/PRD.md): project goals, command model, and design.
