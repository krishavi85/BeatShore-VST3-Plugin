#pragma once
// Minimal JSON value/parser/writer for the BeatShore bridge protocol.
// Not a general-purpose JSON library -- just enough for the flat objects
// and simple arrays PROTOCOL.md's messages actually use, shared by
// BeatShoreDesktop and the plugin's IPC client (and the test client) so the
// protocol's wire format has exactly one implementation, not three.
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <sstream>
#include <cctype>
#include <stdexcept>

namespace bsjson {

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value
{
    Type type = Type::Null;
    bool boolVal = false;
    double numVal = 0;
    std::string strVal;
    std::vector<Value> arrVal;
    std::vector<std::pair<std::string, Value>> objVal; // preserves insertion order

    Value() = default;
    Value(std::nullptr_t) : type(Type::Null) {}
    Value(bool b) : type(Type::Bool), boolVal(b) {}
    Value(double d) : type(Type::Number), numVal(d) {}
    Value(int i) : type(Type::Number), numVal(double(i)) {}
    Value(unsigned i) : type(Type::Number), numVal(double(i)) {}
    Value(const std::string& s) : type(Type::String), strVal(s) {}
    Value(const char* s) : type(Type::String), strVal(s) {}

    static Value object() { Value v; v.type = Type::Object; return v; }
    static Value array() { Value v; v.type = Type::Array; return v; }

    void set(const std::string& key, Value v)
    {
        for (auto& kv : objVal) if (kv.first == key) { kv.second = std::move(v); return; }
        objVal.emplace_back(key, std::move(v));
    }
    void push(Value v) { arrVal.push_back(std::move(v)); }

    bool has(const std::string& key) const
    {
        for (auto& kv : objVal) if (kv.first == key) return true;
        return false;
    }
    const Value& operator[](const std::string& key) const
    {
        static Value nullVal;
        for (auto& kv : objVal) if (kv.first == key) return kv.second;
        return nullVal;
    }

    std::string asString(const std::string& def = "") const { return type == Type::String ? strVal : def; }
    double asNumber(double def = 0) const { return type == Type::Number ? numVal : def; }
    bool asBool(bool def = false) const { return type == Type::Bool ? boolVal : def; }
};

inline void writeEscapedString(std::ostringstream& out, const std::string& s)
{
    out << '"';
    for (char c : s)
    {
        switch (c)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                }
                else out << c;
        }
    }
    out << '"';
}

inline void writeValue(std::ostringstream& out, const Value& v)
{
    switch (v.type)
    {
        case Type::Null: out << "null"; break;
        case Type::Bool: out << (v.boolVal ? "true" : "false"); break;
        case Type::Number:
        {
            double d = v.numVal;
            if (d == static_cast<long long>(d)) out << static_cast<long long>(d);
            else { std::ostringstream tmp; tmp.precision(9); tmp << d; out << tmp.str(); }
            break;
        }
        case Type::String: writeEscapedString(out, v.strVal); break;
        case Type::Array:
        {
            out << '[';
            for (size_t i = 0; i < v.arrVal.size(); ++i) { if (i) out << ','; writeValue(out, v.arrVal[i]); }
            out << ']';
            break;
        }
        case Type::Object:
        {
            out << '{';
            for (size_t i = 0; i < v.objVal.size(); ++i)
            {
                if (i) out << ',';
                writeEscapedString(out, v.objVal[i].first);
                out << ':';
                writeValue(out, v.objVal[i].second);
            }
            out << '}';
            break;
        }
    }
}

inline std::string stringify(const Value& v)
{
    std::ostringstream out;
    writeValue(out, v);
    return out.str();
}

// --- parser ---

class Parser
{
public:
    explicit Parser(const std::string& text) : s(text), i(0) {}

