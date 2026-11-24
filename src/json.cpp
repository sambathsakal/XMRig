#include "json.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace xmr { namespace json {

const Value *Value::find(const std::string &key) const
{
    if (type != Type::Object) {
        return nullptr;
    }
    for (const auto &entry : members) {
        if (entry.first == key) {
            return &entry.second;
        }
    }
    return nullptr;
}

std::string Value::stringValue(const std::string &fallback) const
{
    return (type == Type::String || type == Type::Number) ? text : fallback;
}

bool Value::boolValue(bool fallback) const
{
    return type == Type::Bool ? boolean : fallback;
}

std::int64_t Value::intValue(std::int64_t fallback) const
{
    if (type != Type::Number || text.empty()) {
        return fallback;
    }
    return static_cast<std::int64_t>(std::strtoll(text.c_str(), nullptr, 10));
}

std::uint64_t Value::uintValue(std::uint64_t fallback) const
{
    if (type != Type::Number || text.empty()) {
        return fallback;
    }
    return static_cast<std::uint64_t>(std::strtoull(text.c_str(), nullptr, 10));
}

double Value::doubleValue(double fallback) const
{
    if (type != Type::Number || text.empty()) {
        return fallback;
    }
    return std::strtod(text.c_str(), nullptr);
}

Value Value::null()                    { return Value{}; }
Value Value::booleanValue(bool v)      { Value x; x.type = Type::Bool; x.boolean = v; return x; }
Value Value::number(std::int64_t v)    { Value x; x.type = Type::Number; x.text = std::to_string(v); return x; }
Value Value::number(std::uint64_t v)   { Value x; x.type = Type::Number; x.text = std::to_string(v); return x; }
Value Value::string(std::string v)     { Value x; x.type = Type::String; x.text = std::move(v); return x; }
Value Value::array()                   { Value x; x.type = Type::Array; return x; }
Value Value::object()                  { Value x; x.type = Type::Object; return x; }

void Value::set(std::string key, Value v)
{
    type = Type::Object;
    for (auto &entry : members) {
        if (entry.first == key) {
            entry.second = std::move(v);
            return;
        }
    }
    members.emplace_back(std::move(key), std::move(v));
}

