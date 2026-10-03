#include <Arduino.h>

enum StageKind
{
    STAGE_NONE,
    STAGE_MAP,
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
    Serial.println(F("Commandware ESP-12E shell"));
    Serial.println(F("help"));
    Serial.println(F("pins"));
    Serial.println(F("jobs"));
    Serial.println(F("kill ID"));
    Serial.println(F("status"));
    Serial.println(F("reset"));
    Serial.println(F("read PIN"));
    Serial.println(F("write PIN VALUE"));
    Serial.println(F("adc A0"));
    Serial.println(F("clock MS [PIN]"));
    Serial.println(F("map IN_MIN IN_MAX OUT_MIN OUT_MAX VALUE"));
    Serial.println(F("not VALUE"));
    Serial.println(F("edge VALUE"));
    Serial.println(F("rising VALUE"));
    Serial.println(F("falling VALUE"));
    Serial.println(F("debounce COUNT VALUE"));
    Serial.println(F("toggle [VALUE]"));
    Serial.println(F("divide N VALUE"));
    Serial.println(F("pulse DURATION VALUE"));
    Serial.println(F("print VALUE"));
    Serial.println(F("shiftin DATA CLOCK LOAD [msb|lsb] [BITS]"));
    Serial.println(F("shiftout DATA CLOCK LATCH [msb|lsb] VALUE"));
}

static void printPins()
{
    const char *names[] = {
        "GPIO0", "GPIO1", "GPIO2", "GPIO3", "GPIO4", "GPIO5",
        "GPIO12", "GPIO13", "GPIO14", "GPIO15", "GPIO16", "A0"};
    const uint8_t pins[] = {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, A0};
    for (int i = 0; i < 12; ++i)
    {
        Serial.print(names[i]);
        Serial.print("\t");
        if (pins[i] == 1 || pins[i] == 3)
        {
            Serial.println(F("terminal"));
        }
        else
        {
            Serial.println(F("available"));
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
        Serial.print(i + 1);
        Serial.print(F(" RUNNING  "));
        Serial.println(jobs[i].pipeline);
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

static bool applySignalStage(Stage &stage, bool &hasValue, int &value)
{
    if (!hasValue)
    {
        return true;
    }

    switch (stage.kind)
    {
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
        pinMode(stage.p1, OUTPUT);
        digitalWrite(stage.p1, value ? HIGH : LOW);
        return true;
    case STAGE_PRINT:
        Serial.println(value);
        return true;
    default:
        return false;
    }
}

static bool parsePipelineStage(const String &stageText, Stage &stage)
{
    String args = stageText;
    String name = nextToken(args);

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
        Serial.println(F("ERROR: read requires PIN"));
        return;
    }
    pinMode(pin, INPUT);
    Serial.println(digitalRead(pin));
}

static void handleWriteCommand(String args)
{
    String token = nextToken(args);
    int pin = toIntDef(token, -1);
    String valueToken = nextToken(args);
    int value = toIntDef(valueToken, LOW);
    if (pin < 0)
    {
        Serial.println(F("ERROR: write requires PIN VALUE"));
        return;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, value ? HIGH : LOW);
    Serial.println(value ? 1 : 0);
}

static void handleADCCommand(String args)
{
    String token = nextToken(args);
    if (token.length() == 0 || token != "A0")
    {
        Serial.println(F("ERROR: adc requires A0"));
        return;
    }
    Serial.println(analogRead(A0));
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
    Serial.println(mapValue(current, inMin, inMax, outMin, outMax));
}

static void handleNotCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    Serial.println(current == 0 ? 1 : 0);
}

static void handleEdgeCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    if (!directEdgeState.hasPrev || current != directEdgeState.prev)
    {
        Serial.println(current);
    }
    else
    {
        Serial.println(0);
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
    Serial.println(output);
}

static void handleFallingCommand(String args)
{
    int current = toIntDef(nextToken(args), 0);
    int output = (directFallingState.hasPrev && current < directFallingState.prev) ? 1 : 0;
    directFallingState.prev = current;
    directFallingState.hasPrev = true;
    Serial.println(output);
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
        Serial.println(current);
    }
    else
    {
        Serial.println(0);
    }
}

static void handleToggleCommand(String args)
{
    (void)args;
    directToggleState.toggleState = (directToggleState.toggleState == 0) ? 1 : 0;
    Serial.println(directToggleState.toggleState);
}

