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
    bool option = false;
    std::string text;
    std::vector<Value> list;
    Value() = default;
    Value(double n) : number(n) {}
    Value(std::string s) : kind(TEXT), text(std::move(s)) {}
    std::string str() const {
        if (kind == TEXT) return text;
        if (kind == LIST) return "[list]";
        char buffer[48]; snprintf(buffer, sizeof(buffer), "%.17g", number == 0 ? 0.0 : number);
        return buffer;
    }
};
struct Completion {};
struct Event {
    Value value;
    std::map<std::string, Value> fields;
    uint32_t timestamp = 0;
    bool hasValue = false;
    bool emitted = true;
    std::shared_ptr<Completion> completion;
};
struct Scope {
    std::map<std::string, Value> vars;
    std::map<std::string, Value> params;
    std::shared_ptr<Scope> global;
    std::string functionName;
};
struct Token { std::string text; bool quoted = false; uint16_t line = 1; };
struct TokenWithoutLocation { std::string text; bool quoted; };
static_assert(sizeof(Token) == sizeof(TokenWithoutLocation), "token line number must fit existing padding");
inline std::string located(const std::string &message, uint16_t line) {
    return message.find(" at line ") == std::string::npos ? message + " at line " + std::to_string(line) : message;
}
struct Stage;
using StagePtr = std::shared_ptr<Stage>;
using Pipe = std::vector<StagePtr>;
using Body = std::vector<Pipe>;
struct Stage { std::string name; std::vector<Token> args; Body body; Body alternate; bool hasBody = false; std::shared_ptr<std::string> source; uint16_t line = 1; };

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
    // Chunked storage avoids a large contiguous allocation (and the old/new
    // buffers coexisting) when a program crosses a vector growth boundary.
    std::deque<Token> tokens;
    std::shared_ptr<std::string> retainedSource;
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
                stage->line = tokens[pos].line; stage->source = retainedSource;
                stage->name = std::move(tokens[pos++].text);
                if (++nodes > 128) { error = "script exceeds 128 commands"; break; }
                while (pos < tokens.size() && !at("{") && !at("}") && !at("|") && !at(";") && !at("\n"))
                    stage->args.push_back(std::move(tokens[pos++]));
                if (at("{")) {
                    ++pos; stage->hasBody = true; stage->body = body(depth + 1, stage->name == "schedule");
                    if (!at("}")) { error = "missing closing brace"; break; }
                    ++pos;
                }
                if (lower(stage->name) == "if") {
                    size_t saved = pos; while (at("\n")) ++pos;
                    if (pos < tokens.size() && !tokens[pos].quoted && lower(tokens[pos].text) == "else") {
                        ++pos;
                        if (!at("{")) { error = "else requires a body"; break; }
                        ++pos; stage->alternate = body(depth + 1);
                        if (!at("}")) { error = "missing closing brace"; break; }
                        ++pos;
                    } else pos = saved;
                }
                pipe.push_back(stage);
            } while (at("|") && error.empty());
            result.push_back(pipe);
            if (pos < tokens.size() && !at("}") && !at(";") && !at("\n") && !entries) error = "expected statement separator";
            separators();
            // Parsed statements live in the AST now. Release their lexical
            // storage while parsing the remainder, including nested bodies.
            tokens.erase(tokens.begin(), tokens.begin()+pos);
            pos = 0;
        }
        return result;
    }
