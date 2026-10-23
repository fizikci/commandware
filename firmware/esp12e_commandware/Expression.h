#pragma once

// Included inside namespace cw after Value, Event, Scope and identifier helpers.
inline std::vector<std::string> valueParameters(const std::string &name) {
    if (name == "min" || name == "max") return {"a", "b"};
    if (name == "clamp") return {"x", "low", "high"};
    if (name == "notehz") return {"text"};
    if (name == "len") return {"x"};
    if (name == "contains") return {"x", "item"};
    if (name == "trim" || name == "lower" || name == "upper") return {"text"};
    if (name == "slice") return {"x", "start", "end"};
    if (name == "replace") return {"text", "old", "new"};
    if (name == "at") return {"items", "index"};
    if (name == "join") return {"items", "separator"};
    if (name == "number" || name == "int" || name == "string" || name == "abs" ||
        name == "floor" || name == "ceil" || name == "round") return {"x"};
    return {};
}
inline bool valueFunction(const std::string &name) { return !valueParameters(lower(name)).empty(); }

// Validate Unicode scalar encoding and return byte offsets of character boundaries.
inline bool textOffsets(const std::string &text, std::vector<size_t> &offsets, std::string &error, bool collect = true) {
    if (text.size() > 4096) { error = "string exceeds 4096 UTF-8 bytes"; return false; }
    offsets.clear();
    for (size_t p = 0; p < text.size();) {
        if (collect) offsets.push_back(p);
        unsigned char first = (unsigned char)text[p++];
        unsigned code = first; size_t count = 0;
        if (first < 0x80) continue;
        if (first >= 0xc2 && first <= 0xdf) { code = first & 31; count = 1; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 15; count = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 7; count = 3; }
        else { error = "invalid UTF-8 text"; return false; }
        if (p + count > text.size()) { error = "invalid UTF-8 text"; return false; }
        for (size_t i = 0; i < count; ++i) {
            unsigned char next = (unsigned char)text[p++];
            if ((next & 0xc0) != 0x80) { error = "invalid UTF-8 text"; return false; }
            code = (code << 6) | (next & 63);
        }
        if ((count == 1 && code < 0x80) || (count == 2 && code < 0x800) ||
            (count == 3 && code < 0x10000) || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) {
            error = "invalid UTF-8 text"; return false;
        }
    }
    if (collect) offsets.push_back(text.size());
    return true;
}
inline bool checkedValue(const Value &value, std::string &error) {
    if (value.kind == Value::TEXT) { std::vector<size_t> offsets; return textOffsets(value.text, offsets, error, false); }
    if (value.kind == Value::LIST) {
        if (value.list.size() > 64) { error = "list exceeds 64 items"; return false; }
        for (const auto &item : value.list) {
            if (item.kind == Value::LIST) { error = "nested lists are unsupported"; return false; }
            if (!checkedValue(item, error)) return false;
        }
    } else if (!std::isfinite(value.number)) { error = "nonfinite number"; return false; }
    return true;
}

