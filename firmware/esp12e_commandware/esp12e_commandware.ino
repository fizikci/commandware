#include <Arduino.h>
#include <math.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

struct Stage;

static const char *WIFI_SSID = "KESKIN";
static const char *WIFI_PASSWORD = "";
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

static void handleCommand(const String &line);
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
    input, button { min-height: 3rem; border: 1px solid #34445d; border-radius: .7rem; font: inherit; }
    input { width: 100%; padding: 0 .8rem; background: #0b1424; color: #edf4ff; }
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
            <input id="command" name="command" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="e.g. read 4" required aria-autocomplete="list" aria-controls="command-suggestions" aria-expanded="false">
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
    const f=document.getElementById('command-form'),i=document.getElementById('command'),r=document.getElementById('result'),d=document.getElementById('command-suggestions'),a=document.getElementById('command-action'),j=document.getElementById('jobs-list'),p=['help','write 4 1','write 4 0','write 4 1 100','read 4','read 5','read 12','adc A0','ohm A0','status','clock 500 | write 5'];let h=[],busy=false;
    function suggestions(){const all=[...h,...p.filter(c=>!h.includes(c))],query=i.value.trim().toLowerCase(),matches=query?all.filter(c=>c.toLowerCase().includes(query)):all;d.replaceChildren(...matches.map(c=>{const b=document.createElement('button');b.type='button';b.textContent=c;b.setAttribute('role','option');b.onclick=()=>{i.value=c;closeSuggestions();i.focus()};return b}));if(!matches.length){const empty=document.createElement('div');empty.textContent='No matching commands';empty.style.cssText='padding:.5rem;color:#93a4bc;font-size:.85rem';d.append(empty)}}
    function openSuggestions(){suggestions();d.hidden=false;a.textContent=i.value?'×':'▾';a.setAttribute('aria-label',i.value?'Clear command':'Show command suggestions');a.setAttribute('aria-expanded','true');i.setAttribute('aria-expanded','true')}
    function closeSuggestions(){d.hidden=true;a.textContent=i.value?'×':'▾';a.setAttribute('aria-label',i.value?'Clear command':'Show command suggestions');a.setAttribute('aria-expanded','false');i.setAttribute('aria-expanded','false')}
    function remember(c){h=[c,...h.filter(x=>x!==c)].slice(0,12);if(!d.hidden)suggestions()}
    let requestQueue=Promise.resolve();
    function send(c){const request=requestQueue.then(async()=>{const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),10000);try{const x=await fetch('/api/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({command:c}),signal:controller.signal}),t=await x.text();if(!x.ok)throw Error(t||x.statusText);return t}catch(e){if(e.name==='AbortError')throw Error('Request timed out. Check device connection; command may have executed.');throw e}finally{clearTimeout(timer)}});requestQueue=request.catch(()=>{});return request}
    async function refresh(){if(busy)return;busy=true;try{const t=await send('jobs'),a=t.split(/\r?\n/).map(s=>s.match(/^(\d+)\s+RUNNING\s+(.*)$/)).filter(Boolean);j.replaceChildren();if(!a.length){j.textContent='No jobs running.';return}a.forEach(m=>{const li=document.createElement('li'),b=document.createElement('button');li.append(document.createTextNode(`#${m[1]} ${m[2]} `));b.textContent='Kill';b.onclick=async()=>{b.disabled=true;try{r.textContent=await send(`kill ${m[1]}`);await refresh()}catch(e){r.textContent=`Request failed: ${e}`;b.disabled=false}};li.append(b);j.append(li)})}catch(e){j.textContent=`Unable to load jobs: ${e}`}finally{busy=false}}
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
                         webServer.send(400, "text/plain; charset=utf-8", "ERROR: command is required");
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
};

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
};

struct Job
{
    bool active = false;
    unsigned long interval = 0;
    unsigned long next = 0;
    int clockState = 0;
    String pipeline;
    Stage stages[8];
    int stageCount = 0;
};

static Job jobs[8];
static String inputBuffer;

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
    int idx = trimmed.indexOf(' ');
    String token;
    if (idx < 0)
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
    commandOutput->println(F("adc A0"));
    commandOutput->println(F("ohm [A0] (ohms, <270, >700k, OUT_OF_RANGE)"));
    commandOutput->println(F("adc A0 | ohm (terminal measurement stage)"));
    commandOutput->println(F("clock HZ [PIN]"));
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
        else
        {
            commandOutput->println(F("available"));
        }
    }
}

static void printJobs()
{
    for (int i = 0; i < 8; ++i)
    {
        if (!jobs[i].active)
        {
            continue;
        }
        commandOutput->print(i + 1);
        commandOutput->print(F(" RUNNING  "));
        commandOutput->println(jobs[i].pipeline);
    }
}

static int splitPipeline(const String &line, String parts[], int maxParts)
{
    int count = 0;
    int start = 0;
    while (start <= line.length() && count < maxParts)
    {
        int idx = line.indexOf('|', start);
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
    default:
        return false;
    }
}

static bool parsePipelineStage(const String &stageText, Stage &stage)
{
    String args = stageText;
    String name = nextToken(args);
    name.toLowerCase();

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
        if (pin < 0)
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
    if (token.length() == 0 || !token.equalsIgnoreCase("A0"))
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
    if (id < 1 || id > 8)
    {
        commandOutput->println(F("ERROR: kill requires valid job id"));
        return;
    }
    if (!jobs[id - 1].active)
    {
        commandOutput->print(F("ERROR: unknown job "));
        commandOutput->println(id);
        return;
    }
    jobs[id - 1].active = false;
    jobs[id - 1].stageCount = 0;
    jobs[id - 1].pipeline = "";
    commandOutput->print(F("killed "));
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
        if (!pin.equalsIgnoreCase("A0"))
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
        if (stage.kind == STAGE_WRITE || stage.kind == STAGE_PRINT)
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

static void startClockJob(const String &line)
{
    String parts[10];
    int partCount = splitPipeline(line, parts, 10);
    if (partCount <= 0)
    {
        return;
    }

    String sourceArgs = parts[0];
    String sourceName = nextToken(sourceArgs);
    sourceName.toLowerCase();
    if (sourceName != "clock")
    {
        commandOutput->println(F("ERROR: pipeline source must start with clock"));
        return;
    }

    String frequencyToken = nextToken(sourceArgs);
    float frequency = frequencyToken.toFloat();
    if (frequencyToken.length() == 0 || frequency < 0.01f || frequency > 1000000.0f)
    {
        commandOutput->println(F("ERROR: clock frequency must be between 0.01 and 1000000 Hz"));
        return;
    }
    unsigned long halfPeriod = max(1UL, (unsigned long)(500000.0f / frequency + 0.5f));
    int fallbackPin = toIntDef(nextToken(sourceArgs), -1);

    Stage parsedStages[8];
    int stageCount = 0;
    bool hasSink = false;

    for (int i = 1; i < partCount; ++i)
    {
        if (stageCount >= 8)
        {
            commandOutput->println(F("ERROR: pipeline too long"));
            return;
        }
        Stage stage;
        if (!parsePipelineStage(parts[i], stage))
        {
            commandOutput->print(F("ERROR: unsupported pipeline stage '"));
            commandOutput->print(parts[i]);
            commandOutput->println(F("'"));
            return;
        }
        if (stage.kind == STAGE_WRITE || stage.kind == STAGE_PRINT)
        {
            hasSink = true;
        }
        parsedStages[stageCount++] = stage;
    }

    if (!hasSink && fallbackPin >= 0)
    {
        Stage writeStage;
        writeStage.kind = STAGE_WRITE;
        writeStage.p1 = fallbackPin;
        if (stageCount < 8)
        {
            parsedStages[stageCount++] = writeStage;
            hasSink = true;
        }
    }

    if (!hasSink)
    {
        commandOutput->println(F("ERROR: clock pipeline requires write PIN or print sink"));
        return;
    }

    for (int i = 0; i < 8; ++i)
    {
        if (!jobs[i].active)
        {
            jobs[i].active = true;
            jobs[i].interval = halfPeriod;
            jobs[i].next = micros() + jobs[i].interval;
            jobs[i].clockState = 0;
            jobs[i].stageCount = stageCount;
            jobs[i].pipeline = trim(line);
            for (int s = 0; s < stageCount; ++s)
            {
                jobs[i].stages[s] = parsedStages[s];
            }
            commandOutput->print(F("Started job "));
            commandOutput->println(i + 1);
            return;
        }
    }

    commandOutput->println(F("ERROR: no free jobs"));
}

static void handleCommand(const String &line)
{
    String commandLine = trim(line);
    if (commandLine.length() == 0)
    {
        return;
    }

    if (commandLine.indexOf('|') >= 0)
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
        for (int i = 0; i < 8; ++i)
        {
            jobs[i].active = false;
            jobs[i].stageCount = 0;
            jobs[i].pipeline = "";
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
        if (ch == '\n' || ch == '\r')
        {
            if (inputBuffer.length() > 0)
            {
                handleCommand(inputBuffer);
                inputBuffer = "";
            }
        }
        else
        {
            inputBuffer += ch;
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

    for (int i = 0; i < 8; ++i)
    {
        if (!jobs[i].active)
        {
            continue;
        }
        if ((long)(micros() - jobs[i].next) >= 0)
        {
            jobs[i].clockState = jobs[i].clockState == 0 ? 1 : 0;
            int value = jobs[i].clockState;
            bool hasValue = true;

            for (int s = 0; s < jobs[i].stageCount; ++s)
            {
                if (!applySignalStage(jobs[i].stages[s], hasValue, value))
                {
                    Serial.println(F("ERROR: job stage failure"));
                    jobs[i].active = false;
                    break;
                }
            }

            jobs[i].next += jobs[i].interval;
        }
    }
    yield();
}
