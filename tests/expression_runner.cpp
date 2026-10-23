// Portable conformance adapter: compile the actual firmware expression/runtime code.
#include "ScriptRuntime.h"
#include <iostream>
#include <iterator>
int main(int argc, char **argv) {
    std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    if (argc > 1 && std::string(argv[1]) == "script") {
        cw::Runtime runtime;
        unsigned long id = 0;
        std::vector<std::string> output;
        runtime.newId = [&]() { return ++id; };
        runtime.release = [](unsigned long) {};
        runtime.output = [](const std::string &) {};
        runtime.host = [&](const std::string &name, const std::vector<cw::Value> &args, cw::Event &event,
                           unsigned long, std::string &error) {
            if (name != "print") { error = "unsupported test host command"; return false; }
            if (args.empty() && !event.hasValue) { error = "pipeline value is required"; return false; }
            output.push_back((args.empty() ? event.value : args[0]).str()); return true;
        };
        if (!runtime.submit(source, 0, 0)) { std::cout << "error:" << runtime.error; return 0; }
        for (int i = 0; i < 10 && !runtime.tasks.empty(); ++i) runtime.tick(0, 0);
        if (!runtime.failures.empty()) std::cout << "error:" << runtime.failures.back().message;
        else { std::cout << "ok:"; for (const auto &line : output) std::cout << line << '\n'; }
    } else {
        cw::Scope scope; cw::Event event; std::string error;
        scope.vars["zero"] = cw::Value(0); scope.vars["five"] = cw::Value(5);
        scope.vars["text"] = cw::Value(std::string("$missing"));
        bool syntax = argc > 1 && std::string(argv[1]) == "syntax";
        auto result = cw::Expression(source, scope, event, error, syntax).run();
        std::cout << (error.empty() ? "ok:" + result.str() : "error:" + error);
    }
}
