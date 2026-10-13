#pragma once

// Portable, cooperative script interpreter. Hardware and transport are supplied
// by the host; this file is also compiled verbatim by the native firmware tests.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cw {
struct Value {
    enum Kind { NUMBER, TEXT, LIST } kind = NUMBER;
    double number = 0;
    std::string text;
    std::vector<Value> list;
    Value() = default;
    Value(double n) : number(n) {}
    Value(std::string s) : kind(TEXT), text(std::move(s)) {}
    std::string str() const {
        if (kind == TEXT) return text;
        if (kind == LIST) return "[list]";
        char buffer[48]; snprintf(buffer, sizeof(buffer), "%.12g", number);
        return buffer;
    }
};
struct Event {
    Value value;
    std::map<std::string, Value> fields;
    uint32_t timestamp = 0;
    bool hasValue = false;
};
struct Scope {
    std::map<std::string, Value> vars;
};
struct Token { std::string text; bool quoted = false; uint16_t line = 1; };
struct TokenWithoutLocation { std::string text; bool quoted; };
static_assert(sizeof(Token) == sizeof(TokenWithoutLocation), "token line number must fit existing padding");
inline std::string located(const std::string &message, uint16_t line) {
    return message.find(" at line ") == std::string::npos ? message + " at line " + std::to_string(line) : message;
}
inline bool expressionToken(const Token &t) {
    return !t.quoted && !t.text.empty() && t.text.compare(0, 2, "--") != 0 &&
        (std::string("$(!-+.").find(t.text[0]) != std::string::npos ||
         isdigit((unsigned char)t.text[0]) || t.text.find('(') != std::string::npos);
}
struct Stage;
using StagePtr = std::shared_ptr<Stage>;
using Pipe = std::vector<StagePtr>;
using Body = std::vector<Pipe>;
struct Stage { std::string name; std::vector<Token> args; Body body; uint16_t line = 1; };

inline bool identifier(const std::string &s) {
    if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (char c : s) if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return s != "value";
}
inline std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

class Parser {
    std::vector<Token> tokens;
    size_t pos = 0, nodes = 0;
    uint16_t line = 1, errorLine = 0;
    std::string &error;
    bool at(const char *s) const { return pos < tokens.size() && !tokens[pos].quoted && tokens[pos].text == s; }
    void separators() { while (at(";") || at("\n")) ++pos; }
    Body body(int depth, bool entries = false) {
        Body result;
        if (depth > 8) { error = "block nesting exceeds 8"; return result; }
        separators();
        while (pos < tokens.size() && !at("}") && error.empty()) {
            Pipe pipe;
            do {
                if (at("|")) { ++pos; while (at("\n")) ++pos; }
                if (pos == tokens.size() || at("}") || at("|") || at(";") || at("\n")) {
                    error = "expected command"; break;
                }
                auto stage = std::make_shared<Stage>();
                stage->line = tokens[pos].line;
                stage->name = entries ? tokens[pos++].text : lower(tokens[pos++].text);
                if (++nodes > 128) { error = "script exceeds 128 commands"; break; }
                while (pos < tokens.size() && !at("{") && !at("}") && !at("|") && !at(";") && !at("\n"))
                    stage->args.push_back(tokens[pos++]);
                if (at("{")) {
                    ++pos; stage->body = body(depth + 1, stage->name == "schedule");
                    if (!at("}")) { error = "missing closing brace"; break; }
                    ++pos;
                }
                pipe.push_back(stage);
            } while (at("|") && error.empty());
            result.push_back(pipe);
            if (pos < tokens.size() && !at("}") && !at(";") && !at("\n") && !entries) error = "expected statement separator";
            separators();
        }
        return result;
    }
public:
    Parser(std::string &e) : error(e) {}
    Body parse(const std::string &source) {
        if (source.size() > 4096) { error = "script exceeds 4096 bytes at line 1"; return {}; }
        for (size_t i = 0; i < source.size() && error.empty();) {
            uint16_t tokenLine = line;
            char c = source[i++];
            if (c == '\r' || c == ' ' || c == '\t') continue;
            if (c == '#') { while (i < source.size() && source[i] != '\n') ++i; continue; }
            if (c == '"' || c == '\'') {
                char quote = c; std::string s; bool closed = false;
                while (i < source.size()) {
                    c = source[i++];
                    if (c == '\n') ++line;
                    if (c == quote) { closed = true; break; }
                    if (c == '\\' && i < source.size()) {
                        c = source[i++]; if (c == '\n') ++line; if (c == 'n') c = '\n';
                    }
                    s += c;
                }
                if (!closed) { error = "unterminated string"; errorLine = tokenLine; }
                tokens.push_back({s, true, tokenLine});
            } else if (c == '{' || c == '}' || c == '|' || c == ';' || c == '\n') {
                tokens.push_back({std::string(1, c), false, tokenLine});
                if (c == '\n') ++line;
            } else {
                std::string s(1, c);
                while (i < source.size() && !isspace((unsigned char)source[i]) &&
                       std::string("{}|;\"'").find(source[i]) == std::string::npos) s += source[i++];
                tokens.push_back({s, false, tokenLine});
            }
        }
        Body result = body(0);
        if (pos != tokens.size() && error.empty()) error = "unexpected closing brace";
        if (!error.empty()) error = located(error, errorLine ? errorLine : pos < tokens.size() ? tokens[pos].line : line);
        return result;
    }
};