    // Requires the entire input to be one JSON value plus optional trailing
    // whitespace -- not just a valid *prefix*. Without this, a line like
    // TensorFlow's own diagnostic output ("2026-08-24 00:37:01.772436: I
    // tensorflow/...") parses "successfully" as the bare number 2026, with
    // everything after silently discarded, because parseNumber() only reads
    // as many digits as it can and never checks what's left. That's not
    // hypothetical -- it's exactly what let a stray non-JSON line from
    // Node's stdout (stderr merged in, see NodeEngine::start) get forwarded
    // to a real connected plugin as if it were a protocol message.
    Value parse()
    {
        skipWs();
        Value v = parseValue();
        skipWs();
        if (i != s.size()) throw std::runtime_error("trailing content after JSON value (not fully consumed)");
        return v;
    }

private:
    const std::string& s;
    size_t i;

    void skipWs() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    char peek() const { return i < s.size() ? s[i] : '\0'; }
    char next() { return i < s.size() ? s[i++] : '\0'; }

    Value parseValue()
    {
        skipWs();
        char c = peek();
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value(parseString());
        if (c == 't') { expectLiteral("true"); return Value(true); }
        if (c == 'f') { expectLiteral("false"); return Value(false); }
        if (c == 'n') { expectLiteral("null"); return Value(nullptr); }
        return parseNumber();
    }

    void expectLiteral(const char* lit)
    {
        for (const char* p = lit; *p; ++p)
            if (next() != *p) throw std::runtime_error("bad literal");
    }

    Value parseObject()
    {
        Value v = Value::object();
        next(); // {
        skipWs();
        if (peek() == '}') { next(); return v; }
        while (true)
        {
            skipWs();
            std::string key = parseString();
            skipWs();
            if (next() != ':') throw std::runtime_error("expected ':'");
            Value val = parseValue();
            v.set(key, val);
            skipWs();
            char c = next();
            if (c == '}') break;
            if (c != ',') throw std::runtime_error("expected ',' or '}'");
        }
        return v;
    }

    Value parseArray()
    {
        Value v = Value::array();
        next(); // [
        skipWs();
        if (peek() == ']') { next(); return v; }
        while (true)
        {
            Value val = parseValue();
            v.push(val);
            skipWs();
            char c = next();
            if (c == ']') break;
            if (c != ',') throw std::runtime_error("expected ',' or ']'");
        }
        return v;
    }

    std::string parseString()
    {
        skipWs();
        if (next() != '"') throw std::runtime_error("expected string");
        std::string out;
        while (true)
        {
            char c = next();
            if (c == '"') break;
            if (c == '\0') throw std::runtime_error("unterminated string");
            if (c == '\\')
            {
                char e = next();
                switch (e)
                {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                    {
                        std::string hex = s.substr(i, 4);
                        i += 4;
                        unsigned cp = static_cast<unsigned>(std::stoul(hex, nullptr, 16));
                        if (cp < 0x80) out += static_cast<char>(cp);
                        else if (cp < 0x800)
                        {
                            out += static_cast<char>(0xC0 | (cp >> 6));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        else
                        {
                            out += static_cast<char>(0xE0 | (cp >> 12));
                            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: out += e;
                }
            }
            else out += c;
        }
        return out;
    }

    Value parseNumber()
    {
        size_t start = i;
        if (peek() == '-') next();
        while (std::isdigit(static_cast<unsigned char>(peek()))) next();
        if (peek() == '.') { next(); while (std::isdigit(static_cast<unsigned char>(peek()))) next(); }
        if (peek() == 'e' || peek() == 'E')
        {
            next();
            if (peek() == '+' || peek() == '-') next();
            while (std::isdigit(static_cast<unsigned char>(peek()))) next();
        }
        std::string numStr = s.substr(start, i - start);
        if (numStr.empty()) throw std::runtime_error("expected number");
        return Value(std::stod(numStr));
    }
};

inline Value parse(const std::string& text) { return Parser(text).parse(); }

} // namespace bsjson
