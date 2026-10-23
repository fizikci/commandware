#include "ScriptRuntime.h"
#include <iostream>
#include <iterator>
int main() {
    std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    cw::Runtime runtime;
    unsigned long id = 0;
    runtime.newId = [&]() { return ++id; };
    runtime.release = [](unsigned long) {};
    runtime.output = [](const std::string &) {};
    runtime.claim = [](int, unsigned long) { return true; };
    runtime.write = [](int, int) {};
    runtime.host = [&](const std::string &name, const std::vector<cw::Value> &args,
                        cw::Event &event, unsigned long job, std::string &error) {
        if (name == "reset") { runtime.reset(); return true; }
        if (name == "kill") {
            double target = cw::numericValue(args[0], error);
            if (target != std::floor(target) || target < 1 || !runtime.kill((unsigned long)target)) error = "unknown job ID";
            return error.empty();
        }
        if (name == "print") {
            if (args.empty() && !event.hasValue) { error = "pipeline value is required"; return false; }
            const auto &value = args.empty() ? event.value : args.back();
            std::cout << cw::formatValue(value) << '\n'; return true;
        }
        if (name == "adc") { event.value = cw::Value(0); return true; }
        if (name == "write") return true;
        if (name == "not") {
            if (args.empty() && !event.hasValue) { error = "pipeline value is required"; return false; }
            event.value = cw::Value(cw::numericValue(args.empty() ? event.value : args.back(), error) == 0);
            return error.empty();
        }
        error = "unsupported conformance host command " + name + " job=" + std::to_string(job); return false;
    };
    size_t start = 0;
    const std::string separator = "\n---NEXT---\n";
    do {
        size_t end = source.find(separator, start);
        if (!runtime.submit(source.substr(start, end == std::string::npos ? end : end-start), 0, 0)) {
            std::cout << "ERROR:" << runtime.error; return 0;
        }
        if (end == std::string::npos) break;
        start = end + separator.size();
    } while (true);
    for (unsigned ms = 0; ms <= 200 && !runtime.tasks.empty(); ++ms) runtime.tick(ms, ms*1000);
    if (!runtime.failures.empty()) std::cout << "ERROR:" << runtime.failures.back().message;
}