// Small expression grammar, never eval() or shell substitution.
class Expression {
    std::string source;
    size_t pos = 0;
    int depth = 0;
    bool syntaxOnly;
    Scope &scope; Event &event; std::string &error;
    void space() { while (pos < source.size() && isspace((unsigned char)source[pos])) ++pos; }
    double numeric(Value v) {
        if (syntaxOnly) return 1;
        if (v.kind == Value::NUMBER) return v.number;
        char *end = nullptr;
        std::string s = v.str(); double n = strtod(s.c_str(), &end);
        if (v.kind == Value::LIST || s.empty() || *end || !std::isfinite(n)) error = "expected numeric value";
        return n;
    }
    Value atom() {
        space();
        if (++depth > 16) { error = "expression nesting exceeds 16"; --depth; return {}; }
        Value result;
        if (pos == source.size()) error = "missing expression value";
        else if (source[pos] == '(') {
            ++pos; result = sum(); space();
            if (pos == source.size() || source[pos++] != ')') error = "missing expression closing parenthesis";
        } else if (source[pos] == '!') {
            ++pos; result = Value(numeric(atom()) == 0);
        } else if (source[pos] == '+' || source[pos] == '-') {
            bool minus = source[pos++] == '-'; result = Value(numeric(atom()) * (minus ? -1 : 1));
        } else if (source[pos] == '$') {
            ++pos; size_t start = pos;
            while (pos < source.size() && (isalnum((unsigned char)source[pos]) || source[pos] == '_')) ++pos;
            std::string name = source.substr(start, pos - start);
            if (name.empty()) error = "expected name after $";
            else if (syntaxOnly) result = Value(1);
            else if (name == "value") {
                if (!event.hasValue) error = "pipeline value is required";
                else result = event.value;
            }
            else {
                auto variable = scope.vars.find(name), field = event.fields.find(name);
                if (variable != scope.vars.end() && field != event.fields.end()) error = "ambiguous name '" + name + "'";
                else if (variable != scope.vars.end()) result = variable->second;
                else if (field != event.fields.end()) result = field->second;
                else error = "unknown name '" + name + "'";
            }
        } else if (isdigit((unsigned char)source[pos]) || source[pos] == '.') {
            char *end; double n = strtod(source.c_str() + pos, &end);
            if (end == source.c_str() + pos || !std::isfinite(n)) error = "invalid number";
            else { pos = (size_t)(end - source.c_str()); result = Value(n); }
        } else {
            size_t start = pos;
            while (pos < source.size() && (isalnum((unsigned char)source[pos]) || source[pos] == '_')) ++pos;
            std::string fn = source.substr(start, pos - start); space();
            if (fn.empty() || pos == source.size() || source[pos++] != '(') error = "invalid expression: read variable values with $name";
            else {
                Value arg = sum(); space();
                if (pos == source.size() || source[pos++] != ')') error = "missing function closing parenthesis";
                if (fn == "int") result = Value(std::trunc(numeric(arg)));
                else if (fn == "notehz" && syntaxOnly) result = Value(1);
                else if (fn == "notehz") {
                    std::string note = arg.str();
                    const std::string names = "C D EF G A B";
                    size_t semitone = note.empty() ? std::string::npos : names.find(note[0]);
                    size_t octavePos = 1; int accidental = 0;
                    if (note.size() > 1 && (note[1] == '#' || note[1] == 'b')) { accidental = note[1] == '#' ? 1 : -1; octavePos = 2; }
                    if (semitone == std::string::npos || octavePos + 1 != note.size() || note[octavePos] < '0' || note[octavePos] > '8') error = "invalid note";
                    else result = Value(440.0 * std::pow(2.0, ((note[octavePos] - '0' + 1) * 12 + (int)semitone + accidental - 69) / 12.0));
                } else error = "unknown expression function '" + fn + "'";
            }
        }
        --depth; return result;
    }
    Value product() {
        Value v = atom(); space();
        while (error.empty() && pos < source.size() && (source[pos] == '*' || source[pos] == '/' || source[pos] == '%')) {
            char op = source[pos++]; double left = numeric(v), right = numeric(atom());
            if ((op == '/' || op == '%') && right == 0) { error = "division by zero"; return {}; }
            v = Value(op == '*' ? left * right : op == '/' ? left / right : std::fmod(left, right)); space();
        }
        return v;
    }
    Value sum() {
        Value v = product(); space();
        while (error.empty() && pos < source.size() && (source[pos] == '+' || source[pos] == '-')) {
            char op = source[pos++]; double left = numeric(v), right = numeric(product());
            v = Value(op == '+' ? left + right : left - right); space();
        }
        return v;
    }
public:
    Expression(std::string s, Scope &sc, Event &ev, std::string &e, bool syntax = false) : source(std::move(s)), syntaxOnly(syntax), scope(sc), event(ev), error(e) {}
    Value run() {
        Value v = sum(); space();
        if (error.empty() && (pos != source.size() || (v.kind == Value::NUMBER && !std::isfinite(v.number)))) error = "invalid expression";
        return v;
    }
};

