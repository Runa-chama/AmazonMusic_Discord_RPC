// Minimal JSON parser + helpers used for the iTunes artwork lookup.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }

    const Value* find(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& item : object) {
            if (item.first == key) return &item.second;
        }
        return nullptr;
    }

    const std::string& asString() const { return string; }
};

namespace detail {

inline void appendUtf8(uint32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
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
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> run() {
        skipWhitespace();
        Value value;
        if (!parseValue(value, 0)) return std::nullopt;
        skipWhitespace();
        if (pos_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    bool eof() const { return pos_ >= text_.size(); }
    char peek() const { return eof() ? '\0' : text_[pos_]; }

    void skipWhitespace() {
        while (!eof()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool consume(char expected) {
        if (peek() != expected) return false;
        ++pos_;
        return true;
    }

    bool parseValue(Value& out, int depth) {
        if (depth > 64) return false;
        skipWhitespace();
        switch (peek()) {
        case '{': return parseObject(out, depth + 1);
        case '[': return parseArray(out, depth + 1);
        case '"':
            out.type = Value::Type::String;
            return parseString(out.string);
        case 't':
            if (text_.substr(pos_, 4) == "true") {
                pos_ += 4;
                out.type = Value::Type::Bool;
                out.boolean = true;
                return true;
            }
            return false;
        case 'f':
            if (text_.substr(pos_, 5) == "false") {
                pos_ += 5;
                out.type = Value::Type::Bool;
                out.boolean = false;
                return true;
            }
            return false;
        case 'n':
            if (text_.substr(pos_, 4) == "null") {
                pos_ += 4;
                out.type = Value::Type::Null;
                return true;
            }
            return false;
        default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out, int depth) {
        out.type = Value::Type::Object;
        if (!consume('{')) return false;
        skipWhitespace();
        if (consume('}')) return true;
        while (true) {
            skipWhitespace();
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            Value value;
            if (!parseValue(value, depth)) return false;
            out.object.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (consume(',')) continue;
            if (consume('}')) return true;
            return false;
        }
    }

    bool parseArray(Value& out, int depth) {
        out.type = Value::Type::Array;
        if (!consume('[')) return false;
        skipWhitespace();
        if (consume(']')) return true;
        while (true) {
            Value value;
            if (!parseValue(value, depth)) return false;
            out.array.push_back(std::move(value));
            skipWhitespace();
            if (consume(',')) continue;
            if (consume(']')) return true;
            return false;
        }
    }

    bool parseString(std::string& out) {
        if (!consume('"')) return false;
        out.clear();
        while (!eof()) {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"') return true;
            if (c == '\\') {
                if (eof()) return false;
                const char esc = text_[pos_++];
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
                    uint32_t cp = 0;
                    if (!parseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (pos_ + 1 < text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            uint32_t low = 0;
                            if (!parseHex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                cp = 0xFFFD;
                            }
                        } else {
                            cp = 0xFFFD;
                        }
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        cp = 0xFFFD;
                    }
                    appendUtf8(cp, out);
                    break;
                }
                default: return false;
                }
            } else if (c < 0x20) {
                return false;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return false;
    }

    bool parseHex4(uint32_t& value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) return false;
            const char c = text_[pos_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseNumber(Value& out) {
        const size_t start = pos_;
        if (peek() == '-') ++pos_;
        while (!eof() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if (peek() == '.') {
            ++pos_;
            while (!eof() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            while (!eof() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if (pos_ == start) return false;
        try {
            out.type = Value::Type::Number;
            out.number = std::stod(std::string(text_.substr(start, pos_ - start)));
            return true;
        } catch (...) {
            return false;
        }
    }

    std::string_view text_;
    size_t pos_ = 0;
};

} // namespace detail

inline std::optional<Value> parse(std::string_view text) {
    return detail::Parser(text).run();
}

// Escapes a UTF-8 string for embedding inside a JSON string literal.
inline std::string escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static const char* hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

inline std::string quote(std::string_view text) {
    return "\"" + escape(text) + "\"";
}

// Truncates at a UTF-8 code point boundary (Discord limits text to 128 chars).
inline std::string truncateUtf8(std::string_view text, size_t maxCodepoints) {
    size_t pos = 0;
    size_t count = 0;
    while (pos < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[pos]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (pos + len > text.size()) len = 1;
        if (++count > maxCodepoints) {
            return std::string(text.substr(0, pos)) + "\xE2\x80\xA6"; // U+2026
        }
        pos += len;
    }
    return std::string(text);
}

} // namespace json