namespace {

void appendUtf8(std::string &out, std::uint32_t cp)
{
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

class Parser {
public:
    Parser(const std::string &input, std::string &error) : m_input(input), m_error(error) {}

    bool run(Value &out)
    {
        skipWs();
        if (!parseValue(out)) {
            return false;
        }
        skipWs();
        if (m_pos != m_input.size()) {
            return fail("trailing characters");
        }
        return true;
    }

private:
    const std::string &m_input;
    std::string &m_error;
    std::size_t m_pos = 0;

    bool fail(const char *message)
    {
        if (m_error.empty()) {
            m_error = std::string(message) + " at byte " + std::to_string(m_pos);
        }
        return false;
    }

    void skipWs()
    {
        while (m_pos < m_input.size()) {
            const char c = m_input[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++m_pos;
            } else {
                break;
            }
        }
    }

    bool literal(const char *word)
    {
        const std::size_t n = std::strlen(word);
        if (m_input.compare(m_pos, n, word) != 0) {
            return false;
        }
        m_pos += n;
        return true;
    }

    bool hex4(std::uint32_t &out)
    {
        if (m_pos + 4 > m_input.size()) {
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = m_input[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9')       out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')  out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')  out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool parseString(std::string &out)
    {
        if (m_pos >= m_input.size() || m_input[m_pos] != '"') {
            return fail("expected string");
        }
        ++m_pos;
        out.clear();

        while (m_pos < m_input.size()) {
            const unsigned char c = static_cast<unsigned char>(m_input[m_pos++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return fail("control character in string");
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }

            if (m_pos >= m_input.size()) {
                return fail("dangling escape");
            }
            const char e = m_input[m_pos++];
            switch (e) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    return fail("bad unicode escape");
                }
                if (cp >= 0xd800 && cp <= 0xdbff && m_pos + 1 < m_input.size() &&
                    m_input[m_pos] == '\\' && m_input[m_pos + 1] == 'u') {
                    m_pos += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low) || low < 0xdc00 || low > 0xdfff) {
                        return fail("bad surrogate pair");
                    }
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("unknown escape");
            }
        }

        return fail("unterminated string");
    }

    bool parseNumber(Value &out)
    {
        const std::size_t start = m_pos;
        if (m_pos < m_input.size() && m_input[m_pos] == '-') {
            ++m_pos;
        }
        if (m_pos >= m_input.size()) {
            return fail("bad number");
        }
        if (m_input[m_pos] == '0') {
            ++m_pos;
        } else if (std::isdigit(static_cast<unsigned char>(m_input[m_pos]))) {
            while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos]))) {
                ++m_pos;
            }
        } else {
            return fail("bad number");
        }
        if (m_pos < m_input.size() && m_input[m_pos] == '.') {
            ++m_pos;
            if (m_pos >= m_input.size() || !std::isdigit(static_cast<unsigned char>(m_input[m_pos]))) {
                return fail("bad fraction");
            }
            while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos]))) {
                ++m_pos;
            }
        }
        if (m_pos < m_input.size() && (m_input[m_pos] == 'e' || m_input[m_pos] == 'E')) {
            ++m_pos;
            if (m_pos < m_input.size() && (m_input[m_pos] == '+' || m_input[m_pos] == '-')) {
                ++m_pos;
            }
            if (m_pos >= m_input.size() || !std::isdigit(static_cast<unsigned char>(m_input[m_pos]))) {
                return fail("bad exponent");
            }
            while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos]))) {
                ++m_pos;
            }
        }
        out = Value::number(std::int64_t{0});
        out.text = m_input.substr(start, m_pos - start);
        return true;
    }

    bool parseValue(Value &out)
    {
        skipWs();
        if (m_pos >= m_input.size()) {
            return fail("unexpected end of input");
        }

        const char c = m_input[m_pos];
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            out = Value::string(std::move(s));
            return true;
        }
        if (c == '{') {
            ++m_pos;
            out = Value::object();
            skipWs();
            if (m_pos < m_input.size() && m_input[m_pos] == '}') {
                ++m_pos;
                return true;
            }
            while (true) {
                skipWs();
                std::string key;
                if (!parseString(key)) return false;
                skipWs();
                if (m_pos >= m_input.size() || m_input[m_pos] != ':') return fail("expected colon");
                ++m_pos;
                Value child;
                if (!parseValue(child)) return false;
                out.set(std::move(key), std::move(child));
                skipWs();
                if (m_pos >= m_input.size()) return fail("unterminated object");
                if (m_input[m_pos] == '}') { ++m_pos; return true; }
                if (m_input[m_pos] != ',') return fail("expected comma");
                ++m_pos;
            }
        }
        if (c == '[') {
            ++m_pos;
            out = Value::array();
            skipWs();
            if (m_pos < m_input.size() && m_input[m_pos] == ']') {
                ++m_pos;
                return true;
            }
            while (true) {
                Value child;
                if (!parseValue(child)) return false;
                out.add(std::move(child));
                skipWs();
                if (m_pos >= m_input.size()) return fail("unterminated array");
                if (m_input[m_pos] == ']') { ++m_pos; return true; }
                if (m_input[m_pos] != ',') return fail("expected comma");
                ++m_pos;
            }
        }
        if (c == 't') {
            if (!literal("true")) return fail("bad literal");
            out = Value::booleanValue(true);
            return true;
        }
        if (c == 'f') {
            if (!literal("false")) return fail("bad literal");
            out = Value::booleanValue(false);
            return true;
        }
        if (c == 'n') {
            if (!literal("null")) return fail("bad literal");
            out = Value::null();
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parseNumber(out);
        }
        return fail("unexpected character");
    }
};

void writeEscaped(const std::string &s, std::string &out)
{
    static const char *digits = "0123456789abcdef";
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(digits[c >> 4]);
                out.push_back(digits[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void dumpInto(const Value &v, int indent, std::string &out)
{
    const auto pad = [&](int levels) { out.append(static_cast<std::size_t>(levels * 2), ' '); };

    switch (v.type) {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += v.boolean ? "true" : "false";
        break;
    case Value::Type::Number:
        out += v.text.empty() ? "0" : v.text;
        break;
    case Value::Type::String:
        writeEscaped(v.text, out);
        break;
    case Value::Type::Array:
        out.push_back('[');
        for (std::size_t i = 0; i < v.items.size(); ++i) {
            if (i) out.push_back(',');
            if (indent >= 0) { out.push_back('\n'); pad(indent + 1); }
            dumpInto(v.items[i], indent < 0 ? -1 : indent + 1, out);
        }
        if (!v.items.empty() && indent >= 0) { out.push_back('\n'); pad(indent); }
        out.push_back(']');
        break;
    case Value::Type::Object:
        out.push_back('{');
        for (std::size_t i = 0; i < v.members.size(); ++i) {
            if (i) out.push_back(',');
            if (indent >= 0) { out.push_back('\n'); pad(indent + 1); }
            writeEscaped(v.members[i].first, out);
            out.push_back(':');
            if (indent >= 0) out.push_back(' ');
            dumpInto(v.members[i].second, indent < 0 ? -1 : indent + 1, out);
        }
        if (!v.members.empty() && indent >= 0) { out.push_back('\n'); pad(indent); }
        out.push_back('}');
        break;
    }
}

} // namespace

Value parse(const std::string &input, std::string &error)
{
    error.clear();
    Value result;
    Parser parser(input, error);
    if (!parser.run(result)) {
        return Value::null();
    }
    return result;
}

std::string dump(const Value &value, int indent)
{
    std::string out;
    dumpInto(value, indent, out);
    return out;
}

}} // namespace xmr::json
