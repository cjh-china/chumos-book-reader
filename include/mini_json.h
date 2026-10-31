#ifndef MINI_JSON_H
#define MINI_JSON_H

#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace minijson {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<Value> arr;
    std::map<std::string, Value> obj;

    const Value& operator[](const std::string& key) const {
        static const Value nullVal;
        auto it = obj.find(key);
        return it != obj.end() ? it->second : nullVal;
    }
    const Value& operator[](size_t i) const {
        static const Value nullVal;
        return i < arr.size() ? arr[i] : nullVal;
    }
    size_t size() const { return arr.size(); }
    bool has(const std::string& key) const { return obj.find(key) != obj.end(); }

    std::string asString() const { return type == String ? str : ""; }
    double asNumber() const { return type == Number ? num : 0; }
    bool asBool() const { return type == Bool ? b : false; }

    std::string toStringValue() const {
        switch (type) {
            case String: return str;
            case Number: {
                double d = num;
                if (d == (double)(long long)d) return std::to_string((long long)d);
                std::ostringstream os; os << d; return os.str();
            }
            case Bool: return b ? "true" : "false";
            default: return "";
        }
    }
};

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s), pos_(0) {}
    bool parse(Value& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        return true;
    }
private:
    const std::string& s_;
    size_t pos_;

    void skipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') pos_++;
            else break;
        }
    }

    static void encodeUtf8(std::string& out, unsigned int cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool parseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        pos_++;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        if (pos_ + 4 > s_.size()) return false;
                        unsigned int cp = (unsigned int)std::stoul(s_.substr(pos_, 4), nullptr, 16);
                        pos_ += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size()
                            && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                            unsigned int lo = (unsigned int)std::stoul(s_.substr(pos_ + 2, 4), nullptr, 16);
                            pos_ += 6;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        encodeUtf8(out, cp);
                        break;
                    }
                    default: return false;
                }
            } else {
                out += c;
            }
        }
        return false;
    }

    bool parseNumber(Value& out) {
        out.type = Value::Number;
        size_t start = pos_;
        if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) pos_++;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (std::isdigit((unsigned char)c) || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') pos_++;
            else break;
        }
        if (pos_ == start) return false;
        try { out.num = std::stod(s_.substr(start, pos_ - start)); }
        catch (...) { out.num = 0; }
        return true;
    }

    bool parseObject(Value& out) {
        out.type = Value::Object;
        pos_++;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == '}') { pos_++; return true; }
        while (pos_ < s_.size()) {
            skipWs();
            std::string key;
            if (!parseString(key)) return false;
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != ':') return false;
            pos_++;
            Value v;
            if (!parseValue(v)) return false;
            out.obj[key] = std::move(v);
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { pos_++; continue; }
            if (pos_ < s_.size() && s_[pos_] == '}') { pos_++; return true; }
            return false;
        }
        return false;
    }

    bool parseArray(Value& out) {
        out.type = Value::Array;
        pos_++;
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == ']') { pos_++; return true; }
        while (pos_ < s_.size()) {
            Value v;
            if (!parseValue(v)) return false;
            out.arr.push_back(std::move(v));
            skipWs();
            if (pos_ < s_.size() && s_[pos_] == ',') { pos_++; continue; }
            if (pos_ < s_.size() && s_[pos_] == ']') { pos_++; return true; }
            return false;
        }
        return false;
    }

    bool parseValue(Value& out) {
        skipWs();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') { out.type = Value::String; return parseString(out.str); }
        if (c == 't') { if (s_.compare(pos_, 4, "true") == 0) { pos_ += 4; out.type = Value::Bool; out.b = true; return true; } return false; }
        if (c == 'f') { if (s_.compare(pos_, 5, "false") == 0) { pos_ += 5; out.type = Value::Bool; out.b = false; return true; } return false; }
        if (c == 'n') { if (s_.compare(pos_, 4, "null") == 0) { pos_ += 4; out.type = Value::Null; return true; } return false; }
        return parseNumber(out);
    }
};

inline Value parse(const std::string& s) {
    Value v;
    Parser p(s);
    p.parse(v);
    return v;
}

} // namespace minijson

#endif