class Expression {
    struct Node {
        enum Kind { NUMBER, TEXT, REF, UNARY, BINARY, CALL, LITERAL, GROUP } kind;
        std::string text;
        std::vector<std::shared_ptr<Node>> args;
        Node(Kind k, std::string s) : kind(k), text(std::move(s)) {}
    };
    using Ptr = std::shared_ptr<Node>;
    std::string source;
    size_t pos = 0;
    bool syntaxOnly;
    Scope &scope; Event &event; std::string &error;
    static bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
    static bool digit(char c) { return c >= '0' && c <= '9'; }
    void space() { while (pos < source.size() && isspace((unsigned char)source[pos])) ++pos; }
    Ptr node(Node::Kind kind, const std::string &text) { return std::make_shared<Node>(kind, text); }
    std::string name() {
        size_t start = pos;
        if (pos < source.size() && letter(source[pos])) {
            ++pos; while (pos < source.size() && (letter(source[pos]) || digit(source[pos]))) ++pos;
        }
        return source.substr(start, pos - start);
    }
    std::string option() {
        space(); size_t start = pos;
        if (source.compare(pos, 2, "--") != 0) return "";
        pos += 2; std::string result = name(); pos = start;
        return result.empty() ? "" : "--" + result;
    }
    static size_t numberEnd(const std::string &s, size_t p) {
        size_t start = p;
        if (p + 2 <= s.size() && s[p] == '0' && (s[p+1] == 'x' || s[p+1] == 'X')) {
            p += 2; size_t first = p;
            while (p < s.size() && isxdigit((unsigned char)s[p])) ++p;
            return p == first ? start : p;
        }
        bool digits = false;
        while (p < s.size() && digit(s[p])) { ++p; digits = true; }
        if (p < s.size() && s[p] == '.') {
            ++p; while (p < s.size() && digit(s[p])) { ++p; digits = true; }
        }
        if (!digits) return start;
        if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
            size_t exponent = p++;
            if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
            size_t first = p; while (p < s.size() && digit(s[p])) ++p;
            if (first == p) p = exponent;
        }
        return p;
    }
    static double numeric(const Value &value, std::string &error) {
        if (value.kind == Value::LIST) { error = "expected numeric value"; return 0; }
        double n = value.number;
        if (value.kind == Value::TEXT) {
            const std::string &s = value.text;
            size_t start = 0, end = s.size();
            while (start < end && isspace((unsigned char)s[start])) ++start;
            while (end > start && isspace((unsigned char)s[end-1])) --end;
            size_t first = start;
            if (first < end && (s[first] == '+' || s[first] == '-')) ++first;
            if (first == end || numberEnd(s, first) != end) { error = "invalid numeric text"; return 0; }
            n = strtod(s.c_str() + start, nullptr);
        }
        if (!std::isfinite(n)) error = "nonfinite number";
        return n;
    }
    double numeric(const Value &value) { return numeric(value, error); }
    bool requireText(const Value &value) {
        if (value.kind != Value::TEXT) { error = "expected string"; return false; }
        return checkedValue(value, error);
    }
    bool requireList(const Value &value) {
        if (value.kind != Value::LIST) { error = "expected list"; return false; }
        return checkedValue(value, error);
    }
    bool index(const Value &value, size_t limit, size_t &result) {
        if (value.kind != Value::NUMBER || !std::isfinite(value.number) || value.number < 0 || std::trunc(value.number) != value.number) {
            error = "index must be a nonnegative integer"; return false;
        }
        if (value.number > (double)limit) { error = "index out of range"; return false; }
        result = (size_t)value.number; return true;
    }
    Value reference(const std::string &key) {
        if (key == "value") {
            if (!event.hasValue) error = "pipeline value is required";
            else checkedValue(event.value, error);
            return event.value;
        }
        auto &globals = scope.global ? scope.global->vars : scope.vars;
        auto variable = globals.find(key), parameter = scope.params.find(key), field = event.fields.find(key);
        unsigned matches = (variable != globals.end()) + (parameter != scope.params.end()) + (field != event.fields.end());
        if (matches > 1) error = "ambiguous name '" + key + "'";
        else if (variable != globals.end()) { checkedValue(variable->second, error); return variable->second; }
        else if (parameter != scope.params.end()) { checkedValue(parameter->second, error); return parameter->second; }
        else if (field != event.fields.end()) { checkedValue(field->second, error); return field->second; }
        else error = "unknown name '" + key + "'";
        return {};
    }
    static int precedence(const std::string &op) {
        if (op == "||") return 1;
        if (op == "&&") return 2;
        if (op == "==" || op == "!=") return 3;
        if (op == "<" || op == "<=" || op == ">" || op == ">=") return 4;
        if (op == "+" || op == "-") return 5;
        if (op == "*" || op == "/" || op == "%") return 6;
        return 0;
    }
    Ptr parse(int minimum = 0, int depth = 0) {
        if (depth > 16) { error = "expression nesting exceeds 16"; return {}; }
        space();
        if (pos >= source.size() || !option().empty()) { error = "missing expression value"; return {}; }
        char c = source[pos++]; Ptr left;
        if (c == '+' || c == '-' || c == '!') {
            left = node(Node::UNARY, std::string(1, c)); left->args.push_back(parse(7, depth+1));
        } else if (c == '(') {
            Ptr inner = parse(0, depth+1); left = node(Node::GROUP, ""); left->args.push_back(inner); space();
            if (error.empty() && (pos == source.size() || source[pos++] != ')')) error = "missing expression closing parenthesis";
        } else if (c == '"' || c == '\'') {
            left = node(Node::TEXT, ""); std::string text; bool closed = false;
            while (pos < source.size() && error.empty()) {
                char ch = source[pos++];
                if (ch == c) { closed = true; break; }
                if (ch == '\\') {
                    if (pos == source.size()) break;
                    ch = source[pos++];
                    if (ch == 'n') text += '\n';
                    else if (ch == '"' || ch == '\'' || ch == '\\' || ch == '$') text += ch;
                    else error = "unknown string escape";
                } else if (ch == '$') {
                    if (!text.empty()) { left->args.push_back(node(Node::LITERAL, text)); text.clear(); }
                    std::string key = name();
                    if (key.empty()) error = "expected name after $";
                    left->args.push_back(node(Node::REF, key));
                } else text += ch;
            }
            if (!closed && error.empty()) error = "unterminated string";
            if (!text.empty()) left->args.push_back(node(Node::LITERAL, text));
        } else if (c == '$') {
            std::string key = name(); if (key.empty()) error = "expected name after $";
            left = node(Node::REF, key);
        } else if (digit(c) || c == '.') {
            size_t start = --pos; pos = numberEnd(source, pos);
            if (start == pos) error = "invalid number";
            left = node(Node::NUMBER, source.substr(start, pos-start));
        } else if (letter(c)) {
            --pos; std::string fn = lower(name()); auto params = valueParameters(fn);
            left = node(Node::CALL, fn);
            if (params.empty()) { error = "invalid expression: read variable values with $name"; return {}; }
            if (pos < source.size() && !isspace((unsigned char)source[pos])) { error = "function arguments require spaces"; return {}; }
            bool named = !option().empty(); std::map<std::string, Ptr> args;
            for (const auto &parameter : params) {
                std::string key = parameter;
                if (named) {
                    std::string opt = option();
                    if (opt.empty()) { error = "missing named function argument"; break; }
                    pos += opt.size(); key = opt.substr(2);
                    if (std::find(params.begin(), params.end(), key) == params.end() || args.count(key)) {
                        error = "unknown or duplicate function argument"; break;
                    }
                } else if (!option().empty()) { error = "cannot mix positional and named function arguments"; break; }
                args[key] = parse(0, depth+1);
                if (!error.empty()) break;
            }
            for (const auto &parameter : params) left->args.push_back(args[parameter]);
        } else { error = "invalid expression"; return {}; }
        bool compared = false;
        while (error.empty()) {
            space(); if (!option().empty()) break;
            std::string op;
            for (const char *candidate : {"||", "&&", "==", "!=", "<=", ">=", "+", "-", "*", "/", "%", "<", ">"})
                if (source.compare(pos, std::string(candidate).size(), candidate) == 0) { op = candidate; break; }
            int level = precedence(op); if (!level || level < minimum) break;
            if (level == 3 || level == 4) {
                if (compared) { error = "comparison chaining is unsupported"; break; } compared = true;
            }
            pos += op.size(); Ptr right = parse(level+1, depth+1);
            if (!error.empty()) break;
            if (level == 3 && right->kind == Node::BINARY && (precedence(right->text) == 3 || precedence(right->text) == 4)) {
                error = "comparison chaining is unsupported"; break;
            }
            Ptr parent = node(Node::BINARY, op); parent->args = {left, right}; left = parent;
        }
        return left;
    }
    Value evaluate(const Ptr &n, int depth = 0) {
        if (depth > 16) { error = "expression nesting exceeds 16"; return {}; }
        if (!n || !error.empty()) return {};
        if (n->kind == Node::GROUP) return evaluate(n->args[0], depth+1);
        if (n->kind == Node::LITERAL) return Value(n->text);
        if (n->kind == Node::NUMBER) return Value(numeric(Value(n->text)));
        if (n->kind == Node::REF) return reference(n->text);
        if (n->kind == Node::TEXT) {
            std::string result;
            for (const auto &part : n->args) {
                Value value = evaluate(part, depth+1);
                if (value.kind == Value::LIST) error = "expected numeric value";
                if (!error.empty()) return {};
                if (result.size() + value.str().size() > 4096) { error = "string exceeds 4096 UTF-8 bytes"; return {}; }
                result += value.str();
            }
            checkedValue(Value(result), error);
            return Value(result);
        }
        if (n->kind == Node::UNARY) {
            double value = numeric(evaluate(n->args[0], depth+1));
            return Value(n->text == "!" ? value == 0 : n->text == "-" ? -value : value);
        }
        if (n->kind == Node::CALL) {
            std::vector<Value> args;
            for (const auto &arg : n->args) args.push_back(evaluate(arg, depth+1));
            if (!error.empty()) return {};
            for (const auto &arg : args) if (!checkedValue(arg, error)) return {};
            const auto &fn = n->text;
            if (fn == "len") {
                if (args[0].kind == Value::LIST) return Value((double)args[0].list.size());
                if (!requireText(args[0])) return {};
                std::vector<size_t> offsets; textOffsets(args[0].text, offsets, error);
                return Value((double)offsets.size()-1);
            }
            if (fn == "contains") {
                if (args[0].kind == Value::TEXT) {
                    if (!requireText(args[1])) return {};
                    return Value(args[0].text.find(args[1].text) != std::string::npos);
                }
                if (!requireList(args[0])) return {};
                if (args[1].kind == Value::LIST) { error = "list comparison is unsupported"; return {}; }
                for (const auto &item : args[0].list) {
                    bool equal = item.kind == Value::TEXT && args[1].kind == Value::TEXT ?
                        item.text == args[1].text : numeric(item) == numeric(args[1]);
                    if (!error.empty()) return {};
                    if (equal) return Value(1);
                }
                return Value(0);
            }
            if (fn == "trim" || fn == "lower" || fn == "upper") {
                if (!requireText(args[0])) return {};
                std::string text = args[0].text;
                if (fn == "trim") {
                    size_t start = text.find_first_not_of(" \t\r\n\v\f");
                    if (start == std::string::npos) return Value(std::string());
                    return Value(text.substr(start, text.find_last_not_of(" \t\r\n\v\f")-start+1));
                }
                for (char &c : text) {
                    if (fn == "lower" && c >= 'A' && c <= 'Z') c += 'a'-'A';
                    else if (fn == "upper" && c >= 'a' && c <= 'z') c -= 'a'-'A';
                }
                return Value(text);
            }
            if (fn == "slice") {
                std::vector<size_t> offsets;
                if (args[0].kind != Value::LIST && !requireText(args[0])) return {};
                if (args[0].kind == Value::TEXT) textOffsets(args[0].text, offsets, error);
                size_t length = args[0].kind == Value::LIST ? args[0].list.size() : offsets.size()-1;
                size_t start = 0, end = 0;
                if (!index(args[1], length, start) || !index(args[2], length, end)) return {};
                if (start > end) { error = "invalid slice range"; return {}; }
                if (args[0].kind == Value::TEXT) return Value(args[0].text.substr(offsets[start], offsets[end]-offsets[start]));
                Value result; result.kind = Value::LIST;
                result.list.assign(args[0].list.begin()+start, args[0].list.begin()+end); return result;
            }
            if (fn == "at") {
                if (!requireList(args[0])) return {};
                size_t at = 0;
                if (!index(args[1], args[0].list.size(), at)) return {};
                if (at == args[0].list.size()) { error = "index out of range"; return {}; }
                return args[0].list[at];
            }
            if (fn == "replace") {
                for (const auto &arg : args) if (!requireText(arg)) return {};
                const auto &text = args[0].text, &old = args[1].text, &replacement = args[2].text;
                if (old.empty()) { error = "empty replacement search"; return {}; }
                std::string result; size_t start = 0;
                while (start < text.size()) {
                    size_t match = text.find(old, start);
                    if (match == std::string::npos) match = text.size();
                    size_t added = match-start + (match < text.size() ? replacement.size() : 0);
                    if (result.size()+added > 4096) { error = "string exceeds 4096 UTF-8 bytes"; return {}; }
                    result.append(text, start, match-start);
                    if (match == text.size()) break;
                    result += replacement; start = match+old.size();
                }
                return Value(result);
            }
            if (fn == "join") {
                if (!requireList(args[0]) || !requireText(args[1])) return {};
                std::string result;
                for (size_t i = 0; i < args[0].list.size(); ++i) {
                    const auto &item = args[0].list[i]; if (!requireText(item)) return {};
                    size_t added = item.text.size() + (i ? args[1].text.size() : 0);
                    if (result.size()+added > 4096) { error = "string exceeds 4096 UTF-8 bytes"; return {}; }
                    if (i) result += args[1].text;
                    result += item.text;
                }
                return Value(result);
            }
            if (fn == "string") {
                if (args[0].kind == Value::LIST) error = "expected numeric value";
                return Value(args[0].str());
            }
            if (fn == "notehz") {
                if (args[0].kind != Value::TEXT) { error = "notehz requires text"; return {}; }
                const auto &note = args[0].text; const std::string names = "C D EF G A B";
                size_t semitone = note.empty() ? std::string::npos : names.find(note[0]);
                size_t octavePos = 1; int accidental = 0;
                if (note.size() > 1 && (note[1] == '#' || note[1] == 'b')) { accidental = note[1] == '#' ? 1 : -1; octavePos = 2; }
                if (semitone == std::string::npos || note[0] == ' ' || octavePos+1 != note.size() || note[octavePos] < '0' || note[octavePos] > '8') {
                    error = "invalid note"; return {};
                }
                return Value(440.0 * std::pow(2.0, ((note[octavePos]-'0'+1)*12+(int)semitone+accidental-69)/12.0));
            }
            std::vector<double> values;
            for (const auto &arg : args) values.push_back(numeric(arg));
            double x = values[0];
            if (fn == "int") x = std::trunc(x);
            else if (fn == "abs") x = std::fabs(x);
            else if (fn == "floor") x = std::floor(x);
            else if (fn == "ceil") x = std::ceil(x);
            else if (fn == "round") x = std::round(x);
            else if (fn == "min") x = std::min(x, values[1]);
            else if (fn == "max") x = std::max(x, values[1]);
            else if (fn == "clamp") {
                if (values[1] > values[2]) error = "clamp: low exceeds high";
                x = std::min(std::max(x, values[1]), values[2]);
            }
            return Value(x);
        }
        const auto &op = n->text; Value left = evaluate(n->args[0], depth+1);
        if (op == "&&" && numeric(left) == 0) return Value(0);
        if (op == "||" && numeric(left) != 0) return Value(1);
        Value right = evaluate(n->args[1], depth+1);
        if ((op == "==" || op == "!=") && left.kind == Value::TEXT && right.kind == Value::TEXT)
            return Value(op == "==" ? left.text == right.text : left.text != right.text);
        double a = numeric(left), b = numeric(right), result = 0;
        if ((op == "/" || op == "%") && b == 0) { error = "division by zero"; return {}; }
        if (op == "+") result = a+b;
        else if (op == "-") result = a-b;
        else if (op == "*") result = a*b;
        else if (op == "/") result = a/b;
        else if (op == "%") result = std::fmod(a,b);
        else if (op == "<") result = a < b;
        else if (op == "<=") result = a <= b;
        else if (op == ">") result = a > b;
        else if (op == ">=") result = a >= b;
        else if (op == "==") result = a == b;
        else if (op == "!=") result = a != b;
        else if (op == "&&" || op == "||") result = b != 0;
        if (!std::isfinite(result)) error = "nonfinite number";
        return Value(result);
    }
