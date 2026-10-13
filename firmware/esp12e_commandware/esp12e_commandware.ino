#include <Arduino.h>
#include <math.h>
#include <new>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>

// Credentials stay in a local, Git-ignored header. Fresh checkouts still
// compile and provide the serial console with Wi-Fi disabled.
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif

struct Stage;
struct Job;
struct ScheduledCommand;
struct ClockWindow;

static const char *WIFI_SSID = COMMANDWARE_WIFI_SSID;
static const char *WIFI_PASSWORD = COMMANDWARE_WIFI_PASSWORD;
static const unsigned long WIFI_RETRY_INTERVAL = 15000;

static ESP8266WebServer webServer(80);
static bool webServerStarted = false;
static unsigned long lastWifiAttempt = 0;
static Print *commandOutput = &Serial;

class StringPrint : public Print
{
public:
    size_t write(uint8_t value) override
    {
        buffer += (char)value;
        return 1;
    }

    const String &str() const
    {
        return buffer;
    }

private:
    String buffer;
};

static void handleCommand(const String &line, uint16_t sourceLine = 1);
static String trim(String value);

static const char WEB_PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="utf-8">
  <title>Commandware</title>
  <style>
    :root { color-scheme: dark; font: 16px/1.5 system-ui, sans-serif; background: #0b1120; color: #e5edf8; }
    * { box-sizing: border-box; }
    body { max-width: 32rem; min-height: 100vh; margin: 0 auto; padding: max(1.5rem, env(safe-area-inset-top)) 1rem max(2rem, env(safe-area-inset-bottom)); }
    h1 { margin: 0; font-size: 1.8rem; letter-spacing: -.04em; }
    h2 { margin: 0 0 1rem; font-size: 1.1rem; }
    header { margin-bottom: 1.5rem; }
    header p { margin: .2rem 0 0; color: #93a4bc; font-size: .9rem; }
    section { margin-top: 1rem; padding: 1rem; border: 1px solid #26344a; border-radius: 1rem; background: #111b2d; }
    label { display: block; margin: .9rem 0 .35rem; color: #aebed3; font-size: .82rem; font-weight: 650; }
    textarea, button { min-height: 3rem; border: 1px solid #34445d; border-radius: .7rem; font: inherit; }
    textarea { resize: vertical; min-height: 6rem; padding-top: .65rem; width: 100%; padding: 0 .8rem; background: #0b1424; color: #edf4ff; }
    #command-form { margin-top: .9rem; }
    .command-row { display: flex; align-items: stretch; gap: .55rem; }
    .command-entry { position: relative; min-width: 0; flex: 1; }
    #command { padding-right: 3rem; }
    #command-action { position: absolute; top: .25rem; right: .25rem; width: 2.5rem; min-height: 2.5rem; padding: 0; border: 0; background: transparent; color: #93a4bc; font-size: 1.25rem; }
    #command-action:active { background: #26344a; }
    .suggestions { position: absolute; z-index: 2; top: calc(100% + .35rem); right: 0; left: 0; max-height: 14rem; overflow-y: auto; padding: .3rem; border: 1px solid #34445d; border-radius: .7rem; background: #111b2d; box-shadow: 0 .7rem 1.5rem #0008; }
    .suggestions[hidden] { display: none; }
    .suggestions button { display: block; width: 100%; min-height: 2.7rem; padding: .45rem .6rem; border: 0; border-radius: .45rem; background: transparent; color: #dbeafe; text-align: left; font-size: .9rem; overflow-wrap: anywhere; }
    .suggestions button:active, .suggestions button:focus { background: #26344a; }
    button { padding: 0 .95rem; background: #2563eb; border-color: #3b82f6; color: white; font-weight: 700; cursor: pointer; touch-action: manipulation; }
    button:active { background: #1d4ed8; }
    button:disabled { opacity: .55; cursor: wait; }
    pre { min-height: 4rem; margin: .8rem 0 0; padding: .85rem; overflow-wrap: anywhere; white-space: pre-wrap; border: 1px solid #26344a; border-radius: .7rem; background: #080e19; color: #c6d5e8; font: .88rem/1.5 ui-monospace, SFMono-Regular, Consolas, monospace; }
    #jobs-list { display: grid; gap: .6rem; margin: 0; padding: 0; list-style: none; }
    #jobs-list li { display: flex; align-items: center; justify-content: space-between; gap: .6rem; min-width: 0; padding: .7rem; border: 1px solid #26344a; border-radius: .7rem; background: #0b1424; color: #c6d5e8; font-size: .88rem; overflow-wrap: anywhere; }
    #jobs-list button { min-height: 2.4rem; flex: 0 0 auto; padding: 0 .7rem; border-color: #7f3441; background: #451923; color: #fecdd3; font-size: .82rem; }
    #jobs-list button:active { background: #672331; }
    @media (max-width: 360px) { body { padding-right: .75rem; padding-left: .75rem; } section { padding: .8rem; } .command-row { gap: .4rem; } .command-row > button { padding: 0 .7rem; } }
  </style>
</head>
<body>
  <header><h1>Commandware</h1><p>Change your ESP8266 behavior with commands</p></header>
  <main>
    <section aria-label="Command console">
      <form id="command-form">
        <div class="command-row">
          <div class="command-entry">
            <textarea rows="3" id="command" name="command" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="e.g. read 4" required aria-autocomplete="list" aria-controls="command-suggestions" aria-expanded="false"></textarea>
            <button id="command-action" type="button" aria-label="Show command suggestions" aria-expanded="false">▾</button>
            <div id="command-suggestions" class="suggestions" role="listbox" hidden></div>
          </div>
          <button type="submit">Run</button>
        </div>
      </form>
      <pre id="result" aria-live="polite">Choose a command or enter one above.</pre>
    </section>
    <section aria-labelledby="jobs-heading">
      <h2 id="jobs-heading">Running jobs</h2>
      <ul id="jobs-list"><li>Loading...</li></ul>
    </section>
  </main>
  <script>
    const f=document.getElementById('command-form'),i=document.getElementById('command'),r=document.getElementById('result'),d=document.getElementById('command-suggestions'),a=document.getElementById('command-action'),j=document.getElementById('jobs-list'),p=['help','write 4 1','write 4 0','write 4 1 100','read 4','read 5','read 12','adc A0','ohm A0','status','clock 500 | write 5','clock 1000 --dur 500 | write 15','schedule { 1000 { clock 1000 --dur 500 | write 15 } 1500 { clock 1400 --dur 1000 | write 15 } }'];let h=[],busy=false;
    function suggestions(){const all=[...h,...p.filter(c=>!h.includes(c))],query=i.value.trim().toLowerCase(),matches=query?all.filter(c=>c.toLowerCase().includes(query)):all;d.replaceChildren(...matches.map(c=>{const b=document.createElement('button');b.type='button';b.textContent=c;b.setAttribute('role','option');b.onclick=()=>{i.value=c;closeSuggestions();i.focus()};return b}));if(!matches.length){const empty=document.createElement('div');empty.textContent='No matching commands';empty.style.cssText='padding:.5rem;color:#93a4bc;font-size:.85rem';d.append(empty)}}
    function openSuggestions(){suggestions();d.hidden=false;a.textContent=i.value?'×':'▾';a.setAttribute('aria-label',i.value?'Clear command':'Show command suggestions');a.setAttribute('aria-expanded','true');i.setAttribute('aria-expanded','true')}
    function closeSuggestions(){d.hidden=true;a.textContent=i.value?'×':'▾';a.setAttribute('aria-label',i.value?'Clear command':'Show command suggestions');a.setAttribute('aria-expanded','false');i.setAttribute('aria-expanded','false')}
    function remember(c){h=[c,...h.filter(x=>x!==c)].slice(0,12);if(!d.hidden)suggestions()}
    let requestQueue=Promise.resolve();
    function send(c){const request=requestQueue.then(async()=>{const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),10000);try{const x=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({command:c}),signal:controller.signal}),t=await x.text();if(!x.ok)throw Error(t||x.statusText);return t}catch(e){if(e.name==='AbortError')throw Error('Request timed out. Check device connection; command may have executed.');throw e}finally{clearTimeout(timer)}});requestQueue=request.catch(()=>{});return request}
    async function refresh(){if(busy)return;busy=true;try{const t=await send('jobs'),a=t.split(/\r?\n/).map(s=>s.match(/^(\d+)\s+(RUNNING|FAILED)\s+(.*)$/)).filter(Boolean);j.replaceChildren();if(!a.length){j.textContent='No jobs running.';return}a.forEach(m=>{const li=document.createElement('li'),b=document.createElement('button');li.append(document.createTextNode(`#${m[1]} ${m[2]} ${m[3]} `));b.textContent='Kill';b.onclick=async()=>{b.disabled=true;try{r.textContent=await send(`kill ${m[1]}`);await refresh()}catch(e){r.textContent=`Request failed: ${e}`;b.disabled=false}};if(m[2]==='RUNNING')li.append(b);j.append(li)})}catch(e){j.textContent=`Unable to load jobs: ${e}`}finally{busy=false}}
    a.onclick=()=>{if(i.value)i.value='';openSuggestions();i.focus()};i.onfocus=()=>openSuggestions();i.oninput=()=>openSuggestions();document.addEventListener('click',e=>{if(!e.target.closest('.command-entry'))closeSuggestions()});document.addEventListener('keydown',e=>{if(e.key==='Escape')closeSuggestions()});f.onsubmit=async e=>{e.preventDefault();const c=i.value.trim();r.textContent='Running...';closeSuggestions();try{r.textContent=await send(c);remember(c);await refresh()}catch(x){r.textContent=`Request failed: ${x}`}};closeSuggestions();refresh();setInterval(refresh,3000);
  </script>
</body>
</html>
)rawliteral";

static void configureWebServer()
{
    webServer.on("/", HTTP_GET, []()
                 { webServer.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
    webServer.on("/api/command", HTTP_POST, []()
                 {
                     String command = webServer.arg("command");
                     if (trim(command).length() == 0)
                     {
                         webServer.send(400, "text/plain; charset=utf-8", "ERROR: command is required at line 1");
                         return;
                     }

                     StringPrint response;
                     Print *previousOutput = commandOutput;
                     commandOutput = &response;
                     handleCommand(command);
                     commandOutput = previousOutput;

                     int statusCode = response.str().startsWith("ERROR:") ? 400 : 200;
                     webServer.send(statusCode, "text/plain; charset=utf-8", response.str()); });
    webServer.onNotFound([]()
                         { webServer.send(404, "text/plain; charset=utf-8", "Not found"); });
}

static void serviceWebServer()
{
    if (WIFI_SSID[0] == '\0')
    {
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!webServerStarted)
        {
            webServer.begin();
            webServerStarted = true;
            Serial.print(F("Web interface: http://"));
            Serial.println(WiFi.localIP());
        }
        webServer.handleClient();
        return;
    }

    unsigned long now = millis();
    if (now - lastWifiAttempt >= WIFI_RETRY_INTERVAL)
    {
        lastWifiAttempt = now;
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.println(F("Reconnecting to Wi-Fi..."));
    }
}

enum StageKind
{
    STAGE_NONE,
    STAGE_MAP,
    STAGE_OHM,
    STAGE_NOT,
    STAGE_EDGE,
    STAGE_RISING,
    STAGE_FALLING,
    STAGE_DEBOUNCE,
    STAGE_TOGGLE,
    STAGE_DIVIDE,
    STAGE_PULSE,
    STAGE_WRITE,
    STAGE_PRINT,
    STAGE_LCD_PRINT,
    STAGE_OLED,
};

#include "ScriptRuntime.h"
#include "OledDisplay.h"
static cw::OledDisplay oledDisplay;
// Legacy command handlers also pass through this stream, so even errors from
// commands outside the script interpreter carry a location.
static uint16_t commandErrorLine = 1;
class ErrorLinePrint : public Print
{
    Print &destination;
    String line;
public:
    explicit ErrorLinePrint(Print &target) : destination(target) {}
    size_t write(uint8_t value) override
    {
        if (value == '\n') flushLine(true);
        else if (value != '\r') line += (char)value;
        return 1;
    }
    void flushLine(bool newline = false)
    {
        destination.print(line);
        if (line.startsWith("ERROR:") && line.indexOf(" at line ") < 0)
        {
            destination.print(F(" at line "));
            destination.print(commandErrorLine);
        }
        if (newline) destination.print('\n');
        line = "";
    }
};

class ErrorOutputScope
{
    Print *previous;
    uint16_t previousLine;
    ErrorLinePrint output;
public:
    explicit ErrorOutputScope(uint16_t sourceLine = 1)
        : previous(commandOutput), previousLine(commandErrorLine), output(*previous)
    { commandOutput = &output; commandErrorLine = sourceLine; }
    ~ErrorOutputScope()
    { output.flushLine(); commandOutput = previous; commandErrorLine = previousLine; }
};
static cw::Runtime scriptRuntime;
static std::map<int, unsigned long> scriptPinOwners;
static std::map<int, bool> scriptOutputPins;
static void configureScriptRuntime();
static bool scriptOwnsPin(int pin) { return scriptPinOwners.count(pin) != 0; }
static bool displayReservesPin(int pin);

struct Stage
{
    StageKind kind = STAGE_NONE;
    int p1 = 0;
    int p2 = 0;
    int p3 = 0;
    int p4 = 0;
    int prev = 0;
    bool hasPrev = false;
    int streak = 0;
    int counter = 0;
    int toggleState = 0;
    int pulseRemaining = 0;
    std::shared_ptr<std::vector<cw::Value>> oledArgs;
};

struct Job
{
    unsigned long id = 0;
    bool active = false;
    bool started = false;
    bool isClock = true;
    bool adcWatch = false;
    bool hasADC = false;
    uint16_t line = 1;
    int lastADC = 0;
    unsigned long startMs = 0;
    unsigned long durationMs = 0; // Zero means unlimited.
    unsigned long interval = 0;
    unsigned long next = 0;
    int clockState = 0;
    String pipeline;
    Stage stages[8];
    int stageCount = 0;
};

static Job jobs[8];
static const int MAX_SCHEDULED_COMMANDS = 32;
struct ScheduledCommand
{
    unsigned long id = 0;
    unsigned long startMs = 0;
    unsigned long durationMs = 0;
    unsigned long pinMask = 0;
    bool isClock = false;
    uint16_t line = 1;
    String pipeline;
};
static ScheduledCommand scheduled[MAX_SCHEDULED_COMMANDS];
static unsigned long nextJobId = 1;

struct ClockWindow
{
    unsigned long startMs = 0;
    unsigned long durationMs = 0;
    unsigned long pinMask = 0;
    bool active = false;
};
static String inputBuffer;
static int inputBraceDepth = 0;
static bool inputQuoted = false;
static bool inputEscaped = false;
static char inputQuoteChar = 0;
static bool inputComment = false;
static bool inputAfterCR = false;
static uint16_t inputLine = 1;

struct PendingWritePulse
{
    bool active = false;
    int pin = -1;
    unsigned long endTime = 0;
};

static PendingWritePulse pendingWritePulse;

struct SignalState
{
    bool hasPrev = false;
    int prev = 0;
    int streak = 0;
    int toggleState = 0;
    int divideCounter = 0;
    int pulseRemaining = 0;
};

static void stopJob(Job &job);

static SignalState directEdgeState;
static SignalState directRisingState;
static SignalState directFallingState;
static SignalState directDebounceState;
static SignalState directToggleState;
static SignalState directDivideState;
static SignalState directPulseState;

static String trim(String value)
{
    while (value.length() > 0 && (value[0] == ' ' || value[0] == '\t' || value[0] == '\r' || value[0] == '\n'))
    {
        value.remove(0, 1);
    }
    while (value.length() > 0 && (value[value.length() - 1] == ' ' || value[value.length() - 1] == '\t' || value[value.length() - 1] == '\r' || value[value.length() - 1] == '\n'))
    {
        value.remove(value.length() - 1, 1);
    }
    return value;
}

static String nextToken(String &input)
{
    String trimmed = trim(input);
    if (trimmed.length() == 0)
    {
        input = "";
        return "";
    }
    int idx = 0;
    while (idx < (int)trimmed.length() && trimmed[idx] != ' ' && trimmed[idx] != '\t' && trimmed[idx] != '\r' && trimmed[idx] != '\n')
        ++idx;
    String token;
    if (idx == (int)trimmed.length())
    {
        token = trimmed;
        input = "";
    }
    else
    {
        token = trimmed.substring(0, idx);
        input = trimmed.substring(idx + 1);
    }
    return token;
}

static int toIntDef(const String &value, int fallback)
{
    if (value.length() == 0)
    {
        return fallback;
    }
    return value.toInt();
}

static int mapValue(int value, int inMin, int inMax, int outMin, int outMax)
{
    if (inMax == inMin)
    {
        return outMin;
    }
    long scaled = (long)(value - inMin) * (outMax - outMin);
    long denom = inMax - inMin;
    return (int)(outMin + (scaled / denom));
}

// Common PCF8574 backpack: P0=RS, P1=RW, P2=E, P3=backlight,
// P4..P7=D4..D7. Wire ships with the ESP8266 core.
static bool lcdReady = false;
static uint8_t lcdAddress = 0x27;
static const unsigned long LCD_PIN_MASK = (1UL << 4) | (1UL << 5);
static bool displayReservesPin(int pin) { return (lcdReady || oledDisplay.reserved) && (pin == 4 || pin == 5); }

static bool lcdNibble(uint8_t value)
{
    Wire.beginTransmission(lcdAddress);
    Wire.write((uint8_t)(value | 0x08));
    Wire.write((uint8_t)(value | 0x0c));
    Wire.write((uint8_t)(value | 0x08));
    return Wire.endTransmission() == 0;
}

static bool lcdByte(uint8_t value, bool data = false)
{
    bool ok = lcdNibble((value & 0xf0) | (data ? 1 : 0)) &&
              lcdNibble((value << 4) | (data ? 1 : 0));
    delayMicroseconds(50);
    return ok;
}

static bool lcdFailure()
{
    lcdReady = false;
    commandOutput->println(F("ERROR: LCD I2C communication failed; check wiring and address"));
    return false;
}

static bool initLCD(uint8_t address)
{
    if (oledDisplay.reserved && address == oledDisplay.address) {
        commandOutput->println(F("ERROR: LCD address is already used by OLED")); return false;
    }
    if (scriptOwnsPin(4) || scriptOwnsPin(5))
    {
        commandOutput->println(F("ERROR: LCD pins are owned by a script job"));
        return false;
    }
    // Do not take the bus pins away from running or queued GPIO writers.
    for (int i = 0; i < 8; ++i)
        if (jobs[i].active)
            for (int s = 0; s < jobs[i].stageCount; ++s)
                if (jobs[i].stages[s].kind == STAGE_WRITE &&
                    (jobs[i].stages[s].p1 == 4 || jobs[i].stages[s].p1 == 5))
                {
                    commandOutput->println(F("ERROR: LCD GPIO4/GPIO5 are used by a job"));
                    return false;
                }
    for (int i = 0; i < MAX_SCHEDULED_COMMANDS; ++i)
        if (scheduled[i].id && (scheduled[i].pinMask & LCD_PIN_MASK))
        {
            commandOutput->println(F("ERROR: LCD GPIO4/GPIO5 are used by a queued job"));
            return false;
        }
    if (pendingWritePulse.active && (pendingWritePulse.pin == 4 || pendingWritePulse.pin == 5))
    {
        commandOutput->println(F("ERROR: LCD GPIO4/GPIO5 are used by a pulse"));
        return false;
    }
    lcdReady = false;
    lcdAddress = address;
    Wire.begin(4, 5); // SDA, SCL: GPIO numbers, not board D labels.
    Wire.setClock(100000);
    Wire.beginTransmission(lcdAddress);
    if (Wire.endTransmission() != 0) return lcdFailure();
    // HD44780 power-on / 4-bit initialization (datasheet figure 24).
    delay(50);
    if (!lcdNibble(0x30)) return lcdFailure();
    delayMicroseconds(4500);
    if (!lcdNibble(0x30)) return lcdFailure();
    delayMicroseconds(150);
    if (!lcdNibble(0x30) || !lcdNibble(0x20) || !lcdByte(0x28) ||
        !lcdByte(0x08) || !lcdByte(0x01)) return lcdFailure();
    delay(2);
    if (!lcdByte(0x06) || !lcdByte(0x0c)) return lcdFailure();
    lcdReady = true;
    return true;
}

static bool printLCD(const String &text)
{
    if (!lcdReady && !initLCD(lcdAddress)) return false;
    char cells[32];
    for (int i = 0; i < 32; ++i) cells[i] = ' ';
    int pos = 0;
    for (unsigned int i = 0; i < text.length() && pos < 32; ++i)
        if (text[i] == '\n') pos = pos < 16 ? 16 : 32;
        else cells[pos++] = text[i];
    // Overwrite with spaces instead of clearing to avoid flicker and stale digits.
    for (int row = 0; row < 2; ++row)
    {
        if (!lcdByte(row ? 0xc0 : 0x80)) return lcdFailure();
        for (int col = 0; col < 16; ++col)
            if (!lcdByte(cells[row * 16 + col], true)) return lcdFailure();
    }
    return true;
}

static bool lcdLiteral(String args, String &text)
{
    args = trim(args);
    if (!args.startsWith("\"")) return false;
    for (unsigned int i = 1; i < args.length(); ++i)
    {
        char ch = args[i];
        if (ch == '"') return trim(args.substring(i + 1)).length() == 0;
        if (ch == '\\')
        {
            if (++i == args.length()) return false;
            ch = args[i];
            if (ch == 'n') ch = '\n';
            else if (ch != '\\' && ch != '"') return false;
        }
        text += ch;
    }
    return false;
}

static void handleLCDCommand(String args)
{
    String action = nextToken(args);
    action.toLowerCase();
    if (action == "print")
    {
        String text;
        if (!lcdLiteral(args, text))
            commandOutput->println(F("ERROR: lcd print requires a double-quoted string or piped input"));
        else if (printLCD(text)) commandOutput->println(F("OK"));
        return;
    }
    if (action == "clear" && !trim(args).length())
    {
        if (printLCD("")) commandOutput->println(F("OK"));
        return;
    }
    if (action == "init")
    {
        String token = nextToken(args);
        char *end;
        long address = token.length() ? strtol(token.c_str(), &end, 0) : 0x27;
        if ((token.length() && *end) || address < 0x08 || address > 0x77 || trim(args).length())
            commandOutput->println(F("ERROR: lcd init accepts an I2C address (0x08..0x77)"));
        else if (initLCD((uint8_t)address)) commandOutput->println(F("OK"));
        return;
    }
    commandOutput->println(F("ERROR: use lcd init [ADDRESS], lcd print \"TEXT\", or lcd clear"));
}

// Delimiters inside LCD strings are literal characters, including escaped quotes.
static int unquotedIndex(const String &text, char target, int start = 0)
{
    bool quoted = false, escaped = false;
    for (unsigned int i = start; i < text.length(); ++i)
    {
        char ch = text[i];
        if (quoted && escaped) { escaped = false; continue; }
        if (quoted && ch == '\\') { escaped = true; continue; }
        if (ch == '"') { quoted = !quoted; continue; }
        if (!quoted && ch == target) return i;
    }
    return -1;
}

static void printHelp()
{
    commandOutput->println(F("Commandware ESP-12E shell"));
    commandOutput->println(F("help"));
    commandOutput->println(F("pins"));
    commandOutput->println(F("jobs"));
    commandOutput->println(F("kill ID"));
    commandOutput->println(F("status"));
    commandOutput->println(F("reset"));
    commandOutput->println(F("read PIN"));
    commandOutput->println(F("write PIN VALUE [DURATION_MS]"));
    commandOutput->println(F("adc A0 [--watch] (watch samples changes every 100 ms)"));
    commandOutput->println(F("lcd init [ADDRESS] (16x2 I2C, default 0x27; SDA=4 SCL=5)"));
    commandOutput->println(F("lcd print \"TEXT\"; lcd clear; SOURCE | lcd print"));
    commandOutput->println(F("oled init [ssd1306|sh1106] [--height 32|64] [--address 0x3c|0x3d]"));
    commandOutput->println(F("oled print [X Y] [TEXT] [--x 0 --y 0]; oled clear"));
    commandOutput->println(F("SOURCE | oled chart [VALUE] [--min 0 --max 1023 --x 0 --y 0 --width 128 --height 64]"));
    commandOutput->println(F("ohm [A0] (ohms, <270, >700k, OUT_OF_RANGE)"));
    commandOutput->println(F("adc A0 | ohm (terminal measurement stage)"));
    commandOutput->println(F("clock HZ [PIN] [--dur MILLISECONDS]"));
    commandOutput->println(F("schedule { START_MS { COMMAND | STAGE } ... }"));
    commandOutput->println(F("map IN_MIN IN_MAX OUT_MIN OUT_MAX VALUE"));
    commandOutput->println(F("not VALUE"));
    commandOutput->println(F("edge VALUE"));
    commandOutput->println(F("rising VALUE"));
    commandOutput->println(F("falling VALUE"));
    commandOutput->println(F("debounce COUNT VALUE"));
    commandOutput->println(F("toggle [VALUE]"));
    commandOutput->println(F("divide N VALUE"));
    commandOutput->println(F("pulse DURATION VALUE"));
    commandOutput->println(F("print VALUE"));
    commandOutput->println(F("shiftin DATA CLOCK LOAD [msb|lsb] [BITS]"));
    commandOutput->println(F("shiftout DATA CLOCK LATCH [msb|lsb] VALUE"));
    commandOutput->println(F("do { COMMAND; COMMAND; ... }"));
    commandOutput->println(F("every MILLISECONDS | COMMAND | ..."));
    commandOutput->println(F("var NAME EXPR / set NAME EXPR (read with $NAME)"));
    commandOutput->println(F("func NAME [PARAM...] { BODY } / call NAME [ARG...]"));
    commandOutput->println(F("split DELIM [VALUE | --value VALUE] [--out NAME,...]"));
    commandOutput->println(F("each [--out NAME] [ { BODY } ] (sequential)"));
    commandOutput->println(F("tone PIN --hz HZ --dur MS / sleep MS"));
}

static void printPins()
{
    const char *names[] = {
        "GPIO0", "GPIO1", "GPIO2", "GPIO3", "GPIO4", "GPIO5",
        "GPIO12", "GPIO13", "GPIO14", "GPIO15", "GPIO16", "A0"};
    const uint8_t pins[] = {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, A0};
    for (int i = 0; i < 12; ++i)
    {
        commandOutput->print(names[i]);
        commandOutput->print("\t");
        if (pins[i] == 1 || pins[i] == 3)
        {
            commandOutput->println(F("terminal"));
        }
        else if (displayReservesPin(pins[i]))
        {
            commandOutput->println(oledDisplay.reserved ? F("OLED I2C") : F("LCD I2C"));
        }
        else if (scriptOwnsPin(pins[i]))
        {
            commandOutput->print(F("script job "));
            commandOutput->println(scriptPinOwners[pins[i]]);
        }
        else
        {
            commandOutput->println(F("available"));
        }
    }
}

static void printJobs()
{
    for (const auto &failure : scriptRuntime.failures)
    {
        commandOutput->print(failure.id);
        commandOutput->print(F(" FAILED  "));
        commandOutput->print(failure.label.c_str());
        commandOutput->print(F(": "));
        commandOutput->println(failure.message.c_str());
    }
    for (const auto &task : scriptRuntime.tasks)
    {
        commandOutput->print(task->id);
        commandOutput->print(F(" RUNNING  "));
        commandOutput->println(task->label.c_str());
    }
    for (int i = 0; i < 8; ++i)
    {
        if (!jobs[i].active)
        {
            continue;
        }
        commandOutput->print(jobs[i].id);
        commandOutput->print(F(" RUNNING  "));
        commandOutput->println(jobs[i].pipeline);
    }
    for (int i = 0; i < MAX_SCHEDULED_COMMANDS; ++i)
        if (scheduled[i].id)
        {
            commandOutput->print(scheduled[i].id);
            commandOutput->print(F(" RUNNING  [queued] "));
            commandOutput->println(scheduled[i].pipeline);
        }
}

static int splitPipeline(const String &line, String parts[], int maxParts)
{
    int count = 0;
    int start = 0;
    while (start <= line.length() && count < maxParts)
    {
        int idx = unquotedIndex(line, '|', start);
        String piece;
        if (idx < 0)
        {
            piece = line.substring(start);
            start = line.length() + 1;
        }
        else
        {
            piece = line.substring(start, idx);
            start = idx + 1;
        }
        piece = trim(piece);
        if (piece.length() > 0)
        {
            parts[count++] = piece;
        }
    }
    return count;
}

// Measured counts -> millivolts -> piecewise exponential resistance.
// Calibration: 20160-ohm reference, 3357-ohm parallel shunt.
static void printOhmReading(int raw)
{
    if (raw < 0 || raw > 1023)
    {
        commandOutput->println(F("OUT_OF_RANGE"));
        return;
    }
    // Counts 30-49 extrapolate the first measured segment (which starts at 50).
    if (raw < 30)
    {
        commandOutput->println(F("<270"));
        return;
    }
    if (raw > 720)
    {
        commandOutput->println(F(">700k"));
        return;
    }
    static const int counts[] = {50, 179, 291, 459, 557, 628, 703, 720};
    static const float millivolts[] = {60, 181, 288, 449, 543, 612, 682, 697};
    static const long resistances[] = {270, 995, 1992, 5152, 9959, 20020, 108800, 688900};
    for (int i = 0; i < 8; ++i)
    {
        if (raw == counts[i])
        {
            commandOutput->println(resistances[i]);
            return;
        }
    }
    for (int i = 1; i < 8; ++i)
    {
        if (raw <= counts[i])
        {
            float voltage = (millivolts[i - 1] + (raw - counts[i - 1]) *
                (millivolts[i] - millivolts[i - 1]) / (counts[i] - counts[i - 1]));
            float fraction = (voltage - millivolts[i - 1]) / (millivolts[i] - millivolts[i - 1]);
            float resistance = resistances[i - 1] * expf(fraction *
                logf((float)resistances[i] / resistances[i - 1]));
            commandOutput->println((long)(resistance + 0.5f));
            return;
        }
    }
}

static bool applySignalStage(Stage &stage, bool &hasValue, int &value)
{
    if (!hasValue)
    {
        return true;
    }

    switch (stage.kind)
    {
    case STAGE_OHM:
        printOhmReading(value);
        hasValue = false;
        return true;
    case STAGE_MAP:
        value = mapValue(value, stage.p1, stage.p2, stage.p3, stage.p4);
        return true;
    case STAGE_NOT:
        value = (value == 0) ? 1 : 0;
        return true;
    case STAGE_EDGE:
        if (!stage.hasPrev)
        {
            stage.prev = value;
            stage.hasPrev = true;
            return true;
        }
        if (value == stage.prev)
        {
            hasValue = false;
            return true;
        }
        stage.prev = value;
        return true;
    case STAGE_RISING:
    {
        bool emit = stage.hasPrev && value > stage.prev;
        stage.prev = value;
        stage.hasPrev = true;
        if (!emit)
        {
            hasValue = false;
        }
        return true;
    }
    case STAGE_FALLING:
    {
        bool emit = stage.hasPrev && value < stage.prev;
        stage.prev = value;
        stage.hasPrev = true;
        if (!emit)
        {
            hasValue = false;
        }
        return true;
    }
    case STAGE_DEBOUNCE:
    {
        int threshold = max(1, stage.p1);
        if (!stage.hasPrev || value != stage.prev)
        {
            stage.prev = value;
            stage.hasPrev = true;
            stage.streak = 1;
        }
        else
        {
            stage.streak++;
        }
        if (stage.streak < threshold)
        {
            hasValue = false;
        }
        return true;
    }
    case STAGE_TOGGLE:
        stage.toggleState = (stage.toggleState == 0) ? 1 : 0;
        value = stage.toggleState;
        return true;
    case STAGE_DIVIDE:
    {
        int divisor = max(1, stage.p1);
        stage.counter++;
        if ((stage.counter % divisor) != 0)
        {
            hasValue = false;
        }
        return true;
    }
    case STAGE_PULSE:
    {
        int duration = max(1, stage.p1);
        bool rising = stage.hasPrev && stage.prev == 0 && value == 1;
        if (!stage.hasPrev && value == 1)
        {
            rising = true;
        }
        stage.prev = value;
        stage.hasPrev = true;
        if (rising)
        {
            stage.pulseRemaining = duration;
        }
        value = stage.pulseRemaining > 0 ? 1 : 0;
        if (stage.pulseRemaining > 0)
        {
            stage.pulseRemaining--;
        }
        return true;
    }
    case STAGE_WRITE:
    {
        int writeValue = (stage.p2 >= 0) ? stage.p2 : value;
        int durationMs = (stage.p3 >= 0) ? stage.p3 : ((stage.p2 >= 0) ? value : -1);

        pinMode(stage.p1, OUTPUT);
        digitalWrite(stage.p1, writeValue ? HIGH : LOW);

        if (durationMs > 0 && writeValue != 0)
        {
            pendingWritePulse.pin = stage.p1;
            pendingWritePulse.endTime = millis() + (unsigned long)durationMs;
            pendingWritePulse.active = true;
        }
        return true;
    }
    case STAGE_PRINT:
        commandOutput->println(value);
        return true;
    case STAGE_LCD_PRINT:
        return printLCD(String(value));
    case STAGE_OLED:
    {
        cw::Event event; event.hasValue = hasValue; event.value = cw::Value(value);
        std::string error;
        if (oledDisplay.command(*stage.oledArgs, event, commandErrorLine, (uint32_t)millis(), error)) return true;
        commandOutput->print(F("ERROR: ")); commandOutput->println(error.c_str()); return false;
    }
    default:
        return false;
    }
}

static bool parsePipelineStage(const String &stageText, Stage &stage)
{
    String oledText = stageText;
    if (nextToken(oledText).equalsIgnoreCase("oled")) {
        std::string error; cw::Parser parser(error); auto body = parser.parse(stageText.c_str());
        if (!error.empty() || body.size() != 1 || body[0].size() != 1 || !body[0][0]->body.empty() || body[0][0]->args.empty()) return false;
        auto args = std::make_shared<std::vector<cw::Value>>();
        cw::Scope scope; cw::Event event;
        for (const auto &token : body[0][0]->args) {
            args->push_back(cw::expressionToken(token) ? cw::Expression(token.text, scope, event, error).run() : cw::Value(token.text));
            if (!error.empty()) return false;
        }
        stage.kind = STAGE_OLED; stage.oledArgs = args; return true;
    }
    String args = stageText;
    String name = nextToken(args);
    name.toLowerCase();

    if (name == "lcd")
    {
        if (!nextToken(args).equalsIgnoreCase("print") || trim(args).length()) return false;
        stage.kind = STAGE_LCD_PRINT;
        return true;
    }
    if (name == "ohm")
    {
        if (args.length() != 0)
            return false;
        stage.kind = STAGE_OHM;
        return true;
    }
    if (name == "map")
    {
        stage.kind = STAGE_MAP;
        stage.p1 = toIntDef(nextToken(args), 0);
        stage.p2 = toIntDef(nextToken(args), 1);
        stage.p3 = toIntDef(nextToken(args), 0);
        stage.p4 = toIntDef(nextToken(args), 1);
        return true;
    }
    if (name == "not")
    {
        stage.kind = STAGE_NOT;
        return true;
    }
    if (name == "edge")
    {
        stage.kind = STAGE_EDGE;
        return true;
    }
    if (name == "rising")
    {
        stage.kind = STAGE_RISING;
        return true;
    }
    if (name == "falling")
    {
        stage.kind = STAGE_FALLING;
        return true;
    }
    if (name == "debounce")
    {
        stage.kind = STAGE_DEBOUNCE;
        stage.p1 = toIntDef(nextToken(args), 1);
        return true;
    }
    if (name == "toggle")
    {
        stage.kind = STAGE_TOGGLE;
        return true;
    }
    if (name == "divide")
    {
        stage.kind = STAGE_DIVIDE;
        stage.p1 = toIntDef(nextToken(args), 2);
        return true;
    }
    if (name == "pulse")
    {
        stage.kind = STAGE_PULSE;
        stage.p1 = toIntDef(nextToken(args), 1);
        return true;
    }
    if (name == "write")
    {
        int pin = toIntDef(nextToken(args), -1);
        if (pin < 0 || scriptOwnsPin(pin) || displayReservesPin(pin))
        {
            return false;
        }
        stage.kind = STAGE_WRITE;
        stage.p1 = pin;
        stage.p2 = -1;
        stage.p3 = -1;

        String valueToken = nextToken(args);
        if (valueToken.length() > 0)
        {
            stage.p2 = toIntDef(valueToken, -1);
            String durationToken = nextToken(args);
            if (durationToken.length() > 0)
            {
                stage.p3 = toIntDef(durationToken, -1);
            }
        }
        return true;
    }
    if (name == "print")
    {
        stage.kind = STAGE_PRINT;
        return true;
    }

    return false;
}

static void handleReadCommand(String args)
{
    String token = nextToken(args);
    int pin = toIntDef(token, -1);
    if (pin < 0)
    {
        commandOutput->println(F("ERROR: read requires PIN"));
        return;
    }
    pinMode(pin, INPUT);
    commandOutput->println(digitalRead(pin));
}

static void handleWriteCommand(String args)
{
    String token = nextToken(args);
    int pin = toIntDef(token, -1);
    String valueToken = nextToken(args);
    int value = toIntDef(valueToken, LOW);
    String durationToken = nextToken(args);
    int durationMs = toIntDef(durationToken, -1);
    if (pin < 0)
    {
        commandOutput->println(F("ERROR: write requires PIN VALUE"));
        return;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, value ? HIGH : LOW);

    if (value == 0)
    {
        pendingWritePulse.active = false;
        pendingWritePulse.pin = -1;
        pendingWritePulse.endTime = 0;
    }
    else if (durationMs > 0)
    {
        pendingWritePulse.pin = pin;
        pendingWritePulse.endTime = millis() + (unsigned long)durationMs;
        pendingWritePulse.active = true;
    }
    else
    {
        pendingWritePulse.active = false;
        pendingWritePulse.pin = -1;
        pendingWritePulse.endTime = 0;
    }

    commandOutput->println(value ? 1 : 0);
}

static void handleADCCommand(String args)
{
    String token = nextToken(args);
    if (token.length() == 0 || !token.equalsIgnoreCase("A0") || trim(args).length())
    {
        commandOutput->println(F("ERROR: adc requires A0"));
        return;
    }
    commandOutput->println(analogRead(A0));
}

static void handleMapCommand(String args)
{
    String a1 = nextToken(args);
    String a2 = nextToken(args);
    String a3 = nextToken(args);
    String a4 = nextToken(args);
    String value = nextToken(args);
    int inMin = a1.toInt();
    int inMax = a2.toInt();
    int outMin = a3.toInt();
    int outMax = a4.toInt();
    int current = value.toInt();
    commandOutput->println(mapValue(current, inMin, inMax, outMin, outMax));
}

static void handleNotCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    commandOutput->println(current == 0 ? 1 : 0);
}

static void handleEdgeCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    if (!directEdgeState.hasPrev || current != directEdgeState.prev)
    {
        commandOutput->println(current);
    }
    else
    {
        commandOutput->println(0);
    }
    directEdgeState.prev = current;
    directEdgeState.hasPrev = true;
}

static void handleRisingCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    int output = (directRisingState.hasPrev && current > directRisingState.prev) ? 1 : 0;
    directRisingState.prev = current;
    directRisingState.hasPrev = true;
    commandOutput->println(output);
}

static void handleFallingCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    int output = (directFallingState.hasPrev && current < directFallingState.prev) ? 1 : 0;
    directFallingState.prev = current;
    directFallingState.hasPrev = true;
    commandOutput->println(output);
}

static void handleDebounceCommand(String args)
{
    int threshold = max(1, toIntDef(nextToken(args), 1));
    int current = toIntDef(nextToken(args), 0);

    if (!directDebounceState.hasPrev || current != directDebounceState.prev)
    {
        directDebounceState.prev = current;
        directDebounceState.hasPrev = true;
        directDebounceState.streak = 1;
    }
    else
    {
        directDebounceState.streak++;
    }

    if (directDebounceState.streak >= threshold)
    {
        commandOutput->println(current);
    }
    else
    {
        commandOutput->println(0);
    }
}

static void handleToggleCommand(String args)
{
    (void)args;
    directToggleState.toggleState = (directToggleState.toggleState == 0) ? 1 : 0;
    commandOutput->println(directToggleState.toggleState);
}

static void handleDivideCommand(String args)
{
    int divisor = max(1, toIntDef(nextToken(args), 2));
    int current = toIntDef(nextToken(args), 0);
    directDivideState.divideCounter++;
    if ((directDivideState.divideCounter % divisor) == 0)
    {
        commandOutput->println(current);
    }
    else
    {
        commandOutput->println(0);
    }
}

static void handlePulseCommand(String args)
{
    int duration = max(1, toIntDef(nextToken(args), 1));
    int current = toIntDef(nextToken(args), 0);
    bool rising = directPulseState.hasPrev && directPulseState.prev == 0 && current == 1;
    if (!directPulseState.hasPrev && current == 1)
    {
        rising = true;
    }
    directPulseState.prev = current;
    directPulseState.hasPrev = true;
    if (rising)
    {
        directPulseState.pulseRemaining = duration;
    }
    int output = directPulseState.pulseRemaining > 0 ? 1 : 0;
    if (directPulseState.pulseRemaining > 0)
    {
        directPulseState.pulseRemaining--;
    }
    commandOutput->println(output);
}

static void handlePrintCommand(String args)
{
    int value = toIntDef(nextToken(args), 0);
    commandOutput->println(value);
}

static void handleShiftinCommand(String args)
{
    int dataPin = toIntDef(nextToken(args), -1);
    int clockPin = toIntDef(nextToken(args), -1);
    int loadPin = toIntDef(nextToken(args), -1);
    String orderToken = nextToken(args);
    int bits = max(1, toIntDef(nextToken(args), 8));
    if (dataPin < 0 || clockPin < 0 || loadPin < 0)
    {
        commandOutput->println(F("ERROR: shiftin requires DATA CLOCK LOAD [msb|lsb] [BITS]"));
        return;
    }

    bool msb = orderToken.length() == 0 || orderToken.equalsIgnoreCase("msb");

    pinMode(dataPin, INPUT);
    pinMode(clockPin, OUTPUT);
    pinMode(loadPin, OUTPUT);

    digitalWrite(loadPin, LOW);
    digitalWrite(loadPin, HIGH);

    unsigned int value = 0;
    for (int i = 0; i < bits; ++i)
    {
        digitalWrite(clockPin, HIGH);
        int bit = digitalRead(dataPin) ? 1 : 0;
        digitalWrite(clockPin, LOW);
        if (msb)
        {
            value = (value << 1) | bit;
        }
        else
        {
            value |= (bit << i);
        }
    }

    commandOutput->println((int)value);
}

static void handleShiftoutCommand(String args)
{
    String dataToken = nextToken(args);
    String clockToken = nextToken(args);
    String latchToken = nextToken(args);
    String orderToken = nextToken(args);
    String valueToken = nextToken(args);

    int dataPin = dataToken.toInt();
    int clockPin = clockToken.toInt();
    int latchPin = latchToken.toInt();
    int value = valueToken.toInt();

    pinMode(dataPin, OUTPUT);
    pinMode(clockPin, OUTPUT);
    pinMode(latchPin, OUTPUT);

    bool msb = orderToken.length() == 0 || orderToken.equalsIgnoreCase("msb");
    for (int i = 0; i < 8; ++i)
    {
        int bitIndex = msb ? (7 - i) : i;
        int bit = (value >> bitIndex) & 1;
        digitalWrite(dataPin, bit ? HIGH : LOW);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, LOW);
    }
    digitalWrite(latchPin, HIGH);
    digitalWrite(latchPin, LOW);
    commandOutput->println(value);
}

static void handleKillCommand(String args)
{
    int id = toIntDef(nextToken(args), -1);
    if (id < 1)
    {
        commandOutput->println(F("ERROR: kill requires valid job id"));
        return;
    }
    if (scriptRuntime.kill((unsigned long)id))
    {
        commandOutput->print(F("killed "));
        commandOutput->println(id);
        return;
    }
    for (int i = 0; i < 8; ++i)
        if (jobs[i].active && jobs[i].id == (unsigned long)id)
        {
            stopJob(jobs[i]);
            commandOutput->print(F("killed "));
            commandOutput->println(id);
            return;
        }
    for (int i = 0; i < MAX_SCHEDULED_COMMANDS; ++i)
        if (scheduled[i].id == (unsigned long)id)
        {
            scheduled[i] = ScheduledCommand();
            commandOutput->print(F("killed "));
            commandOutput->println(id);
            return;
        }
    commandOutput->print(F("ERROR: unknown job "));
    commandOutput->println(id);
}

static bool evaluateImmediateSource(const String &sourceText, bool &hasValue, int &value)
{
    String args = sourceText;
    String name = nextToken(args);
    name.toLowerCase();

    if (name == "read")
    {
        int pin = toIntDef(nextToken(args), -1);
        if (scriptOwnsPin(pin) || displayReservesPin(pin))
        {
            commandOutput->println(F("ERROR: GPIO4/GPIO5 are reserved for display I2C"));
            return false;
        }
        if (pin < 0)
        {
            commandOutput->println(F("ERROR: read requires PIN"));
            return false;
        }
        pinMode(pin, INPUT);
        value = digitalRead(pin) ? 1 : 0;
        hasValue = true;
        return true;
    }
    if (name == "adc")
    {
        String pin = nextToken(args);
        if (!pin.equalsIgnoreCase("A0") || trim(args).length())
        {
            commandOutput->println(F("ERROR: adc requires A0"));
            return false;
        }
        value = analogRead(A0);
        hasValue = true;
        return true;
    }
    if (name == "map")
    {
        int inMin = toIntDef(nextToken(args), 0);
        int inMax = toIntDef(nextToken(args), 1);
        int outMin = toIntDef(nextToken(args), 0);
        int outMax = toIntDef(nextToken(args), 1);
        int current = toIntDef(nextToken(args), 0);
        value = mapValue(current, inMin, inMax, outMin, outMax);
        hasValue = true;
        return true;
    }
    if (name == "not")
    {
        value = toIntDef(nextToken(args), 0);
        value = (value == 0) ? 1 : 0;
        hasValue = true;
        return true;
    }
    if (name == "edge" || name == "rising" || name == "falling" || name == "debounce" || name == "toggle" || name == "divide" || name == "pulse")
    {
        int current = toIntDef(nextToken(args), 0);
        hasValue = true;
        value = current;
        return true;
    }
    if (name == "write")
    {
        int pin = toIntDef(nextToken(args), -1);
        if (scriptOwnsPin(pin) || displayReservesPin(pin))
        {
            commandOutput->println(F("ERROR: GPIO4/GPIO5 are reserved for display I2C"));
            return false;
        }
        if (pin < 0)
        {
            commandOutput->println(F("ERROR: write requires PIN VALUE"));
            return false;
        }
        int v = toIntDef(nextToken(args), 0);
        pinMode(pin, OUTPUT);
        digitalWrite(pin, v ? HIGH : LOW);
        value = v ? 1 : 0;
        hasValue = true;
        return true;
    }
    if (name == "print")
    {
        value = toIntDef(nextToken(args), 0);
        hasValue = true;
        return true;
    }

    commandOutput->print(F("ERROR: unsupported pipeline source '"));
    commandOutput->print(name);
    commandOutput->println(F("'"));
    return false;
}

static void runImmediatePipeline(const String &line)
{
    String parts[10];
    int partCount = splitPipeline(line, parts, 10);
    if (partCount <= 0)
    {
        return;
    }

    bool hasValue = false;
    int value = 0;
    if (!evaluateImmediateSource(parts[0], hasValue, value))
    {
        return;
    }

    bool hasSink = false;
    for (int i = 1; i < partCount; ++i)
    {
        Stage stage;
        if (!parsePipelineStage(parts[i], stage))
        {
            commandOutput->print(F("ERROR: unsupported pipeline stage '"));
            commandOutput->print(parts[i]);
            commandOutput->println(F("'"));
            return;
        }
        if (stage.kind == STAGE_WRITE || stage.kind == STAGE_PRINT || stage.kind == STAGE_LCD_PRINT || stage.kind == STAGE_OLED)
        {
            hasSink = true;
        }
        if (!applySignalStage(stage, hasValue, value))
        {
            commandOutput->println(F("ERROR: pipeline stage failure"));
            return;
        }
    }

    if (hasValue && !hasSink)
    {
        commandOutput->println(value);
    }
}

// Restrict deadlines to half the millis() range for rollover-safe comparisons.
static bool parseMilliseconds(const String &token, unsigned long &value, bool allowZero)
{
    if (!token.length()) return false;
    value = 0;
    for (unsigned int i = 0; i < token.length(); ++i)
    {
        if (token[i] < '0' || token[i] > '9') return false;
        unsigned int digit = token[i] - '0';
        if (value > (2147483647UL - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return allowZero || value > 0;
}

static void stopJob(Job &job)
{
    if (job.started && job.isClock)
    {
        for (int s = 0; s < job.stageCount; ++s)
            if (job.stages[s].kind == STAGE_WRITE)
            {
                int pin = job.stages[s].p1;
                pinMode(pin, OUTPUT);
                digitalWrite(pin, LOW);
                if (pendingWritePulse.active && pendingWritePulse.pin == pin)
                    pendingWritePulse.active = false;
            }
    }
    job = Job();
}

static ClockWindow clockWindow(const Job &job)
{
    ClockWindow result;
    result.active = job.active && job.isClock;
    result.startMs = job.startMs;
    result.durationMs = job.durationMs;
    for (int i = 0; i < job.stageCount; ++i)
        if (job.stages[i].kind == STAGE_WRITE)
            result.pinMask |= 1UL << job.stages[i].p1;
        else if (job.stages[i].kind == STAGE_LCD_PRINT || job.stages[i].kind == STAGE_OLED)
            result.pinMask |= LCD_PIN_MASK | (1UL << 18);
    return result;
}

static ClockWindow clockWindow(const ScheduledCommand &entry)
{
    ClockWindow result;
    result.active = entry.isClock && entry.pipeline.length();
    result.startMs = entry.startMs;
    result.durationMs = entry.durationMs;
    result.pinMask = entry.pinMask;
    return result;
}

static bool windowsConflict(const ClockWindow &a, const ClockWindow &b, unsigned long now)
{
    if (!a.active || !b.active || !(a.pinMask & b.pinMask)) return false;
    int64_t aStart = (long)(a.startMs - now), bStart = (long)(b.startMs - now);
    int64_t aEnd = a.durationMs ? aStart + a.durationMs : INT64_MAX;
    int64_t bEnd = b.durationMs ? bStart + b.durationMs : INT64_MAX;
    return aEnd > bStart && bEnd > aStart;
}

static bool windowContains(const ClockWindow &window, int64_t offset, unsigned long now)
{
    int64_t start = (long)(window.startMs - now);
    return window.active && start <= offset &&
        (!window.durationMs || offset < start + window.durationMs);
}

static ClockWindow plannedWindow(int index, const ScheduledCommand *incoming, int count)
{
    if (index < 8) return clockWindow(jobs[index]);
    index -= 8;
    if (index < MAX_SCHEDULED_COMMANDS) return clockWindow(scheduled[index]);
    index -= MAX_SCHEDULED_COMMANDS;
    return index < count ? clockWindow(incoming[index]) : ClockWindow();
}

// Validate both GPIO ownership and the eight concurrent execution slots at
// every start boundary, without reserving one full pipeline per future note.
static bool validatePlan(const ScheduledCommand *incoming, int count, unsigned long now)
{
    int total = 8 + MAX_SCHEDULED_COMMANDS + count;
    for (int i = 0; i < total; ++i)
    {
        ClockWindow a = plannedWindow(i, incoming, count);
        if (!a.active) continue;
        int concurrent = 0;
        int64_t offset = max((int64_t)0, (int64_t)(long)(a.startMs - now));
        for (int j = 0; j < total; ++j)
        {
            ClockWindow b = plannedWindow(j, incoming, count);
            if (i != j && windowsConflict(a, b, now))
            {
                commandOutput->println(F("ERROR: overlapping clock writes to the same pin"));
                return false;
            }
            if (windowContains(b, offset, now)) ++concurrent;
        }
        if (concurrent > 8)
        {
            commandOutput->println(F("ERROR: schedule exceeds 8 concurrent clocks"));
            return false;
        }
    }
    return true;
}

static bool prepareClockJob(const String &line, Job &job)
{
    int separators = 0;
    for (unsigned int i = 0; i < line.length(); ++i)
        if (line[i] == '|') ++separators;
    String parts[10];
    int partCount = splitPipeline(line, parts, 10);
    if (partCount <= 0 || separators != partCount - 1 || partCount > 9)
    {
        commandOutput->println(F("ERROR: invalid or too long clock pipeline"));
        return false;
    }

    String sourceArgs = parts[0];
    String sourceName = nextToken(sourceArgs);
    sourceName.toLowerCase();
    if (sourceName != "clock")
    {
        commandOutput->println(F("ERROR: pipeline source must start with clock"));
        return false;
    }

    String frequencyToken = nextToken(sourceArgs);
    char *frequencyEnd;
    float frequency = strtof(frequencyToken.c_str(), &frequencyEnd);
    if (*frequencyEnd != '\0' || !isfinite(frequency) || frequencyToken.length() == 0 || frequency < 0.01f || frequency > 1000000.0f)
    {
        commandOutput->println(F("ERROR: clock frequency must be between 0.01 and 1000000 Hz"));
        return false;
    }
    unsigned long halfPeriod = max(1UL, (unsigned long)(500000.0f / frequency + 0.5f));
    int fallbackPin = -1;
    bool hasDuration = false;
    while (trim(sourceArgs).length())
    {
        String token = nextToken(sourceArgs);
        if (token == "--dur" && !hasDuration)
        {
            if (!parseMilliseconds(nextToken(sourceArgs), job.durationMs, false))
            {
                commandOutput->println(F("ERROR: --dur requires 1..2147483647 milliseconds"));
                return false;
            }
            hasDuration = true;
        }
        else if (fallbackPin < 0 && token != "--dur")
        {
            unsigned long pin;
            if (!parseMilliseconds(token, pin, true) || pin > 16)
            {
                commandOutput->println(F("ERROR: invalid clock argument"));
                return false;
            }
            fallbackPin = (int)pin;
        }
        else
        {
            commandOutput->println(F("ERROR: invalid clock argument"));
            return false;
        }
    }

    Stage parsedStages[8];
    int stageCount = 0;
    bool hasSink = false;

    for (int i = 1; i < partCount; ++i)
    {
        if (stageCount >= 8)
        {
            commandOutput->println(F("ERROR: pipeline too long"));
            return false;
        }
        Stage stage;
        if (!parsePipelineStage(parts[i], stage))
        {
            commandOutput->print(F("ERROR: unsupported pipeline stage '"));
            commandOutput->print(parts[i]);
            commandOutput->println(F("'"));
            return false;
        }
        if (stage.kind == STAGE_WRITE && stage.p1 > 16)
        {
            commandOutput->println(F("ERROR: invalid clock output pin"));
            return false;
        }
        if (stage.kind == STAGE_WRITE || stage.kind == STAGE_PRINT || stage.kind == STAGE_LCD_PRINT || stage.kind == STAGE_OLED)
        {
            hasSink = true;
        }
        parsedStages[stageCount++] = stage;
    }

    if (!hasSink && fallbackPin >= 0)
    {
        if (scriptOwnsPin(fallbackPin) || displayReservesPin(fallbackPin))
        {
            commandOutput->println(F("ERROR: GPIO4/GPIO5 are reserved for display I2C"));
            return false;
        }
        Stage writeStage;
        writeStage.kind = STAGE_WRITE;
        writeStage.p1 = fallbackPin;
        writeStage.p2 = -1;
        writeStage.p3 = -1;
        if (stageCount < 8)
        {
            parsedStages[stageCount++] = writeStage;
            hasSink = true;
        }
    }

    if (!hasSink)
    {
        if (stageCount == 8)
        {
            commandOutput->println(F("ERROR: pipeline too long"));
            return false;
        }
        Stage printStage;
        printStage.kind = STAGE_PRINT;
        parsedStages[stageCount++] = printStage;
    }

    job.active = true;
    job.interval = halfPeriod;
    job.clockState = 0;
    job.stageCount = stageCount;
    job.pipeline = trim(line);
    for (int s = 0; s < stageCount; ++s)
        job.stages[s] = parsedStages[s];
    return true;
}

static bool prepareBackgroundJob(const String &line, Job &job)
{
    String parts[10];
    int count = splitPipeline(line, parts, 10);
    String args = count ? parts[0] : String("");
    String name = nextToken(args);
    if (name.equalsIgnoreCase("clock")) return prepareClockJob(line, job);
    if (!name.equalsIgnoreCase("adc") || !nextToken(args).equalsIgnoreCase("A0") ||
        nextToken(args) != "--watch" || trim(args).length())
    {
        commandOutput->println(F("ERROR: expected adc A0 --watch"));
        return false;
    }
    // Reuse pipeline compilation, resource checks, and the eight job slots.
    String compiled = String("clock 5") + line.substring(parts[0].length());
    if (!prepareClockJob(compiled, job)) return false;
    job.pipeline = line;
    job.adcWatch = true;
    return true;
}

static bool startClockJob(const String &line)
{
    Job job;
    if (!prepareBackgroundJob(line, job)) return false;
    job.line = commandErrorLine;
    job.startMs = millis();
    ScheduledCommand proposed;
    proposed.pipeline = job.pipeline;
    proposed.isClock = true;
    proposed.startMs = job.startMs;
    proposed.durationMs = job.durationMs;
    proposed.pinMask = clockWindow(job).pinMask;
    if (!validatePlan(&proposed, 1, job.startMs)) return false;
    for (int i = 0; i < 8; ++i)
        if (!jobs[i].active)
        {
            job.id = nextJobId++;
            jobs[i] = job;
            commandOutput->print(F("Started job "));
            commandOutput->println(job.id);
            return true;
        }
    commandOutput->println(F("ERROR: no free jobs"));
    return false;
}

static bool startSchedule(String text)
{
    text = trim(text);
    if (!text.startsWith("{") || !text.endsWith("}"))
    {
        commandOutput->println(F("ERROR: schedule requires { START_MS { COMMAND } ... }"));
        return false;
    }
    text = text.substring(1, text.length() - 1);
    uint16_t sourceLine = commandErrorLine;
    // Store only command text and timing for future notes; compiled stages
    // occupy the eight execution slots only while their clocks are running.
    ScheduledCommand *queued = new (std::nothrow) ScheduledCommand[MAX_SCHEDULED_COMMANDS];
    if (!queued)
    {
        commandOutput->println(F("ERROR: not enough memory for schedule"));
        return false;
    }
    int count = 0;
    bool valid = true;
    while (text.length() && valid)
    {
        unsigned int leading = 0;
        while (leading < text.length() && isspace((unsigned char)text[leading]))
            if (text[leading++] == '\n') ++sourceLine;
        text = text.substring(leading);
        if (!text.length()) break;
        commandErrorLine = sourceLine;
        int open = unquotedIndex(text, '{'), close = unquotedIndex(text, '}');
        unsigned long offset;
        if (count == MAX_SCHEDULED_COMMANDS || open < 0 || close < open ||
            !parseMilliseconds(trim(text.substring(0, open)), offset, true))
        { valid = false; break; }
        String pipeline = trim(text.substring(open + 1, close));
        uint16_t pipelineLine = sourceLine;
        unsigned int commandStart = open + 1;
        while (commandStart < (unsigned int)close && isspace((unsigned char)text[commandStart])) ++commandStart;
        for (unsigned int i = 0; i < commandStart; ++i) if (text[i] == '\n') ++pipelineLine;
        commandErrorLine = pipelineLine;
        queued[count].line = pipelineLine;
        if (!pipeline.length() || unquotedIndex(pipeline, '{') >= 0)
        { valid = false; break; }
        String args = pipeline;
        String name = nextToken(args);
        name.toLowerCase();
        if (name == "clock" || (name == "adc" && args.indexOf("--watch") >= 0))
        {
            Job parsed;
            if (!prepareBackgroundJob(pipeline, parsed)) { valid = false; break; }
            queued[count].isClock = true;
            queued[count].durationMs = parsed.durationMs;
            queued[count].pinMask = clockWindow(parsed).pinMask;
        }
        else
        {
            // Immediate commands run once when their deadline arrives.
            const String allowed = " read write adc ohm lcd oled map not edge rising falling debounce toggle divide pulse print shiftin shiftout status pins help ";
            if (allowed.indexOf(String(" ") + name + " ") < 0)
            { valid = false; break; }
            String parts[10];
            int partCount = splitPipeline(pipeline, parts, 10);
            int separators = 0;
            for (int i = unquotedIndex(pipeline, '|'); i >= 0; i = unquotedIndex(pipeline, '|', i + 1))
                ++separators;
            if (partCount > 9 || separators != partCount - 1)
            { valid = false; break; }
            for (int i = 1; i < partCount; ++i)
            {
                Stage stage;
                if (!parsePipelineStage(parts[i], stage)) valid = false;
            }
            if (!valid) break;
            queued[count].isClock = false;
        }
        queued[count].pipeline = pipeline;
        if (queued[count].durationMs > 2147483647UL - offset)
        { valid = false; break; }
        queued[count++].startMs = offset;
        for (int i = 0; i <= close; ++i) if (text[i] == '\n') ++sourceLine;
        text = text.substring(close + 1);
    }
    int freeCount = 0;
    for (int i = 0; i < MAX_SCHEDULED_COMMANDS; ++i) if (!scheduled[i].id) ++freeCount;
    if (!valid || !count || count > freeCount)
    {
        delete[] queued;
        commandOutput->println(F("ERROR: invalid schedule or queue full (maximum 32 pending entries)"));
        return false;
    }
    unsigned long base = millis();
    for (int i = 0; i < count; ++i) queued[i].startMs += base;
    if (!validatePlan(queued, count, base))
    {
        delete[] queued;
        return false;
    }
    int entry = 0;
    for (int i = 0; i < MAX_SCHEDULED_COMMANDS && entry < count; ++i)
        if (!scheduled[i].id)
        {
            queued[entry].id = nextJobId++;
            scheduled[i] = queued[entry++];
            commandOutput->print(F("Scheduled job "));
            commandOutput->println(scheduled[i].id);
        }
    delete[] queued;
    return true;
}

static void executeJobs()
{
    unsigned long now = millis();
    // Finish every expired note before starting any note at the same boundary.
    for (int i = 0; i < 8; ++i)
        if (jobs[i].active && jobs[i].durationMs &&
            (long)(now - (jobs[i].startMs + jobs[i].durationMs)) >= 0)
            stopJob(jobs[i]);
    // Start due entries in timestamp order, even if the block was unordered.
    for (int n = 0; n < MAX_SCHEDULED_COMMANDS; ++n)
    {
        int first = -1;
        for (int i = 0; i < MAX_SCHEDULED_COMMANDS; ++i)
            if (scheduled[i].id && (long)(now - scheduled[i].startMs) >= 0 &&
                (first < 0 || (long)(scheduled[i].startMs - scheduled[first].startMs) < 0))
                first = i;
        if (first < 0) break;
        ScheduledCommand &entry = scheduled[first];
        commandErrorLine = entry.line;
        if (entry.durationMs && (long)(now - (entry.startMs + entry.durationMs)) >= 0)
        {
            entry = ScheduledCommand(); // Skip a note whose entire window was missed.
            continue;
        }
        if (!entry.isClock)
        {
            String command = entry.pipeline;
            uint16_t sourceLine = entry.line;
            entry = ScheduledCommand();
            handleCommand(command, sourceLine);
            continue;
        }
        int slot = -1;
        for (int i = 0; i < 8; ++i) if (!jobs[i].active) { slot = i; break; }
        if (slot < 0) break; // Preflight reserves capacity; retry if the loop ran late.
        Job parsed;
        if (prepareBackgroundJob(entry.pipeline, parsed))
        {
            parsed.id = entry.id;
            parsed.startMs = entry.startMs;
            parsed.line = entry.line;
            jobs[slot] = parsed;
        }
        entry = ScheduledCommand();
    }
    for (int i = 0; i < 8; ++i)
    {
        Job &job = jobs[i];
        commandErrorLine = job.line;
        if (!job.active || (long)(now - job.startMs) < 0) continue;
        if (!job.isClock)
        {
            String command = job.pipeline;
            stopJob(job);
            handleCommand(command, job.line);
            continue;
        }
        if (!job.started)
        {
            job.started = true;
            for (int s = 0; s < job.stageCount; ++s)
                if (job.stages[s].kind == STAGE_WRITE)
                {
                    pinMode(job.stages[s].p1, OUTPUT);
                    digitalWrite(job.stages[s].p1, LOW);
                }
            job.next = micros() + (job.adcWatch ? 0 : job.interval);
        }
        if ((long)(micros() - job.next) >= 0)
        {
            job.clockState = 1 - job.clockState;
            int value = job.adcWatch ? analogRead(A0) : job.clockState;
            if (job.adcWatch)
            {
                job.next = micros() + job.interval;
                if (job.hasADC && job.lastADC == value) continue;
                job.hasADC = true;
                job.lastADC = value;
            }
            bool hasValue = true;
            for (int s = 0; s < job.stageCount; ++s)
                if (!applySignalStage(job.stages[s], hasValue, value))
                { stopJob(job); break; }
            if (job.active && !job.adcWatch) job.next += job.interval;
        }
    }
}

static void serviceJobs()
{
    ErrorOutputScope output;
    executeJobs();
}

static bool scriptClaimPin(int pin, unsigned long owner)
{
    // UART and flash pins are unavailable to script hardware commands.
    if (pin < 0 || pin > 16 || pin == 1 || pin == 3 || (pin >= 6 && pin <= 11)) return false;
    if (displayReservesPin(pin)) return false;
    auto existing = scriptPinOwners.find(pin);
    if (existing != scriptPinOwners.end()) return existing->second == owner;
    unsigned long mask = 1UL << pin;
    for (const auto &job : jobs)
        if (job.active && (clockWindow(job).pinMask & mask)) return false;
    for (const auto &entry : scheduled)
        if (entry.id && (entry.pinMask & mask)) return false;
    if (pendingWritePulse.active && pendingWritePulse.pin == pin) return false;
    scriptPinOwners[pin] = owner;
    return true;
}

static void configureScriptRuntime()
{
    oledDisplay.begin = [](int address, std::string &error) {
        if (lcdReady && address == lcdAddress) { error = "oled: address is already used by LCD"; return false; }
        if (scriptOwnsPin(4) || scriptOwnsPin(5)) { error = "oled: I2C pins are owned by a script job"; return false; }
        for (const auto &job : jobs) if (job.active)
            for (int i = 0; i < job.stageCount; ++i)
                if (job.stages[i].kind == STAGE_WRITE && (job.stages[i].p1 == 4 || job.stages[i].p1 == 5))
                { error = "oled: I2C pins are used by a running job"; return false; }
        for (const auto &entry : scheduled) if (entry.id && (entry.pinMask & LCD_PIN_MASK))
            { error = "oled: I2C pins are reserved by a queued job"; return false; }
        if (pendingWritePulse.active && (pendingWritePulse.pin == 4 || pendingWritePulse.pin == 5))
            { error = "oled: I2C pins are used by a pulse"; return false; }
        Wire.begin(4, 5);
        Wire.setClock(lcdReady ? 100000 : 400000);
        return true;
    };
    oledDisplay.send = [](uint8_t address, uint8_t control, const uint8_t *bytes, size_t count) {
        Wire.beginTransmission(address); Wire.write(control);
        for (size_t i = 0; i < count; ++i) if (Wire.write(bytes[i]) != 1) { Wire.endTransmission(); return false; }
        return Wire.endTransmission() == 0;
    };
    scriptRuntime.background = [](const std::string &text) {
        ErrorOutputScope output(scriptRuntime.executingLine);
        return startClockJob(String(text.c_str()));
    };
    scriptRuntime.schedule = [](const std::string &text) {
        ErrorOutputScope output(scriptRuntime.executingLine);
        return startSchedule(String(text.c_str()));
    };
    scriptRuntime.newId = []() { return nextJobId++; };
    scriptRuntime.output = [](const std::string &s) { commandOutput->println(s.c_str()); };
    scriptRuntime.claim = scriptClaimPin;
    scriptRuntime.write = [](int pin, int value) {
        scriptOutputPins[pin] = true;
        pinMode(pin, OUTPUT); digitalWrite(pin, value ? HIGH : LOW);
    };
    scriptRuntime.release = [](unsigned long id) {
        for (auto it = scriptPinOwners.begin(); it != scriptPinOwners.end();)
            if (it->second == id) {
                if (scriptOutputPins.count(it->first)) {
                    pinMode(it->first, OUTPUT); digitalWrite(it->first, LOW);
                    scriptOutputPins.erase(it->first);
                }
                it = scriptPinOwners.erase(it);
            } else ++it;
    };
    scriptRuntime.host = [](const std::string &name, const std::vector<cw::Value> &supplied, cw::Event &event,
                            unsigned long id, std::string &error) {
        ErrorOutputScope output(scriptRuntime.executingLine);
        if (name == "oled") return oledDisplay.command(supplied, event, scriptRuntime.executingLine, (uint32_t)millis(), error);
        auto args = supplied;
        cw::Value input = event.value;
        for (size_t i = 0; i < args.size(); ++i) if (args[i].str() == "--value") {
            if (i + 2 != args.size()) { error = "--value requires one value at end"; return false; }
            input = args[i+1]; args.resize(i); break;
        }
        auto number = [&](const cw::Value &v) {
            if (v.kind != cw::Value::NUMBER || !std::isfinite(v.number) || v.number < -2147483647.0 || v.number > 2147483647.0) {
                error = "command requires numeric input"; return 0;
            }
            return (int)v.number;
        };
        auto pin = [&](const cw::Value &v) {
            int p = number(v);
            if (error.empty() && (!scriptClaimPin(p, id) || v.number != p)) error = "GPIO is invalid or reserved";
            return p;
        };
        if (name == "adc" && args.size() == 1 && cw::lower(args[0].str()) == "a0") {
            event.value = cw::Value(analogRead(A0)); return true;
        }
        if (name == "read" && args.size() == 1) {
            int p = pin(args[0]); if (!error.empty()) return false;
            pinMode(p, INPUT); event.value = cw::Value(digitalRead(p)); return true;
        }
        if (name == "write" && (args.size() == 1 || args.size() == 2)) {
            int p = pin(args[0]), value = number(args.size() == 2 ? args[1] : input);
            if (!error.empty()) return false;
            scriptOutputPins[p] = true;
            pinMode(p, OUTPUT); digitalWrite(p, value ? HIGH : LOW); event.value = cw::Value(value != 0); return true;
        }
        if (name == "map" && args.size() == 4) {
            int a = number(args[0]), b = number(args[1]), c = number(args[2]), d = number(args[3]), v = number(input);
            if (!error.empty()) return false;
            if (a == b) { error = "map input range cannot be zero"; return false; }
            event.value = cw::Value(c + (double(v) - a) * (double(d) - c) / (double(b) - a));
            event.value.number = std::trunc(event.value.number); return true;
        }
        if (name == "not" && args.empty()) {
            event.value = cw::Value(number(input) == 0); return error.empty();
        }
        if (name == "print" && args.size() <= 1) {
            commandOutput->println((args.empty() ? input.str() : args[0].str()).c_str()); return true;
        }
        if (name == "lcd" && !args.empty()) {
            std::string action = cw::lower(args[0].str());
            if (action == "init" && args.size() <= 2) {
                int address = args.size() == 2 ? number(args[1]) : 0x27;
                if (address < 8 || address > 0x77) error = "invalid LCD I2C address";
                if (error.empty() && initLCD((uint8_t)address)) return true;
            } else if ((action == "print" && args.size() <= 2) || (action == "clear" && args.size() == 1)) {
                std::string text = action == "clear" ? "" : args.size() == 2 ? args[1].str() : input.str();
                if (printLCD(String(text.c_str()))) return true;
            }
            if (error.empty()) error = "LCD command failed";
            return false;
        }
        if (name == "ohm" && args.empty()) {
            // Match the numeric portion of the existing calibrated conversion.
            int raw = number(input);
            if (!error.empty()) return false;
            if (raw < 0 || raw > 1023) event.value = cw::Value(std::string("OUT_OF_RANGE"));
            else if (raw < 30) event.value = cw::Value(std::string("<270"));
            else if (raw > 720) event.value = cw::Value(std::string(">700k"));
            else {
                const int counts[] = {50,179,291,459,557,628,703,720};
                const double resistance[] = {270,995,1992,5152,9959,20020,108800,688900};
                int index = 0; while (index < 6 && raw > counts[index+1]) ++index;
                double fraction = double(raw-counts[index]) / (counts[index+1]-counts[index]);
                event.value = cw::Value(std::floor(resistance[index] * exp(fraction * log(resistance[index+1]/resistance[index])) + 0.5));
            }
            return true;
        }
        error = "invalid arguments for '" + name + "'"; return false;
    };
}

static void executeCommand(const String &line)
{
    String commandLine = trim(line);
    if (commandLine.length() == 0)
    {
        return;
    }

    String scheduleArgs = commandLine;
    String scheduleName = nextToken(scheduleArgs);
    if (scheduleName.equalsIgnoreCase("schedule") && commandLine.indexOf('$') < 0)
    {
        int brace = unquotedIndex(line, '{');
        for (int i = 0; i < brace; ++i) if (line[i] == '\n') ++commandErrorLine;
        startSchedule(scheduleArgs);
        return;
    }

    std::string first = cw::lower(scheduleName.c_str());
    const std::string scriptCommands = " do func call var set every each split sleep tone oled ";
    bool scriptPipeline = scriptCommands.find(" " + first + " ") != std::string::npos;
    for (int separator = unquotedIndex(commandLine, '|'); !scriptPipeline && separator >= 0;
         separator = unquotedIndex(commandLine, '|', separator + 1))
    {
        String remaining = commandLine.substring(separator + 1);
        std::string name = cw::lower(nextToken(remaining).c_str());
        scriptPipeline = !name.empty() && scriptCommands.find(" " + name + " ") != std::string::npos;
    }
    if (scriptPipeline ||
        commandLine.indexOf('$') >= 0 || commandLine.indexOf("--out") >= 0 ||
        line.indexOf('\n') >= 0 || commandLine.indexOf(';') >= 0 || commandLine.startsWith("#"))
    {
        configureScriptRuntime();
        std::string source(commandErrorLine - 1, '\n');
        source += line.c_str();
        if (!scriptRuntime.submit(source, (uint32_t)millis(), (uint32_t)micros()))
        {
            commandOutput->print(F("ERROR: "));
            commandOutput->println(scriptRuntime.error.c_str());
        }
        return;
    }

    String backgroundArgs = commandLine;
    String backgroundName = nextToken(backgroundArgs);
    if (backgroundName.equalsIgnoreCase("adc") && backgroundArgs.indexOf("--watch") >= 0)
    {
        startClockJob(commandLine);
        return;
    }

    if (unquotedIndex(commandLine, '|') >= 0)
    {
        String source = commandLine;
        String sourceName = nextToken(source);
        sourceName.toLowerCase();
        if (sourceName == "clock")
        {
            startClockJob(commandLine);
        }
        else
        {
            runImmediatePipeline(commandLine);
        }
        return;
    }

    String command = nextToken(commandLine);
    command.toLowerCase();
    if (command == "read" || command == "write" || command == "shiftin" || command == "shiftout")
    {
        String pins = commandLine;
        int count = (command == "read" || command == "write") ? 1 : 3;
        for (int i = 0; i < count; ++i)
            if (scriptOwnsPin(toIntDef(nextToken(pins), -1)))
            {
                commandOutput->println(F("ERROR: GPIO is owned by a script job"));
                return;
            }
    }
    if ((lcdReady || oledDisplay.reserved) && (command == "read" || command == "write" || command == "shiftin" || command == "shiftout"))
    {
        String pins = commandLine;
        int count = (command == "read" || command == "write") ? 1 : 3;
        for (int i = 0; i < count; ++i)
        {
            int pin = toIntDef(nextToken(pins), -1);
            if (pin == 4 || pin == 5)
            {
                commandOutput->println(F("ERROR: GPIO4/GPIO5 are reserved for display I2C"));
                return;
            }
        }
    }
    if (command == "lcd")
    {
        handleLCDCommand(commandLine);
        return;
    }
    if (command == "help")
    {
        printHelp();
        return;
    }
    if (command == "pins")
    {
        printPins();
        return;
    }
    if (command == "jobs")
    {
        printJobs();
        return;
    }
    if (command == "kill")
    {
        handleKillCommand(commandLine);
        return;
    }
    if (command == "reset")
    {
        scriptRuntime.reset();
        for (int i = 0; i < 8; ++i)
        {
            stopJob(jobs[i]);
        }
        for (int i = 0; i < MAX_SCHEDULED_COMMANDS; ++i)
            scheduled[i] = ScheduledCommand();
        if (pendingWritePulse.active)
        {
            digitalWrite(pendingWritePulse.pin, LOW);
            pendingWritePulse.active = false;
        }
        commandOutput->println(F("reset"));
        return;
    }
    if (command == "read")
    {
        handleReadCommand(commandLine);
        return;
    }
    if (command == "write")
    {
        handleWriteCommand(commandLine);
        return;
    }
    if (command == "adc")
    {
        handleADCCommand(commandLine);
        return;
    }
    if (command == "ohm")
    {
        String pin = nextToken(commandLine);
        if ((pin.length() && !pin.equalsIgnoreCase("A0")) || nextToken(commandLine).length())
        {
            commandOutput->println(F("ERROR: ohm accepts only A0"));
            return;
        }
        printOhmReading(analogRead(A0));
        return;
    }
    if (command == "clock")
    {
        startClockJob(line);
        return;
    }
    if (command == "map")
    {
        handleMapCommand(commandLine);
        return;
    }
    if (command == "not")
    {
        handleNotCommand(commandLine);
        return;
    }
    if (command == "edge")
    {
        handleEdgeCommand(commandLine);
        return;
    }
    if (command == "rising")
    {
        handleRisingCommand(commandLine);
        return;
    }
    if (command == "falling")
    {
        handleFallingCommand(commandLine);
        return;
    }
    if (command == "debounce")
    {
        handleDebounceCommand(commandLine);
        return;
    }
    if (command == "toggle")
    {
        handleToggleCommand(commandLine);
        return;
    }
    if (command == "divide")
    {
        handleDivideCommand(commandLine);
        return;
    }
    if (command == "pulse")
    {
        handlePulseCommand(commandLine);
        return;
    }
    if (command == "print")
    {
        handlePrintCommand(commandLine);
        return;
    }
    if (command == "shiftin")
    {
        handleShiftinCommand(commandLine);
        return;
    }
    if (command == "shiftout")
    {
        handleShiftoutCommand(commandLine);
        return;
    }
    if (command == "status")
    {
        commandOutput->println(F("ESP8266 Commandware ready"));
        commandOutput->print(F("Uptime ms: "));
        commandOutput->println(millis());
        commandOutput->print(F("Free heap: "));
        commandOutput->println(ESP.getFreeHeap());
        commandOutput->print(F("Wi-Fi status: "));
        commandOutput->println((int)WiFi.status());
        commandOutput->print(F("IP: "));
        commandOutput->println(WiFi.localIP());
        return;
    }

    commandOutput->print(F("ERROR: unknown command '"));
    commandOutput->print(command);
    commandOutput->println(F("'"));
}

static void handleCommand(const String &line, uint16_t sourceLine)
{
    ErrorOutputScope output(sourceLine);
    executeCommand(line);
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("Commandware ESP-12E ready"));
    Serial.println(ESP.getResetReason());
    Serial.println(F("Type 'help' for commands"));

    configureWebServer();
    WiFi.mode(WIFI_STA);
    if (WIFI_SSID[0] != '\0')
    {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        lastWifiAttempt = millis();
        Serial.println(F("Connecting to Wi-Fi..."));
    }
    else
    {
        Serial.println(F("Set WIFI_SSID and WIFI_PASSWORD in the sketch to enable web access."));
    }

    // Do not manipulate boot-sensitive pins: GPIO0, GPIO2, GPIO15.
    // GPIO1 and GPIO3 are UART, and GPIO16 is safe but not always usable as output.
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);
    pinMode(5, OUTPUT);
    digitalWrite(5, LOW);
    pinMode(12, OUTPUT);
    digitalWrite(12, LOW);
    pinMode(13, OUTPUT);
    digitalWrite(13, LOW);
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW);
    pinMode(16, OUTPUT);
    digitalWrite(16, LOW);
    pinMode(A0, INPUT);
}

void loop()
{
    serviceWebServer();

    while (Serial.available())
    {
        char ch = Serial.read();
        if (ch == '\n' && inputAfterCR) { inputAfterCR = false; continue; }
        inputAfterCR = ch == '\r';
        if (ch == '\n' || ch == '\r')
        {
            inputComment = false;
            if (inputBuffer.length() > 0 && inputBraceDepth <= 0 && !trim(inputBuffer).endsWith("|"))
            {
                handleCommand(inputBuffer);
                inputBuffer = "";
                inputBraceDepth = 0;
                inputQuoted = inputEscaped = false;
                inputQuoteChar = 0;
                inputLine = 1;
            }
            else if (inputBuffer.length() > 0) { inputBuffer += '\n'; ++inputLine; }
        }
        else
        {
            if (inputComment) {} // Braces and quotes in comments are ordinary text.
            else if (!inputQuoted && ch == '#' && (inputBuffer.length() == 0 ||
                     isspace((unsigned char)inputBuffer[inputBuffer.length()-1]) ||
                     inputBuffer.endsWith("{") || inputBuffer.endsWith(";") || inputBuffer.endsWith("|"))) inputComment = true;
            else if (inputQuoted && inputEscaped) inputEscaped = false;
            else if (inputQuoted && ch == '\\') inputEscaped = true;
            else if (inputQuoted && ch == inputQuoteChar) { inputQuoted = false; inputQuoteChar = 0; }
            else if (!inputQuoted && (ch == '"' || ch == '\'')) { inputQuoted = true; inputQuoteChar = ch; }
            else if (!inputQuoted && ch == '{') ++inputBraceDepth;
            else if (!inputQuoted && ch == '}') --inputBraceDepth;
            inputBuffer += ch;
            if (inputBuffer.length() > 4096)
            {
                Serial.print(F("ERROR: command too long at line "));
                Serial.println(inputLine);
                inputBuffer = "";
                inputBraceDepth = 0;
                inputQuoted = inputEscaped = false;
                inputQuoteChar = 0; inputComment = false;
                inputLine = 1;
            }
        }
    }

    unsigned long now = millis();
    if (pendingWritePulse.active && now >= pendingWritePulse.endTime)
    {
        pinMode(pendingWritePulse.pin, OUTPUT);
        digitalWrite(pendingWritePulse.pin, LOW);
        pendingWritePulse.active = false;
        pendingWritePulse.pin = -1;
        pendingWritePulse.endTime = 0;
    }

    serviceJobs();
    scriptRuntime.tick((uint32_t)millis(), (uint32_t)micros());
    std::string oledError;
    if (!oledDisplay.service((uint32_t)millis(), oledError)) {
        commandOutput->print(F("ERROR: ")); commandOutput->println(oledError.c_str());
    }
    yield();
}