static void handleDivideCommand(String args)
{
    int divisor = max(1, toIntDef(nextToken(args), 2));
    int current = toIntDef(nextToken(args), 0);
    directDivideState.divideCounter++;
    if ((directDivideState.divideCounter % divisor) == 0)
    {
        Serial.println(current);
    }
    else
    {
        Serial.println(0);
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
    Serial.println(output);
}

static void handlePrintCommand(String args)
{
    int value = toIntDef(nextToken(args), 0);
    Serial.println(value);
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
        Serial.println(F("ERROR: shiftin requires DATA CLOCK LOAD [msb|lsb] [BITS]"));
        return;
    }

    bool msb = orderToken.length() == 0 || orderToken == "msb";

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

    Serial.println((int)value);
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

    bool msb = orderToken.length() == 0 || orderToken == "msb";
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
    Serial.println(value);
}

static void handleKillCommand(String args)
{
    int id = toIntDef(nextToken(args), -1);
    if (id < 1 || id > 8)
    {
        Serial.println(F("ERROR: kill requires valid job id"));
        return;
    }
    if (!jobs[id - 1].active)
    {
        Serial.print(F("ERROR: unknown job "));
        Serial.println(id);
        return;
    }
    jobs[id - 1].active = false;
    jobs[id - 1].stageCount = 0;
    jobs[id - 1].pipeline = "";
    Serial.print(F("killed "));
    Serial.println(id);
}

static bool evaluateImmediateSource(const String &sourceText, bool &hasValue, int &value)
{
    String args = sourceText;
    String name = nextToken(args);

    if (name == "read")
    {
        int pin = toIntDef(nextToken(args), -1);
        if (pin < 0)
        {
            Serial.println(F("ERROR: read requires PIN"));
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
        if (pin != "A0")
        {
            Serial.println(F("ERROR: adc requires A0"));
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
            Serial.println(F("ERROR: write requires PIN VALUE"));
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

    Serial.print(F("ERROR: unsupported pipeline source '"));
    Serial.print(name);
    Serial.println(F("'"));
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
            Serial.print(F("ERROR: unsupported pipeline stage '"));
            Serial.print(parts[i]);
            Serial.println(F("'"));
            return;
        }
        if (stage.kind == STAGE_WRITE || stage.kind == STAGE_PRINT)
        {
            hasSink = true;
        }
        if (!applySignalStage(stage, hasValue, value))
        {
            Serial.println(F("ERROR: pipeline stage failure"));
            return;
        }
    }

    if (hasValue && !hasSink)
    {
        Serial.println(value);
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
    if (sourceName != "clock")
    {
        Serial.println(F("ERROR: pipeline source must start with clock"));
        return;
    }

    int interval = max(1, toIntDef(nextToken(sourceArgs), 100));
    int fallbackPin = toIntDef(nextToken(sourceArgs), -1);

    Stage parsedStages[8];
    int stageCount = 0;
    bool hasSink = false;

    for (int i = 1; i < partCount; ++i)
    {
        if (stageCount >= 8)
        {
            Serial.println(F("ERROR: pipeline too long"));
            return;
        }
        Stage stage;
        if (!parsePipelineStage(parts[i], stage))
        {
            Serial.print(F("ERROR: unsupported pipeline stage '"));
            Serial.print(parts[i]);
            Serial.println(F("'"));
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
        Serial.println(F("ERROR: clock pipeline requires write PIN or print sink"));
        return;
    }

    for (int i = 0; i < 8; ++i)
    {
        if (!jobs[i].active)
        {
            jobs[i].active = true;
            jobs[i].interval = interval;
            jobs[i].next = millis() + jobs[i].interval;
            jobs[i].clockState = 0;
            jobs[i].stageCount = stageCount;
            jobs[i].pipeline = trim(line);
            for (int s = 0; s < stageCount; ++s)
            {
                jobs[i].stages[s] = parsedStages[s];
            }
            Serial.print(F("Started job "));
            Serial.println(i + 1);
            return;
        }
    }

    Serial.println(F("ERROR: no free jobs"));
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
        Serial.println(F("reset"));
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
        Serial.println(F("ESP8266 Commandware ready"));
        return;
    }

    Serial.print(F("ERROR: unknown command '"));
    Serial.print(command);
    Serial.println(F("'"));
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("Commandware ESP-12E ready"));
    Serial.println(F("Type 'help' for commands"));

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
    for (int i = 0; i < 8; ++i)
    {
        if (!jobs[i].active)
        {
            continue;
        }
        if (now >= jobs[i].next)
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
}
