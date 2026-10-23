#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cctype>
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
        char buffer[48]; snprintf(buffer, sizeof(buffer), "%.17g", number == 0 ? 0.0 : number); return buffer;
    }
};
struct Event { Value value; std::map<std::string, Value> fields; bool hasValue = false; };
struct Scope { std::map<std::string, Value> vars, params; std::shared_ptr<Scope> global; };
inline std::string lower(std::string s) { for (char &c : s) c = (char)tolower((unsigned char)c); return s; }
#include "Expression.h"
}
#include <iostream>
#include <iterator>
int main() {
    std::string source((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    cw::Scope scope; cw::Event event; std::string error;
    cw::Value items; items.kind = cw::Value::LIST;
    items.list = {cw::Value(std::string("é")), cw::Value(std::string("雪")), cw::Value(std::string(""))};
    scope.vars["items"] = items;
    items.list = {cw::Value(2), cw::Value(std::string("03"))}; scope.vars["mixed"] = items;
    items.list.clear(); scope.vars["empty"] = items;
    items.list.assign(65, cw::Value(1)); scope.vars["oversized"] = items;
    scope.vars["bad"] = cw::Value(std::string("\xc0\xaf", 2));
    auto result = cw::Expression(source, scope, event, error).run();
    std::cout << (error.empty() ? "ok:" + cw::formatValue(result) : "error:" + error);
}
