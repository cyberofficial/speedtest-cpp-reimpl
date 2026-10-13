// Tiny JSON reader/writer, only what the engine needs. No third-party code.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace st {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> members;

    bool isArray() const { return type == Type::Array; }
    bool isObject() const { return type == Type::Object; }

    const JsonValue* find(const std::string& key) const;
    std::string strOr(const std::string& fallback = std::string()) const;
    double numOr(double fallback = 0.0) const;  // accepts numbers and numeric strings
};

bool JsonParse(const std::string& text, JsonValue& out, std::string* error);
std::string JsonEscape(const std::string& in);

}  // namespace st
