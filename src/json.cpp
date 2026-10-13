#include "json.h"

#include <cstdlib>
#include <cstdio>

namespace st {
namespace {

void AppendUtf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class Parser {
public:
    Parser(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        if (p_ != end_) {
            error_ = "trailing characters";
            return false;
        }
        return true;
    }

    const std::string& error() const { return error_; }

private:
    void SkipWs() {
        while (p_ != end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    bool ParseValue(JsonValue& out, int depth) {
        if (depth > 64) {
            error_ = "nesting too deep";
            return false;
        }
        SkipWs();
        if (p_ == end_) {
            error_ = "unexpected end of input";
            return false;
        }
        const char c = *p_;
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return ParseString(out.str);
        }
        if (c == 't' || c == 'f') return ParseBool(out);
        if (c == 'n') {
            if (end_ - p_ >= 4 && std::string(p_, p_ + 4) == "null") {
                p_ += 4;
                out.type = JsonValue::Type::Null;
                return true;
            }
            error_ = "invalid literal";
            return false;
        }
        return ParseNumber(out);
    }

    bool ParseObject(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        ++p_;  // {
        SkipWs();
        if (p_ != end_ && *p_ == '}') {
            ++p_;
            return true;
        }
        for (;;) {
            SkipWs();
            std::string key;
            if (p_ == end_ || *p_ != '"') {
                error_ = "expected object key";
                return false;
            }
            if (!ParseString(key)) return false;
            SkipWs();
            if (p_ == end_ || *p_ != ':') {
                error_ = "expected ':'";
                return false;
            }
            ++p_;
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.members.emplace_back(std::move(key), std::move(value));
            SkipWs();
            if (p_ == end_) {
                error_ = "unterminated object";
                return false;
            }
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            if (*p_ == '}') {
                ++p_;
                return true;
            }
            error_ = "expected ',' or '}'";
            return false;
        }
    }

    bool ParseArray(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        ++p_;  // [
        SkipWs();
        if (p_ != end_ && *p_ == ']') {
            ++p_;
            return true;
        }
        for (;;) {
            JsonValue value;
            if (!ParseValue(value, depth + 1)) return false;
            out.items.push_back(std::move(value));
            SkipWs();
            if (p_ == end_) {
                error_ = "unterminated array";
                return false;
            }
            if (*p_ == ',') {
                ++p_;
                continue;
            }
            if (*p_ == ']') {
                ++p_;
                return true;
            }
            error_ = "expected ',' or ']'";
            return false;
        }
    }

    bool ParseString(std::string& out) {
        ++p_;  // opening quote
        out.clear();
        while (p_ != end_) {
            const char c = *p_++;
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ == end_) break;
            const char esc = *p_++;
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' &&
                        p_[1] == 'u') {
                        p_ += 2;
                        unsigned int low = 0;
                        if (!ParseHex4(low)) return false;
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default:
                    error_ = "bad escape";
                    return false;
            }
        }
        error_ = "unterminated string";
        return false;
    }

    bool ParseHex4(unsigned int& out) {
        if (end_ - p_ < 4) {
            error_ = "bad \\u escape";
            return false;
        }
        unsigned int v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned int>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned int>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned int>(c - 'A' + 10);
            else {
                error_ = "bad hex digit";
                return false;
            }
        }
        out = v;
        return true;
    }

    bool ParseBool(JsonValue& out) {
        if (end_ - p_ >= 4 && std::string(p_, p_ + 4) == "true") {
            p_ += 4;
            out.type = JsonValue::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (end_ - p_ >= 5 && std::string(p_, p_ + 5) == "false") {
            p_ += 5;
            out.type = JsonValue::Type::Bool;
            out.boolean = false;
            return true;
        }
        error_ = "invalid literal";
        return false;
    }

    bool ParseNumber(JsonValue& out) {
        char* stop = nullptr;
        const double v = std::strtod(p_, &stop);
        if (stop == p_) {
            error_ = "invalid number";
            return false;
        }
        p_ = stop;
        out.type = JsonValue::Type::Number;
        out.number = v;
        return true;
    }

    const char* p_;
    const char* end_;
    std::string error_;
};

}  // namespace

const JsonValue* JsonValue::find(const std::string& key) const {
    for (const auto& kv : members) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

std::string JsonValue::strOr(const std::string& fallback) const {
    if (type == Type::String) return str;
    if (type == Type::Number) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%g", number);
        return std::string(buf);
    }
    return fallback;
}

double JsonValue::numOr(double fallback) const {
    if (type == Type::Number) return number;
    if (type == Type::String && !str.empty()) {
        char* stop = nullptr;
        const double v = std::strtod(str.c_str(), &stop);
        if (stop != str.c_str()) return v;
    }
    return fallback;
}

bool JsonParse(const std::string& text, JsonValue& out, std::string* error) {
    Parser parser(text);
    if (!parser.Parse(out)) {
        if (error) *error = parser.error();
        return false;
    }
    return true;
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (u < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", u);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

}  // namespace st