public:
    Parser(std::string &e) : error(e) {}
    Body parse(const std::string &source) {
        retainedSource=std::make_shared<std::string>(source);
        if (source.size() > 4096) { error = "script exceeds 4096 bytes at line 1"; return {}; }
        for (size_t i = 0; i < source.size() && error.empty();) {
            uint16_t tokenLine = line;
            char c = source[i++];
            if (c == '\r' || c == ' ' || c == '\t') continue;
            if (c == '#') { while (i < source.size() && source[i] != '\n') ++i; continue; }
            if (c == '"' || c == '\'') {
                char quote=c;std::string s(1,c);bool closed=false;
                while(i<source.size()){
                    c=source[i++];s+=c;if(c=='\n')++line;
                    if(c=='\\'&&i<source.size()){c=source[i++];s+=c;if(c=='\n')++line;continue;}
                    if(c==quote){closed=true;break;}
                }
                if(!closed){error="unterminated string";errorLine=tokenLine;}
                tokens.push_back({s,true,tokenLine});
            } else if (c == '|' && i < source.size() && source[i] == '|') {
                ++i; tokens.push_back({"||", false, tokenLine});
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

#include "Expression.h"

inline bool expressionToken(const Token &t) {
    return !t.quoted && !t.text.empty() && t.text.compare(0, 2, "--") != 0 &&
        (std::string("\"'$(!-+.").find(t.text[0]) != std::string::npos ||
         isdigit((unsigned char)t.text[0]) || t.text.find('(') != std::string::npos ||
         valueFunction(t.text.substr(0, t.text.find(' '))));
}
inline std::string argumentSource(const std::vector<Token> &args, size_t end) {
    std::string source;
    for (size_t i = 0; i < end; ++i) {
        if (i) source += ' ';
        if (!args[i].quoted) { source += args[i].text; continue; }
        if(args[i].text.size()>=2 && (args[i].text[0]=='"'||args[i].text[0]=='\'') && args[i].text.back()==args[i].text[0]){source+=args[i].text;continue;}
        source += '"';
        for (char c : args[i].text) {
            if (c == '\n') source += "\\n";
            else { if (c == '"' || c == '\\') source += '\\'; source += c; }
        }
        source += '"';
    }
    return source;
}
inline bool normalizeArguments(Stage &stage, std::string &error) {
    if (stage.name == "func" || stage.name == "schedule" || stage.name == "help") return true;
    Scope scope; Event event;
    if (valueFunction(stage.name)) {
        size_t out = 0;
        while (out < stage.args.size() && (stage.args[out].quoted || stage.args[out].text != "--out")) ++out;
        std::string text = stage.name + " " + argumentSource(stage.args, out);
        Expression(text, scope, event, error, true).run();
        std::vector<Token> args = {{text, false, stage.line}};
        args.insert(args.end(), stage.args.begin()+out, stage.args.end());
        stage.args = std::move(args); return error.empty();
    }
    std::string source = argumentSource(stage.args, stage.args.size());
    std::vector<Token> args;
    for (size_t pos = 0; pos < source.size() && error.empty();) {
        while (pos < source.size() && isspace((unsigned char)source[pos])) ++pos;
        if (pos == source.size()) break;
        size_t end = pos;
        while (end < source.size() && !isspace((unsigned char)source[end])) ++end;
        std::string word = source.substr(pos, end-pos);
        bool nameArgument = (args.empty() && (stage.name == "set")) ||
            (!args.empty() && args.back().text == "--out");
        if (!nameArgument && expressionToken({word, false, stage.line})) {
            Expression parser(source.substr(pos), scope, event, error, true);
            parser.prefix(); end = pos + parser.consumed();
        }
        std::string text = source.substr(pos, end-pos);
        while (!text.empty() && isspace((unsigned char)text.back())) text.pop_back();
        args.push_back({text, false, stage.line}); pos = end;
    }
    stage.args = std::move(args); return error.empty();
}

struct SignalState { int value = 0, previous = -1, count = 0; uint32_t deadline = 0; };
using StageStates = std::map<const Stage *, SignalState>;
struct Action {
    Pipe pipe;
    std::shared_ptr<Scope> scope;
    std::shared_ptr<Event> event;
    int depth = 0;
    std::shared_ptr<StageStates> states;
    std::shared_ptr<Event> result; // A completion marker copies a final statement result.
    StagePtr iteration;
    size_t index = 0;
    std::string out;
    std::vector<Action> exits;
    uint64_t timerOrder = 0;
    std::shared_ptr<Completion> barrier;
};
struct Function { std::vector<std::string> params; Body body; StagePtr definition; };
struct Task {
    unsigned long id = 0;
    std::string label;
    std::deque<Action> work;
    Pipe repeat;
    std::shared_ptr<Scope> scope;
    std::shared_ptr<StageStates> states = std::make_shared<StageStates>();
    std::shared_ptr<Event> captured;
    std::shared_ptr<Completion> blockedGroup;
    std::map<std::pair<StagePtr,std::shared_ptr<StageStates>>, std::pair<uint32_t, Action>> timers;
    uint32_t interval = 0, next = 0, wake = 0, end = 0, edge = 0, halfPeriod = 0;
    uint32_t registered = 0, sequence = 0;
    uint64_t timerSequence = 0;
    int tonePin = -1, toneState = 0, lastSample = -1;
    bool waiting = false, periodic = false, first = true, clockSource = false, watchSource = false, bounded = false, scheduled = false;
    std::string sourceOut, functionName;
    uint16_t line = 1;
};
struct FailedJob { unsigned long id; std::string label, message; };

class Runtime {
public:
    using Host = std::function<bool(const std::string &, const std::vector<Value> &, Event &, unsigned long, std::string &)>;
    Host host;
    std::function<bool(const std::string &)> schedule, background; // Compatibility hooks; AST jobs no longer use them.
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
    const Action *activeAction = nullptr;
    std::vector<const Stage*> accountingStages;
    static bool due(uint32_t a, uint32_t b) { return (int32_t)(a-b) >= 0; }
    static bool builtin(const std::string &n) {
        return valueFunction(n) || std::string("|func|schedule|clock|every|do|if|each|set|where|split|sleep|tone|adc|read|write|map|print|toggle|not|edge|rising|falling|debounce|divide|pulse|shiftin|shiftout|lcd|oled|ohm|help|pins|jobs|kill|status|reset|run|script|").find("|"+n+"|") != std::string::npos;
    }
    Value eval(const Token &t, Scope &scope, Event &event) {
        if (t.quoted) return Expression(argumentSource({t}, 1), scope, event, error).run();
        if (expressionToken(t)) return Expression(t.text, scope, event, error).run();
        return Value(t.text);
    }
    uint32_t milliseconds(const Value &v, bool zero = false) {
        double n=numericValue(v,error);if(!error.empty())return 0;
        if (n < (zero ? 0 : 1) || n > 2147483647 || n != std::floor(n)) {
            error = zero ? "duration requires 0..2147483647 integer milliseconds" : "interval requires 1..2147483647 integer milliseconds"; return 0;
        }
        return (uint32_t)n;
    }
    void enqueue(Task &task, const Pipe &pipe, std::shared_ptr<Scope> scope, std::shared_ptr<Event> event, int depth,
                 std::shared_ptr<StageStates> states) {
        if (!pipe.empty()) task.work.push_front({pipe, scope, event, depth, states, {}, {}, 0, {}, {}, 0, {}});
    }
    void pushBody(Task &task, const Body &body, std::shared_ptr<Scope> scope, std::shared_ptr<Event> input, int depth,
                  std::shared_ptr<StageStates> states, std::shared_ptr<Event> result, const std::vector<Action> &exits = {}) {
        if (body.empty()) { if (result) *result = *input; return; }
        std::vector<std::shared_ptr<Completion>> groups(body.size());
        for(size_t i=0;i<body.size();++i)groups[i]=i+1<body.size()?std::make_shared<Completion>():input->completion;
        for (size_t i = body.size(); i-- > 0;) {
            auto ev = std::make_shared<Event>(*input);ev->completion=groups[i];
            if (result && i == body.size()-1) task.work.push_front({{}, scope, ev, depth, states, result, {}, 0, {}, {}, 0, {}});
            Action statement;statement.pipe=body[i];statement.scope=scope;statement.event=ev;statement.depth=depth;statement.states=states;
            if(i>0)statement.barrier=groups[i-1];
            if(i==body.size()-1)statement.exits=exits;
            task.work.push_front(statement);
        }
    }
    bool definitions(const Body &body, std::map<std::string, Function> &defs, bool top = true) {
        std::vector<std::string> seen;
        for (const auto &pipe : body) for (const auto &s : pipe) {
            errorLine = s->line;
            auto n = lower(s->name);
            if (builtin(n)) s->name = n;
            if (n == "func") {
                if (!top || !s->hasBody || pipe.size()!=1 || s->args.empty() || !identifier(s->args[0].text) || s->args[0].quoted) { error="func requires a top-level NAME [PARAM...] { BODY }"; return false; }
                const auto name = s->args[0].text;
                if (builtin(lower(name)) || std::find(seen.begin(),seen.end(),name)!=seen.end()) { error="duplicate or built-in function name"; return false; }
                seen.push_back(name); Function f; f.body=s->body; f.definition=s;
                for(size_t i=1;i<s->args.size();++i) {
                    auto p=s->args[i].text;
                    if(s->args[i].quoted || !identifier(p) || std::find(f.params.begin(),f.params.end(),p)!=f.params.end()) {error="invalid/duplicate function parameter";return false;}
                    f.params.push_back(p);
                }
                if(defs.size()>=16 && !defs.count(name)){error="function limit is 16";return false;}
                defs[name]=f;
            }
            if (!definitions(s->body,defs,false) || !definitions(s->alternate,defs,false)) return false;
        }
        return true;
    }
    bool arguments(const std::vector<Token> &args, const std::vector<std::string> &params, std::vector<Token> &ordered) {
        ordered.resize(params.size());
        if(args.empty()) { if(params.empty())return true; error="missing function arguments";return false; }
        bool named=!args[0].quoted && args[0].text.compare(0,2,"--")==0;
        if(!named) {
            if(args.size()!=params.size()){error="incorrect function argument count";return false;}
            for(auto &a:args)if(!a.quoted&&a.text.compare(0,2,"--")==0){error="cannot mix positional and named function arguments";return false;}
            ordered=args;return true;
        }
        std::vector<bool> seen(params.size());
        if(args.size()!=params.size()*2){error="incorrect named function arguments";return false;}
        for(size_t i=0;i<args.size();i+=2){
            auto key=args[i].text.substr(2); auto at=std::find(params.begin(),params.end(),key);
            if(args[i].quoted || args[i].text.compare(0,2,"--") || at==params.end() || seen[at-params.begin()]){error="unknown or duplicate function argument";return false;}
            size_t k=at-params.begin();seen[k]=true;ordered[k]=args[i+1];
        }
        return true;
    }
    bool stripOut(std::vector<Token> &args,std::string &out) {
        for(size_t i=0;i<args.size();++i)if(!args[i].quoted&&args[i].text=="--out"){
            if(i+2!=args.size()){error="--out requires names at end of command";return false;}
            out=args[i+1].text;args.resize(i);return true;
        }
        return true;
    }
    bool validate(const Body &body,const std::map<std::string,Function>&defs) {
        for(const auto &pipe:body)for(const auto &s:pipe){
            errorLine=s->line; const auto &n=s->name;
            if(!builtin(n)&&!defs.count(n)){error="unknown command '"+n+"'";return false;}
            if(n=="run"){error="run is currently simulator-only";return false;}
            if(n=="schedule") {
                if(!s->hasBody || !s->args.empty() || pipe.size()!=1){error="schedule requires entries";return false;}
                for(auto &p:s->body){
                    if(p.size()!=1||!p[0]->hasBody){error="invalid schedule entry";return false;}
                    auto &e=*p[0];
                    std::string offset=e.name;
                    if(!e.args.empty())offset+=" "+argumentSource(e.args,e.args.size());
                    e.name=offset;e.args.clear();
                    Scope scope;Event event;Expression(offset,scope,event,error,true).run();
                    if(!error.empty()||!validate(e.body,defs))return false;
                }
                continue;
            }
            if(!normalizeArguments(*s,error))return false;
            if(n=="func"){if(!validate(s->body,defs))return false;continue;}
            bool block=n=="do"||n=="if"||n=="each";
            if(!block&&s->hasBody){error="unexpected command body";return false;}
            auto args=s->args;std::string out;if(!stripOut(args,out))return false;
            if(!out.empty()){
                size_t begin=0;std::vector<std::string>keys;
                do{size_t end=out.find(',',begin);auto key=out.substr(begin,end==std::string::npos?end:end-begin);
                    if(!identifier(key)||std::find(keys.begin(),keys.end(),key)!=keys.end()){error="invalid or duplicate output name";return false;}
                    keys.push_back(key);if(end==std::string::npos)break;begin=end+1;
                }while(true);
                if(keys.size()>16){error="event field limit is 16";return false;}
            }

            if(!out.empty() && !(valueFunction(n)||defs.count(n)||n=="adc"||n=="read"||n=="map"||n=="split"||n=="each"||n=="clock"||n=="every"||n=="not"||n=="toggle"||n=="divide"||n=="pulse"||n=="ohm"||(n=="script"&&!args.empty()&&(lower(args[0].text)=="list"||lower(args[0].text)=="load"))||n=="shiftin")){error="--out unsupported for '"+n+"'";return false;}
            if(n=="set" && (args.size()!=2 || !identifier(args[0].text))){error="set requires NAME EXPR";return false;}
            if(n=="do" && (!s->hasBody||!args.empty())){error="do requires a body";return false;}
            if(n=="each" && (!s->hasBody||!args.empty())){error="each requires { BODY }";return false;}
            if((n=="if"||n=="where") && (args.size()!=1||(n=="if"&&!s->hasBody))){error=n+" requires an expression";return false;}
            if(n=="every" && args.size()!=1){error="every requires INTERVAL";return false;}
            if(n=="clock" && !(args.size()==1||(args.size()==3&&args[1].text=="--dur"))){error="clock requires HZ [--dur MS]";return false;}
            if(defs.count(n)){std::vector<Token> ordered;if(!arguments(args,defs.at(n).params,ordered))return false;}
            size_t c=args.size(); bool valid=true;
            if(n=="toggle"||n=="edge"||n=="rising"||n=="falling"||n=="jobs"||n=="pins"||n=="status"||n=="reset")valid=c==0;
            if(n=="sleep"||n=="read"||n=="kill"||n=="debounce"||n=="divide"||n=="pulse")valid=c==1;
            if(n=="adc")valid=c==1||(c==2&&args[1].text=="--watch");
            if(n=="not"||n=="ohm")valid=c<=1||(c==2&&args[0].text=="--value");
            if(n=="print")valid=c<=1||(c==2&&args[0].text=="--value");
            if(n=="write")valid=c==1||c==2||(c==3&&args[1].text=="--value");
            if(n=="split")valid=c==1||c==2||(c==3&&args[1].text=="--value");
            if(n=="map")valid=c==4||c==5||(c==6&&args[4].text=="--value");
            if(n=="tone")valid=c==5&&args[1].text=="--hz"&&args[3].text=="--dur";
            if(n=="adc" && (args.empty()||lower(args[0].text)!="a0"))valid=false;
            if(n=="help")valid=c<=1;
            if(n=="script") {
                auto action=c?lower(args[0].text):"";
                valid=(action=="list"&&c==1)||((action=="save"||action=="load"||action=="delete")&&c==2);
            }
            if(n=="shiftin"||n=="shiftout"){
                size_t pins=0;bool bits=false,order=false,value=false;
                for(size_t i=0;i<c;++i){
                    auto text=lower(args[i].text);
                    if(text=="msb"||text=="lsb"){if(order)valid=false;order=true;}
                    else if(text=="--bits"||text=="--value"){
                        bool &seen=text=="--bits"?bits:value;
                        if(seen||i+1>=c||(text=="--value"&&n=="shiftin"))valid=false;
                        seen=true;++i;
                    }else if(text.compare(0,2,"--")==0)valid=false;
                    else ++pins;
                }
                if(n=="shiftin")valid=valid&&pins==3;
                else valid=valid&&(pins==2||pins==3);
            }
            if(n=="lcd"||n=="oled"){
                if(args.empty())valid=false;
                else{
                    auto action=lower(args[0].text);
                    std::string allowed=n=="lcd"?(action=="print"?" --value ":" "):
                        action=="init"?" --address --width --height ":action=="print"?" --x --y --value ":action=="chart"?" --x --y --width --height --min --max --value ":action=="rect"?" --value ":" ";
                    std::vector<std::string>seen;size_t positional=0;bool explicitValue=false;
                    for(size_t i=1;i<c;++i){auto text=args[i].text;
                        if(text.compare(0,2,"--")==0){
                            if(allowed.find(" "+text+" ")==std::string::npos||std::find(seen.begin(),seen.end(),text)!=seen.end()||i+1>=c)valid=false;
                            seen.push_back(text);if(text=="--value")explicitValue=true;++i;
                        }else ++positional;
                    }
                    if(action=="clear")valid=valid&&c==1;
                    else if(action=="init")valid=valid&&positional<=1;
                    else if(n=="oled"&&action=="rect")valid=valid&&positional==4;
                    else if(action=="print"||(n=="oled"&&action=="chart"))valid=valid&&positional<=(n=="lcd"?1:3)&&!(explicitValue&&(positional==1||positional==3));
                    else valid=false;
                }
            }
            if(!valid){error="invalid arguments for '"+n+"'";return false;}
            if(!validate(s->body,defs)||!validate(s->alternate,defs))return false;
        }
        return true;
    }
    bool bind(Event &ev,Scope &scope,const std::string &names) {
        if(!ev.emitted)return true;
        if(!ev.hasValue){error="output binding requires a value";return false;}
        std::vector<std::string> keys;size_t start=0;
        do{
            size_t end=names.find(',',start);auto key=names.substr(start,end==std::string::npos?end:end-start);
            if(!identifier(key)||globals->vars.count(key)||scope.params.count(key)||std::find(keys.begin(),keys.end(),key)!=keys.end()){error="invalid or conflicting output name '"+key+"'";return false;}
            keys.push_back(key);if(end==std::string::npos)break;start=end+1;
        }while(true);
        size_t count=ev.fields.size();for(auto &key:keys)if(!ev.fields.count(key))++count;
        if(count>16){error="event field limit is 16";return false;}
        if(keys.size()>1&&(ev.value.kind!=Value::LIST||ev.value.list.size()!=keys.size())){error="output count mismatch";return false;}
        for(size_t i=0;i<keys.size();++i)ev.fields[keys[i]]=keys.size()==1?ev.value:ev.value.list[i];
        return true;
    }
    size_t valueBytes(const Value&v) {
        size_t bytes=v.kind==Value::NUMBER?8:0;
        if(v.kind==Value::TEXT){if(v.text.size()>4096)error="string limit is 4096 bytes";bytes+=v.text.size();}
        if(v.kind==Value::LIST){if(v.list.size()>64)error="list limit is 64";for(auto &x:v.list)bytes+=valueBytes(x);}
        return bytes;
    }
    bool boundedValues() {
        size_t bytes=0;std::vector<const Scope*>scopes;std::vector<const Event*>events;
        auto sc=[&](std::shared_ptr<Scope> s){if(!s||std::find(scopes.begin(),scopes.end(),s.get())!=scopes.end())return;scopes.push_back(s.get());for(auto &p:s->vars)bytes+=p.first.size()+valueBytes(p.second);for(auto&p:s->params)bytes+=p.first.size()+valueBytes(p.second);};
        auto ev=[&](std::shared_ptr<Event> e){if(!e||std::find(events.begin(),events.end(),e.get())!=events.end())return;events.push_back(e.get());if(e->hasValue)bytes+=valueBytes(e->value);for(auto&p:e->fields)bytes+=p.first.size()+valueBytes(p.second);};
        std::function<void(const Action&)>actionValues=[&](const Action&a){sc(a.scope);ev(a.event);ev(a.result);for(auto&exit:a.exits)actionValues(exit);};
        sc(globals);if(activeAction)actionValues(*activeAction);for(auto&t:tasks){sc(t->scope);ev(t->captured);for(auto&a:t->work)actionValues(a);for(auto&p:t->timers)actionValues(p.second.second);}
        if(bytes>8192)error="retained value limit is 8192 bytes";
        return error.empty();
    }
    size_t programBytes(const Body &body,std::vector<const std::string*>&seen) {
        size_t bytes=0;for(auto&p:body)for(auto&s:p){
            if(!s||std::find(accountingStages.begin(),accountingStages.end(),s.get())!=accountingStages.end())continue;accountingStages.push_back(s.get());
            if(s->source&&std::find(seen.begin(),seen.end(),s->source.get())==seen.end()){seen.push_back(s->source.get());bytes+=s->source->size();}
            bytes+=programBytes(s->body,seen)+programBytes(s->alternate,seen);
        }return bytes;
    }
    bool boundedPrograms(const std::map<std::string,Function>&defs,const Body &body) {
        accountingStages.clear();std::vector<const std::string*>seen;size_t bytes=programBytes(body,seen);
        for(auto&f:defs)bytes+=programBytes({{f.second.definition}},seen);
        std::function<size_t(const Action&)>actionBytes=[&](const Action&a){size_t n=programBytes({a.pipe},seen);if(a.iteration)n+=programBytes({{a.iteration}},seen);for(auto&exit:a.exits)n+=actionBytes(exit);return n;};
        for(auto&t:tasks){bytes+=programBytes({t->repeat},seen);for(auto&a:t->work)bytes+=actionBytes(a);for(auto&p:t->timers){bytes+=programBytes({{p.first.first}},seen);bytes+=actionBytes(p.second.second);}}
        if(accountingStages.size()>128){error="retained command limit is 128";return false;}
        if(bytes>16384){error="retained program limit is 16384 bytes";return false;}return true;
    }
    bool boundedFrames(){
        std::function<size_t(const Action&)>count=[&](const Action&a){size_t n=1;for(auto&exit:a.exits)n+=count(exit);return n;};
        size_t total=0;for(auto&t:tasks){for(auto&a:t->work)total+=count(a);for(auto&p:t->timers)total+=count(p.second.second);}
        if(activeAction)for(auto&exit:activeAction->exits)total+=count(exit);
        if(total>128){error="pending work limit is 128";return false;}return true;
    }
    void startSource(const Action &action,const StagePtr &s,std::vector<Token> args,const std::string&out) {
        auto child=std::make_shared<Task>();child->scope=action.scope;child->repeat=action.pipe;child->periodic=true;child->line=s->line;child->sourceOut=out;child->registered=now;
        child->label=s->name; child->next=now;
        auto &scope=*action.scope;auto &event=*action.event;
        if(s->name=="every")child->interval=milliseconds(eval(args[0],scope,event));
        else if(s->name=="clock"){
            Value hz=eval(args[0],scope,event);double frequency=numericValue(hz,error);
            if(!error.empty()||frequency<0.01||frequency>1000000){error="clock frequency requires 0.01..1000000";return;}
            child->clockSource=true;child->interval=std::max((uint32_t)1,(uint32_t)std::floor(500000.0/frequency+0.5));child->next=us+child->interval;
            if(args.size()==3){child->bounded=true;child->end=now+milliseconds(eval(args[2],scope,event));}
        }else{
            if(lower(args[0].text)!="a0"){error="adc supports A0 only";return;}
            child->watchSource=true;child->interval=100;
        }
        size_t running=0;for(auto&t:tasks)if(!t->scheduled)++running;
        if(running>=8)error="script job limit is 8";
        if(!error.empty())return;
        tasks.push_back(child);if(!boundedValues()||!boundedFrames()){tasks.pop_back();return;}
        child->id=newId();if(output)output("Started job "+std::to_string(child->id));
    }
    void step(Task &task) {
        Action action=task.work.front();task.work.pop_front();
        struct Guard { const Action *&slot; Guard(const Action *&s,const Action *a):slot(s){slot=a;} ~Guard(){slot=nullptr;} } guard(activeAction,&action);
        task.functionName=action.scope?action.scope->functionName:std::string();
        if(action.barrier){
            bool pending=false;for(auto &timer:task.timers)if(timer.second.second.event->completion==action.barrier)pending=true;
            if(pending){task.blockedGroup=action.barrier;task.work.push_front(action);return;}
            task.blockedGroup.reset();action.barrier.reset();
        }
        if(action.iteration){
            auto stage=action.iteration;
            if(action.index>=action.event->value.list.size())return;
            auto item=std::make_shared<Event>(*action.event);item->value=action.event->value.list[action.index++];item->completion=std::make_shared<Completion>();
            if(!action.out.empty()&&!bind(*item,*action.scope,action.out))return;
            action.barrier=item->completion;task.work.push_front(action);pushBody(task,stage->body,action.scope,item,action.depth,action.states,{});return;
        }
        if(action.pipe.empty()){if(action.result)*action.result=*action.event;return;}
        if(!action.event->emitted)return;
        auto s=action.pipe.front();action.pipe.erase(action.pipe.begin());executingLine=task.line=s->line;
        const auto &n=s->name;auto &scope=*action.scope;auto &event=*action.event;
        if(!action.states)action.states=task.states;
        auto continuation=[&](){if(!action.pipe.empty())task.work.push_front(action);};
        auto exits=[&](){auto chain=action.exits;Action outer=action;outer.exits.clear();outer.event.reset();chain.insert(chain.begin(),outer);return chain;};
        if(n=="func")return;
        auto args=s->args;std::string out;if(!stripOut(args,out))return;
        if(n=="__bind"){bind(event,scope,args[0].text);if(error.empty())continuation();return;}
        if(n=="every"||n=="clock"||(n=="adc"&&args.size()==2&&args[1].text=="--watch")){
            startSource(action,s,args,out);event.emitted=false;return;
        }
        if(n=="schedule"){
            std::vector<std::shared_ptr<Task>>pending;
            size_t total=0;for(auto&t:tasks)if(t->scheduled)++total;
            if(total+s->body.size()>32){error="pending schedule entry limit is 32";return;}
            for(auto&p:s->body){
                auto e=p[0];uint32_t offset=milliseconds(eval({e->name,false,e->line},scope,event),true);if(!error.empty())return;
                auto child=std::make_shared<Task>();child->label="schedule";child->scope=action.scope;child->scheduled=true;child->wake=now+offset;child->line=e->line;child->captured=std::make_shared<Event>(event);
                pushBody(*child,e->body,child->scope,child->captured,action.depth,child->states,{});pending.push_back(child);
            }
            // Scheduled entries count against a separate pending pool until started.
            size_t previous=tasks.size();for(auto&child:pending)tasks.push_back(child);
            if(!boundedValues()||!boundedFrames()){tasks.resize(previous);return;}
            for(auto&child:pending){child->id=newId();if(output)output("Started job "+std::to_string(child->id));}
            event.emitted=false;return;
        }
        if(n=="do"||n=="if"){
            Body body=s->body;
            if(n=="if"){
                Value cond=eval(args[0],scope,event);double truth=numericValue(cond,error);if(!error.empty())return;
                body=truth!=0?s->body:s->alternate;
            }
            auto original=std::make_shared<Event>(event);continuation();pushBody(task,body,action.scope,original,action.depth,action.states,action.event,exits());return;
        }
        if(n=="where"){
            Value cond=eval(args[0],scope,event);double truth=numericValue(cond,error);if(!error.empty())return;
            if(truth==0){event.emitted=false;return;}continuation();return;
        }
        if(n=="set"){
            auto key=args[0].text;if(scope.params.count(key)||event.fields.count(key)){error="assignment conflicts with parameter or event field '"+key+"'";return;}
            Value value=eval(args[1],scope,event);valueBytes(value);
            if(globals->vars.size()>=32&&!globals->vars.count(key))error="global variable limit is 32";
            if(!error.empty())return;
            auto prior=globals->vars;Event priorEvent=event;globals->vars[key]=value;event.value=value;event.hasValue=true;
            if(!boundedValues()){globals->vars=prior;event=priorEvent;return;}
            continuation();return;
        }
        auto fn=functions.find(n);
        if(fn!=functions.end()){
            if(action.depth>=8){error="function call depth exceeds 8";return;}
            std::vector<Token>ordered;if(!arguments(args,fn->second.params,ordered))return;
            auto local=std::make_shared<Scope>();local->global=globals;local->functionName=n;
            for(size_t i=0;i<ordered.size();++i){auto key=fn->second.params[i];if(event.fields.count(key)||globals->vars.count(key)){error="parameter conflicts with global or event field '"+key+"'";return;}local->params[key]=eval(ordered[i],scope,event);}
            if(!error.empty())return;
            if(!out.empty()){auto binding=std::make_shared<Stage>();binding->name="__bind";binding->args={{out,false,s->line}};binding->line=s->line;action.pipe.insert(action.pipe.begin(),binding);}
            auto input=std::make_shared<Event>(event);continuation();pushBody(task,fn->second.body,local,input,action.depth+1,std::make_shared<StageStates>(),action.event,exits());return;
        }
        if(n=="each"){
            if(!event.hasValue||event.value.kind!=Value::LIST){error="each requires a list";return;}
            continuation();
            Action iterate=action;iterate.pipe.clear();iterate.iteration=s;iterate.index=0;iterate.out=out;task.work.push_front(iterate);return;
        }
        std::vector<Value>values;for(auto&a:args){auto v=eval(a,scope,event);v.option=!a.quoted&&a.text.compare(0,2,"--")==0;values.push_back(v);}if(!error.empty())return;
        if(valueFunction(n)){event.value=values[0];event.hasValue=true;}
        else if(n=="split"){
            Value input=values.size()==3?values[2]:values.size()==2?values[1]:event.value;
            if(values.size()==1&&!event.hasValue){error="split requires a value";return;}
            if(values[0].kind!=Value::TEXT||input.kind!=Value::TEXT){error="split requires strings";return;}
            auto delim=values[0].text;if(delim.empty()){error="split delimiter cannot be empty";return;}
            Value list;list.kind=Value::LIST;size_t start=0;
            do{size_t end=input.text.find(delim,start);list.list.emplace_back(input.text.substr(start,end==std::string::npos?end:end-start));if(list.list.size()>64){error="list limit is 64";return;}if(end==std::string::npos)break;start=end+delim.size();}while(true);
            event.value=list;event.hasValue=true;
        }else if(n=="toggle"||n=="edge"||n=="rising"||n=="falling"||n=="divide"||n=="debounce"||n=="pulse"){
            auto &state=(*action.states)[s.get()];
            if(n=="toggle"){state.value=1-state.value;event.value=Value(state.value);event.hasValue=true;}
            else if(n=="pulse"){
                uint32_t duration=milliseconds(values[0]);if(!error.empty())return;
                auto low=std::make_shared<Event>(event);low->value=Value(0);low->hasValue=true;
                bool retrigger=task.timers.count({s,action.states})!=0;
                Action pending=action;pending.event=low;
                pending.timerOrder=retrigger?task.timers[{s,action.states}].second.timerOrder:++task.timerSequence;
                if(!out.empty()){
                    auto binding=std::make_shared<Stage>();binding->name="__bind";binding->args={{out,false,s->line}};binding->line=s->line;pending.pipe.insert(pending.pipe.begin(),binding);
                }
                task.timers[{s,action.states}]={now+duration,pending};
                if(retrigger){event.emitted=false;return;}
                event.value=Value(1);event.hasValue=true;
            }else{
                if(!event.hasValue){error=n+" requires Boolean 0 or 1";return;}
                double input=numericValue(event.value,error);if(!error.empty())return;
                if(input!=0&&input!=1){error=n+" requires Boolean 0 or 1";return;}
                int value=(int)input;
                if(n=="debounce"){
                    uint32_t duration=milliseconds(values[0]);if(!error.empty())return;
                    if(state.previous<0||state.previous!=value){
                        state.previous=value;
                        Action pending=action;pending.event=std::make_shared<Event>(event);
                        pending.timerOrder=task.timers.count({s,action.states})?task.timers[{s,action.states}].second.timerOrder:++task.timerSequence;
                        task.timers[{s,action.states}]={now+duration,pending};
                    }
                    event.emitted=false;return;
                }else if(n=="divide"){
                    uint32_t divisor=milliseconds(values[0]);if(!error.empty())return;
                    bool initial=state.previous<0,emit=initial;
                    if(!initial&&value!=state.previous&&++state.count>=(int)divisor){state.count=0;state.value=1-state.value;emit=true;}
                    state.previous=value;event.value=Value(state.value);
                    if(!emit){event.emitted=false;return;}
                }else{
                    bool emit=state.previous>=0&&value!=state.previous&&(n=="edge"||(n=="rising"&&value==1)||(n=="falling"&&value==0));state.previous=value;
                    if(!emit){event.emitted=false;return;}
                }
            }
        }else if(n=="sleep"||n=="tone"){
            uint32_t duration=0;
            if(n=="sleep")duration=milliseconds(values[0],true);
            else{
                duration=milliseconds(values[4],true);
                double pinValue=numericValue(values[0],error),frequency=numericValue(values[2],error);
                if(!error.empty()||pinValue!=std::floor(pinValue)||pinValue<0||pinValue>16||frequency<0.01||frequency>1000000){if(error.empty())error="invalid tone pin/frequency";return;}
                int pin=(int)pinValue;
                if(pin!=0&&pin!=2&&pin!=4&&pin!=5&&pin!=12&&pin!=13&&pin!=14&&pin!=15&&pin!=16){error="invalid tone GPIO";return;}
                if(claim&&!claim(pin,task.id)){error="tone pin is reserved";return;}
                if(duration){task.tonePin=pin;task.toneState=0;if(write)write(pin,0);task.halfPeriod=std::max((uint32_t)1,(uint32_t)std::floor(500000.0/frequency+0.5));task.edge=us+task.halfPeriod;}

            }
            if(!error.empty())return;
            if(duration){task.waiting=true;task.wake=now+duration;}
        }else{
            if(!host||!host(n,values,event,task.id,error)){if(error.empty())error="command failed";return;}
            if(std::none_of(tasks.begin(),tasks.end(),[&](const std::shared_ptr<Task>&t){return t.get()==&task;})){event.emitted=false;return;}
            if(n=="adc"||n=="read"||n=="map"||n=="not"||n=="ohm"||n=="shiftin")event.hasValue=true;
        }
        if(event.hasValue)valueBytes(event.value);
        if(!out.empty()&&error.empty())bind(event,scope,out);
        if(error.empty())continuation();
    }
    void warnings(const Body &body,std::vector<const Stage*>&seen){
        for(auto&p:body){bool repeating=false;for(auto&s:p){
            bool source=s->name=="every"||s->name=="clock"||(s->name=="adc"&&s->args.size()>=2&&s->args[1].text=="--watch");
            if(source&&repeating&&std::find(seen.begin(),seen.end(),s.get())==seen.end()){
                seen.push_back(s.get());if(output)output("Warning at line "+std::to_string(s->line)+": each incoming event starts a new "+s->name+" job.");
            }
            repeating=repeating||source;warnings(s->body,seen);warnings(s->alternate,seen);
        }}
    }
    void finish(const std::shared_ptr<Task>&task){task->work.clear();task->timers.clear();task->waiting=false;task->periodic=false;task->blockedGroup.reset();if(release)release(task->id);tasks.erase(std::remove(tasks.begin(),tasks.end(),task),tasks.end());}
public:
    bool submit(const std::string &source,uint32_t ms,uint32_t micros,
                std::shared_ptr<Event> input = {}, std::shared_ptr<Event> result = {}){
        error.clear();errorLine=1;Parser parser(error);Body body=parser.parse(source);auto defs=functions;
        if(!error.empty()||!definitions(body,defs)||!validate(body,defs)||!boundedPrograms(defs,body)){error=located(error,errorLine);return false;}
        bool runnable=false;for(auto&p:body)if(!p.empty()&&p[0]->name!="func")runnable=true;
        size_t running=0;for(auto&t:tasks)if(!t->scheduled)++running;
        if(runnable&&running>=8){error=located("script job limit is 8",body.front().front()->line);return false;}
        std::vector<const Stage*>warned;warnings(body,warned);
        functions=defs;if(!runnable){if(output)output("OK");return true;}
        // Definitions have already been registered; do not allocate runtime
        // events/continuations for them. Normalized arguments likewise replace
        // lexical vectors above, releasing their oversized parsing capacity.
        Body executable;
        for (const auto &pipe : body) if (!pipe.empty() && pipe[0]->name != "func") executable.push_back(pipe);
        auto task=std::make_shared<Task>();task->id=newId();task->label="script";task->scope=globals;
        if (!input) input=std::make_shared<Event>();
        if (input->hasValue) valueBytes(input->value);
        if (!error.empty()) { error=located(error,1); return false; }
        pushBody(*task,executable,globals,input,0,task->states,result);tasks.push_back(task);
        if (!boundedValues()) { finish(task); error=located(error,1); return false; }
        if(output)output("Started job "+std::to_string(task->id));tick(ms,micros);return true;
    }
    void tick(uint32_t ms,uint32_t micros){
        now=ms;us=micros;auto snapshot=tasks;
        for(auto&task:snapshot){
            if(std::find(tasks.begin(),tasks.end(),task)==tasks.end())continue;error.clear();
            if(task->scheduled){
                if(!due(now,task->wake))continue;
                size_t running=0;for(auto&t:tasks)if(!t->scheduled)++running;
                if(running>=8)error="script job limit is 8 at scheduled start";
                else{task->scheduled=false;for(auto&a:task->work)if(a.event)a.event->timestamp=now;}
            }
            if(task->waiting&&error.empty()){
                if(!due(now,task->wake)){
                    if(task->tonePin>=0&&due(us,task->edge)){
                        uint32_t skipped=(us-task->edge)/task->halfPeriod;task->toneState^=(int)((skipped+1)&1);if(write)write(task->tonePin,task->toneState);task->edge+=(skipped+1)*task->halfPeriod;
                    }continue;
                }
                task->waiting=false;task->tonePin=-1;
            }
            if((task->work.empty()||task->blockedGroup)&&error.empty()){
                using TimerKey=std::pair<StagePtr,std::shared_ptr<StageStates>>;
                std::vector<TimerKey>ready;
                for(auto &timer:task->timers)if(due(now,timer.second.first))ready.push_back(timer.first);
                std::stable_sort(ready.begin(),ready.end(),[&](const TimerKey&a,const TimerKey&b){auto &left=task->timers[a],&right=task->timers[b];return left.first==right.first?left.second.timerOrder<right.second.timerOrder:(int32_t)(left.first-right.first)<0;});
                std::deque<Action> delivered;
                for(auto &key:ready){
                    auto it=task->timers.find(key);
                    {
                        Action pending=it->second.second;
                        auto &st=(*pending.states)[it->first.first.get()];
                        // Debounce emits only a newly accepted stable level; pulse low always emits.
                        bool debounce=it->first.first->name=="debounce";
                        int accepted=debounce?(int)numericValue(pending.event->value,error):0;
                        bool emit=!debounce||st.count!=accepted+1;
                        if(debounce)st.count=accepted+1;
                        if(emit){
                            delivered.push_back(pending);
                            for(size_t i=0;i<pending.exits.size();++i){auto next=pending.exits[i];next.event=pending.event;next.exits.assign(pending.exits.begin()+i+1,pending.exits.end());delivered.push_back(next);}
                        }
                        task->timers.erase(it);
                    }
                }
                if(!delivered.empty()){
                    task->work.insert(task->work.begin(),delivered.begin(),delivered.end());
                    task->blockedGroup.reset();
                }else if(task->blockedGroup){
                    bool pending=false;for(auto &timer:task->timers)if(timer.second.second.event->completion==task->blockedGroup)pending=true;
                    if(!pending)task->blockedGroup.reset();
                }
            }
            if(task->periodic&&task->bounded&&due(now,task->end))task->periodic=false;
            if(task->work.empty()&&task->periodic&&error.empty()){
                uint32_t current=task->clockSource?us:now,interval=task->interval;
                if((!task->first||task->clockSource) && due(current,task->next) && current!=task->next){
                    uint32_t skipped=(current-task->next)/interval+1;task->next+=skipped*interval;task->sequence+=skipped;
                }
                if((task->first&&!task->clockSource)||current==task->next){
                    auto ev=std::make_shared<Event>();ev->timestamp=now;ev->hasValue=true;
                    if(task->clockSource){uint32_t skipped=(current-task->next)/interval;task->sequence+=skipped;task->next+=(skipped+1)*interval;ev->value=Value((task->sequence++%2)==0?1:0);}
                    else{task->next=task->registered+((now-task->registered)/interval+1)*interval;ev->value=Value(0);}
                    task->first=false;
                    if(task->watchSource){
                        if(!host||!host("adc",{Value(std::string("A0"))},*ev,task->id,error)){if(error.empty())error="adc watch failed";}
                        else if(task->lastSample==(int)ev->value.number)ev->emitted=false;
                        else task->lastSample=(int)ev->value.number;
                    }
                    if(!task->sourceOut.empty()&&error.empty())bind(*ev,*task->scope,task->sourceOut);
                    enqueue(*task,task->repeat,task->scope,ev,0,task->states);
                }
            }
            for(int budget=0;budget<32&&!task->work.empty()&&!task->waiting&&!task->blockedGroup&&error.empty()&&std::find(tasks.begin(),tasks.end(),task)!=tasks.end();++budget){step(*task);if(error.empty())boundedFrames();if(error.empty())boundedPrograms(functions,{});if(error.empty())boundedValues();}
            if(!error.empty()){
                error=located(error,task->line);if(!task->functionName.empty())error+=" in function '"+task->functionName+"'";failures.push_back({task->id,task->label,error});if(failures.size()>8)failures.pop_front();if(output)output("ERROR: job "+std::to_string(task->id)+": "+error);finish(task);
            }else if(task->work.empty()&&!task->waiting&&!task->periodic&&task->timers.empty())finish(task);
        }
    }
    bool kill(unsigned long id){for(auto&task:tasks)if(task->id==id){auto copy=task;finish(copy);return true;}return false;}
    void reset(){while(!tasks.empty())finish(tasks.back());globals->vars.clear();failures.clear();}
};
} // namespace cw