struct Iteration {
    StagePtr stage;
    Pipe tail;
    std::shared_ptr<Scope> scope;
    std::shared_ptr<Event> original;
    size_t index = 0;
    int depth = 0;
    std::string out;
};
struct Action {
    Pipe pipe;
    std::shared_ptr<Scope> scope;
    std::shared_ptr<Event> event;
    int depth = 0;
    std::shared_ptr<Iteration> iteration;
};
struct Function { std::vector<std::string> params; Body body; };
struct Task {
    unsigned long id = 0;
    std::string label;
    std::deque<Action> work;
    Pipe repeat;
    std::shared_ptr<Scope> scope;
    uint32_t interval = 0, next = 0, wake = 0, end = 0, edge = 0, halfPeriod = 0;
    int tonePin = -1, toneState = 0;
    bool waiting = false;
    uint16_t line = 1;
    std::map<const Stage *, int> states;
};
struct FailedJob { unsigned long id; std::string label, message; };

class Runtime {
public:
    // A host command may update the event's primary value, leaving fields intact.
    using Host = std::function<bool(const std::string &, const std::vector<Value> &, Event &, unsigned long, std::string &)>;
    Host host;
    std::function<bool(const std::string &)> schedule;
    std::function<bool(const std::string &)> background;
    std::function<bool(int, unsigned long)> claim;
    std::function<void(unsigned long)> release;
    std::function<void(int, int)> write;
    std::function<void(const std::string &)> output;
    std::function<unsigned long()> newId;
    std::vector<std::shared_ptr<Task>> tasks;
    std::deque<FailedJob> failures;
    std::map<std::string, Function> functions;
    std::shared_ptr<Scope> globals = std::make_shared<Scope>();
    std::string error;
    uint16_t executingLine = 1;
private:
    uint32_t now = 0, us = 0;
    uint16_t errorLine = 1;
    static bool due(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }
    Value eval(const Token &t, Scope &scope, Event &event) {
        if (t.quoted) {
            std::string s;
            for (size_t i = 0; i < t.text.size();) {
                if (t.text[i] != '$') { s += t.text[i++]; continue; }
                size_t start = i++;
                while (i < t.text.size() && (isalnum((unsigned char)t.text[i]) || t.text[i] == '_')) ++i;
                s += Expression(t.text.substr(start, i - start), scope, event, error).run().str();
            }
            return Value(s);
        }
        if (expressionToken(t))
            return Expression(t.text, scope, event, error).run();
        return Value(t.text);
    }
    uint32_t milliseconds(const Value &v) {
        if (v.kind != Value::NUMBER || v.number < 1 || v.number > 2147483647 || v.number != std::floor(v.number)) {
            error = "duration/interval requires 1..2147483647 integer milliseconds"; return 0;
        }
        return (uint32_t)v.number;
    }
    void push(Task &task, const Body &body, std::shared_ptr<Scope> scope, std::shared_ptr<Event> event, int depth, bool fresh = false) {
        for (auto it = body.rbegin(); it != body.rend(); ++it)
            task.work.push_front({*it, scope, fresh ? std::make_shared<Event>() : event, depth});
    }
    bool validate(const Body &body, std::map<std::string, Function> &defs) {
        for (const auto &pipe : body) for (const auto &s : pipe) {
            errorLine = s->line;
            const std::string &n = s->name;
            if (n == "schedule") {
                if (pipe.size() != 1 || !s->args.empty() || s->body.empty()) { error = "schedule requires entries"; return false; }
                for (const auto &entry : s->body) {
                    if (entry.size() != 1 || !entry[0]->args.empty() || entry[0]->body.size() != 1) { error = "invalid schedule entry"; return false; }
                    for (const auto &command : entry[0]->body[0])
                        if (!command->body.empty()) { error = "nested schedule bodies are unsupported"; return false; }
                }
                continue;
            }
            if (n == "func") {
                if (pipe.size() != 1 || s->args.empty() || s->body.empty() || !identifier(s->args[0].text)) { error = "func requires NAME [PARAM...] { BODY }"; return false; }
                Function f; f.body = s->body;
                for (size_t i = 1; i < s->args.size(); ++i) {
                    std::string p = s->args[i].text;
                    if (!identifier(p) || std::find(f.params.begin(), f.params.end(), p) != f.params.end()) { error = "invalid/duplicate function parameter"; return false; }
                    f.params.push_back(p);
                }
                if (defs.size() >= 16 && !defs.count(s->args[0].text)) { error = "function limit is 16"; return false; }
                defs[s->args[0].text] = f;
            } else if (n != "clock" && n != "do" && n != "every" && n != "each" && n != "var" && n != "set" && n != "call" && n != "split" &&
                       n != "sleep" && n != "tone" && n != "adc" && n != "read" && n != "write" && n != "map" && n != "print" &&
                       n != "toggle" && n != "not" && n != "lcd" && n != "oled" && n != "ohm") {
                error = "unsupported script command '" + n + "'"; return false;
            }
            if (n != "func" && n != "do" && n != "each" && !s->body.empty()) { error = "unexpected command body"; return false; }
            if (n == "do" && (!s->args.empty() || s->body.empty() || pipe.size() != 1)) { error = "do requires { BODY }"; return false; }
            if (n == "every" && (s != pipe.front() || s->args.size() != 1 || pipe.size() == 1)) { error = "every requires INTERVAL | COMMAND"; return false; }
            if ((n == "var" || n == "set") && (s->args.size() < 2 || !identifier(s->args[0].text))) { error = n + " requires NAME EXPR"; return false; }
            if (n == "var" || n == "set") {
                if (s->args.size() > 2 || expressionToken(s->args[1])) {
                    std::string text;
                    for (size_t i = 1; i < s->args.size(); ++i) { if (i > 1) text += ' '; text += s->args[i].text; }
                    Scope scope; Event event;
                    Expression(text, scope, event, error, true).run();
                    if (!error.empty()) return false;
                }
            } else {
                for (const auto &arg : s->args) if (expressionToken(arg)) {
                    Scope scope; Event event;
                    Expression(arg.text, scope, event, error, true).run();
                    if (!error.empty()) return false;
                }
            }
            if (n != "var" && n != "set" && n != "func" && n != "do" && n != "every") {
                auto args = s->args;
                for (size_t i = 0; i < args.size(); ++i) if (!args[i].quoted && args[i].text == "--out") {
                    if (i + 2 != args.size()) { error = "--out requires names at end of command"; return false; }
                    args.resize(i); break;
                }
                size_t count = args.size();
                bool valid = true;
                if (n == "each" || n == "toggle" || n == "not" || n == "ohm") valid = count == 0;
                if (n == "sleep" || n == "read") valid = count == 1;
                if (n == "adc") valid = count == 1 || (count == 2 && args[1].text == "--watch");
                if (n == "print") valid = count <= 1;
                if (n == "write" || n == "lcd") valid = count == 1 || count == 2;
                if (n == "oled") valid = count >= 1;
                if (n == "split") valid = count == 1 || (count == 2 && (args[1].quoted || args[1].text != "--value")) ||
                    (count == 3 && !args[1].quoted && args[1].text == "--value");
                if (n == "map") valid = count == 4 || (count == 6 && args[4].text == "--value");
                if (n == "tone") valid = count == 5 && args[1].text == "--hz" && args[3].text == "--dur";
                if (!valid) { error = "invalid arguments for '" + n + "'"; return false; }
            }
            if (!s->body.empty() && !validate(s->body, defs)) return false;
        }
        return true;
    }
    bool callsValid(const Body &body, const std::map<std::string, Function> &defs) {
        for (const auto &pipe : body) for (const auto &s : pipe) {
            errorLine = s->line;
            if (s->name == "schedule") continue;
            if (s->name == "call") {
                if (s->args.empty() || !defs.count(s->args[0].text)) { error = "unknown function"; return false; }
                size_t argc = s->args.size() - 1;
                if (argc >= 2 && s->args[s->args.size()-2].text == "--out") argc -= 2;
                if (argc != defs.at(s->args[0].text).params.size()) { error = "incorrect function argument count"; return false; }
            }
            if (!callsValid(s->body, defs)) return false;
        }
        return true;
    }
    bool bind(Event &ev, Scope &scope, const std::string &names) {
        std::vector<std::string> keys;
        size_t start = 0;
        do {
            size_t end = names.find(',', start);
            std::string key = names.substr(start, end == std::string::npos ? end : end - start);
            if (!identifier(key) || scope.vars.count(key) || ev.fields.count(key) || std::find(keys.begin(), keys.end(), key) != keys.end()) {
                error = "invalid or duplicate output name '" + key + "'"; return false;
            }
            keys.push_back(key);
            if (end == std::string::npos) break;
            start = end + 1;
        } while (true);
        if (ev.fields.size() + keys.size() > 16) { error = "event field limit is 16"; return false; }
        if (keys.size() > 1 && (ev.value.kind != Value::LIST || ev.value.list.size() != keys.size())) { error = "output count mismatch"; return false; }
        for (size_t i = 0; i < keys.size(); ++i) ev.fields[keys[i]] = keys.size() == 1 ? ev.value : ev.value.list[i];
        return true;
    }
    void step(Task &task) {
        Action action = task.work.front(); task.work.pop_front();
        if (action.iteration) {
            auto it = action.iteration;
            executingLine = task.line = it->stage->line;
            if (it->index == it->original->value.list.size()) return;
            auto item = std::make_shared<Event>();
            item->fields = it->original->fields; item->timestamp = it->original->timestamp;
            item->value = it->original->value.list[it->index++];
            item->hasValue = true;
            if (!it->out.empty() && !bind(*item, *it->scope, it->out)) return;
            task.work.push_front(action); // Resume only after this item's entire tail.
            if (it->stage->body.empty()) task.work.push_front({it->tail, it->scope, item, it->depth});
            else push(task, it->stage->body, it->scope, item, it->depth);
            return;
        }
        if (action.pipe.empty()) return;
        auto s = action.pipe.front(); action.pipe.erase(action.pipe.begin());
        executingLine = task.line = s->line;
        auto &scope = *action.scope; auto &event = *action.event;
        const auto &n = s->name;
        auto continuation = [&]() { if (!action.pipe.empty()) task.work.push_front(action); };
        if (n == "func") return;
        if (n == "schedule") {
            std::string text = "{";
            uint16_t textLine = s->line;
            auto advanceLine = [&](uint16_t target) {
                while (textLine < target) { text += '\n'; ++textLine; }
            };
            for (const auto &entry : s->body) {
                advanceLine(entry[0]->line);
                Value offset = eval({entry[0]->name, false}, scope, event);
                text += " " + offset.str() + " { ";
                bool first = true;
                for (const auto &command : entry[0]->body[0]) {
                    if (!first) text += " | "; first = false;
                    advanceLine(command->line);
                    text += command->name;
                    for (const auto &token : command->args) {
                        Value value = eval(token, scope, event); text += ' ';
                        if (token.quoted) {
                            text += '"';
                            for (char c : value.str()) {
                                if (c == '\n') text += "\\n";
                                else { if (c == '"' || c == '\\') text += '\\'; text += c; }
                            }
                            text += '"';
                        } else text += value.str();
                    }
                }
                text += " }";
            }
            text += " }";
            if (error.empty() && (!schedule || !schedule(text))) error = "schedule rejected";
            continuation(); return;
        }
        if (n == "do") {
            auto local = std::make_shared<Scope>(scope);
            push(task, s->body, local, action.event, action.depth, true); return;
        }
        if (n == "clock" || (n == "adc" && s->args.size() == 2 && s->args[1].text == "--watch")) {
            Pipe pipe = action.pipe; pipe.insert(pipe.begin(), s);
            std::string text; bool first = true;
            for (const auto &command : pipe) {
                if (!command->body.empty()) { error = "legacy background pipeline cannot contain blocks"; return; }
                if (!first) text += " | "; first = false;
                text += command->name;
                for (const auto &token : command->args) {
                    if (token.text == "--out") { error = "use every for named background readings"; return; }
                    auto value = eval(token, scope, event).str(); text += ' ';
                    if (token.quoted) {
                        text += '"';
                        for (char c : value) {
                            if (c == '\n') text += "\\n";
                            else { if (c == '"' || c == '\\') text += '\\'; text += c; }
                        }
                        text += '"';
                    } else text += value;
                }
            }
            if (error.empty() && (!background || !background(text))) error = "background pipeline rejected";
            return;
        }
        if (n == "var" || n == "set") {
            std::string key = s->args[0].text;
            if (event.fields.count(key) || (n == "set" && scope.vars.count(key) == 0)) { error = "invalid variable declaration/assignment '" + key + "'"; return; }
            Value value;
            if (s->args.size() == 2) value = eval(s->args[1], scope, event);
            else {
                std::string expression;
                for (size_t i = 1; i < s->args.size(); ++i) { if (i > 1) expression += ' '; expression += s->args[i].text; }
                value = Expression(expression, scope, event, error).run();
            }
            if (scope.vars.size() >= 32 && !scope.vars.count(key)) error = "variable limit is 32";
            if (error.empty()) scope.vars[key] = value;
            continuation(); return;
        }
        if (n == "every") {
            uint32_t interval = milliseconds(eval(s->args[0], scope, event));
            if (tasks.size() >= 8) error = "script job limit is 8";
            if (!error.empty()) return;
            auto child = std::make_shared<Task>(); child->id = newId(); child->label = "every " + std::to_string(interval);
            child->interval = interval; child->next = now; child->repeat = action.pipe; child->scope = action.scope;
            tasks.push_back(child); output("Started job " + std::to_string(child->id)); return;
        }
        std::vector<Token> args = s->args; std::string out;
        for (size_t i = 0; i < args.size(); ++i) if (!args[i].quoted && args[i].text == "--out") {
            if (i + 2 != args.size()) { error = "--out requires names at end of command"; return; }
            out = args[i+1].text; args.resize(i); break;
        }
        // A continuation binding runs after a function returns.
        if (n == "__bind") { bind(event, scope, args[0].text); continuation(); return; }
        if (n == "call") {
            if (action.depth >= 8) { error = "function call depth exceeds 8"; return; }
            auto fn = functions.find(args[0].text);
            if (fn == functions.end() || args.size() != fn->second.params.size() + 1) { error = "invalid function call"; return; }
            auto local = std::make_shared<Scope>();
            for (size_t i = 1; i < args.size(); ++i) {
                std::string key = fn->second.params[i-1];
                if (event.fields.count(key)) { error = "parameter conflicts with event field"; return; }
                local->vars[key] = eval(args[i], scope, event);
            }
            if (!out.empty()) {
                auto binding = std::make_shared<Stage>(); binding->name = "__bind"; binding->args.push_back({out, false});
                binding->line = s->line;
                action.pipe.insert(action.pipe.begin(), binding);
            }
            continuation(); push(task, fn->second.body, local, action.event, action.depth + 1); return;
        }
        if (n == "each") {
            if (!args.empty() || event.value.kind != Value::LIST) { error = "each requires a list and optional --out NAME"; return; }
            // Block form continues after all iterations. Pipe form consumes the
            // downstream tail once per element, including its pending work.
            if (!s->body.empty()) continuation();
            auto it = std::make_shared<Iteration>();
            it->stage = s; it->tail = action.pipe; it->scope = action.scope;
            it->original = action.event; it->depth = action.depth; it->out = out;
            Action next; next.iteration = it; task.work.push_front(next);
            return;
        }
        std::vector<Value> values;
        for (const auto &arg : args) values.push_back(eval(arg, scope, event));
        if (!error.empty()) return;
        if (n == "split") {
            if (values.size() == 1 && !event.hasValue) { error = "split: value is required through pipe or explicit parameter --value"; return; }
            std::string delim = values[0].str(), text = values.size() == 3 ? values[2].str() : values.size() == 2 ? values[1].str() : event.value.str();
            if (delim.empty()) { error = "split delimiter cannot be empty"; return; }
            Value list; list.kind = Value::LIST;
            size_t start = 0;
            do {
                size_t end = text.find(delim, start);
                list.list.push_back(Value(text.substr(start, end == std::string::npos ? end : end - start)));
                if (list.list.size() > 64) { error = "list limit is 64"; return; }
                if (end == std::string::npos) break;
                start = end + delim.size();
            } while (true);
            event.value = list;
            event.hasValue = true;
        } else if (n == "toggle") {
            if (!values.empty()) { error = "toggle takes no arguments"; return; }
            int &state = task.states[s.get()]; state = 1 - state; event.value = Value(state);
            event.hasValue = true;
        } else if (n == "sleep" || n == "tone") {
            uint32_t duration = 0;
            if (n == "sleep" && values.size() == 1) duration = milliseconds(values[0]);
            else if (n == "tone" && values.size() == 5 && values[1].str() == "--hz" && values[3].str() == "--dur") {
                if (values[0].kind != Value::NUMBER || values[0].number != std::floor(values[0].number) || values[0].number < 0 || values[0].number > 16 ||
                    values[2].kind != Value::NUMBER || values[2].number < 0.01 || values[2].number > 1000000) error = "invalid tone pin/frequency";
                duration = milliseconds(values[4]);
                if (error.empty()) {
                    int pin = (int)values[0].number;
                    if (!claim(pin, task.id)) error = "tone pin is reserved";
                    else {
                        task.tonePin = pin; task.toneState = 0; write(pin, 0);
                        task.halfPeriod = std::max((uint32_t)1, (uint32_t)(500000.0 / values[2].number + 0.5));
                        task.edge = us + task.halfPeriod;
                    }
                }
            } else error = "use sleep MS or tone PIN --hz HZ --dur MS";
            if (!error.empty()) return;
            task.waiting = true; task.wake = now + duration;
        } else {
            if (!host(n, values, event, task.id, error) && error.empty()) error = "command failed";
            if (error.empty() && (n == "adc" || n == "read" || n == "write" || n == "map" || n == "not" || n == "ohm")) event.hasValue = true;
        }
        if (!out.empty() && error.empty()) bind(event, scope, out);
        if (error.empty()) continuation();
    }
    void finish(const std::shared_ptr<Task> &task) {
        if (task->tonePin >= 0) write(task->tonePin, 0);
        release(task->id);
        tasks.erase(std::remove(tasks.begin(), tasks.end(), task), tasks.end());
    }
public:
    bool submit(const std::string &source, uint32_t ms, uint32_t micros) {
        error.clear(); errorLine = 1; Parser parser(error); Body body = parser.parse(source);
        auto defs = functions;
        if (!error.empty() || !validate(body, defs) || !callsValid(body, defs)) { error = located(error, errorLine); return false; }
        // Definitions alone take no job slot and produce no hardware effects.
        bool runnable = false;
        for (const auto &p : body) if (!p.empty() && p[0]->name != "func") runnable = true;
        if (runnable && tasks.size() >= 8) { error = located("script job limit is 8", body.front().front()->line); return false; }
        functions = defs;
        if (!runnable) { output("OK"); return true; }
        auto task = std::make_shared<Task>(); task->id = newId(); task->label = "script";
        push(*task, body, globals, std::make_shared<Event>(), 0, true);
        tasks.push_back(task); output("Started job " + std::to_string(task->id));
        tick(ms, micros); return true;
    }
    void tick(uint32_t ms, uint32_t micros) {
        now = ms; us = micros;
        // Snapshot: newly launched jobs start on the next loop, avoiding iterator
        // invalidation and bounding work even when scripts launch more jobs.
        auto snapshot = tasks;
        for (const auto &task : snapshot) {
            if (std::find(tasks.begin(), tasks.end(), task) == tasks.end()) continue;
            error.clear();
            if (task->waiting) {
                if (!due(now, task->wake)) {
                    if (task->tonePin >= 0 && due(us, task->edge)) {
                        task->toneState = 1 - task->toneState; write(task->tonePin, task->toneState);
                        task->edge = us + task->halfPeriod;
                    }
                    continue;
                }
                task->waiting = false;
                if (task->tonePin >= 0) { write(task->tonePin, 0); task->tonePin = -1; }
            }
            if (task->work.empty() && task->interval && due(now, task->next)) {
                auto event = std::make_shared<Event>(); event->timestamp = now; event->hasValue = true;
                task->work.push_back({task->repeat, task->scope, event, 0});
                task->next = now + task->interval; // Skip missed ticks; never build an unbounded backlog.
            }
            for (int budget = 0; budget < 32 && !task->work.empty() && !task->waiting && error.empty(); ++budget) {
                step(*task);
                if (task->work.size() > 128) error = "pending work limit is 128";
            }
            if (!error.empty()) {
                error = located(error, task->line);
                failures.push_back({task->id, task->label, error});
                if (failures.size() > 8) failures.pop_front();
                output("ERROR: job " + std::to_string(task->id) + ": " + error); finish(task);
            }
            else if (task->work.empty() && !task->waiting && !task->interval) finish(task);
        }
    }
    bool kill(unsigned long id) {
        for (const auto &task : tasks) if (task->id == id) { auto copy = task; finish(copy); return true; }
        return false;
    }
    void reset() {
        while (!tasks.empty()) finish(tasks.back());
        globals = std::make_shared<Scope>();
        failures.clear();
    }
};
} // namespace cw