public:
    static double numberValue(const Value &value, std::string &error) {
        if (!checkedValue(value, error)) return 0;
        return numeric(value, error);
    }
    Expression(std::string s, Scope &sc, Event &ev, std::string &e, bool syntax = false) : source(std::move(s)), syntaxOnly(syntax), scope(sc), event(ev), error(e) {}
    size_t consumed() const { return pos; }
    bool prefix() { parse(); return error.empty(); }
    Value run() {
        Ptr root = parse(); space();
        if (error.empty() && pos != source.size()) error = "invalid expression";
        Value result = error.empty() ? (syntaxOnly ? Value(1) : evaluate(root)) : Value();
        if (error.empty() && !syntaxOnly) checkedValue(result, error);
        return result;
    }
};

inline double numericValue(const Value &value, std::string &error) {
    // Host numeric arguments do not need another parsed/evaluated expression.
    // Re-entering the evaluator here exhausted the ESP8266's small stack.
    return Expression::numberValue(value, error);
}
inline std::string formatValue(const Value &value) {
    if (value.kind != Value::LIST) return value.str();
    std::string result = "[";
    for (size_t i = 0; i < value.list.size(); ++i) {
        if (i) result += ", ";
        const auto &item = value.list[i];
        if (item.kind != Value::TEXT) { result += item.str(); continue; }
        result += '"';
        for (unsigned char c : item.text) {
            if (c == '"' || c == '\\') { result += '\\'; result += (char)c; }
            else if (c == '\b') result += "\\b";
            else if (c == '\f') result += "\\f";
            else if (c == '\n') result += "\\n";
            else if (c == '\r') result += "\\r";
            else if (c == '\t') result += "\\t";
            else if (c < 0x20) { char escaped[7]; snprintf(escaped, sizeof(escaped), "\\u%04x", c); result += escaped; }
            else result += (char)c;
        }
        result += '"';
    }
    return result + "]";
}
