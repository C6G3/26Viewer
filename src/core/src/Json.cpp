#include "viewer/core/Json.hpp"

#include <charconv>
#include <cmath>
#include <format>

namespace viewer::json {

namespace {

const Value kNull;
const Array kEmptyArray;
const Object kEmptyObject;

class Reader {
public:
    explicit Reader(std::string_view text) : m_s(text) {}

    Result<Value> document()
    {
        auto v = value(0);
        if (!v)
            return v;
        space();
        if (m_at != m_s.size())
            return error("unexpected text after the value");
        return v;
    }

private:
    static constexpr int kMaxDepth = 200;

    std::unexpected<Error> error(std::string_view what) const
    {
        return fail(ErrorCode::InvalidFormat, "JSON: {} at character {}", what, m_at);
    }

    void space()
    {
        while (m_at < m_s.size() && (m_s[m_at] == ' ' || m_s[m_at] == '\t' || m_s[m_at] == '\n' || m_s[m_at] == '\r'))
            ++m_at;
    }

    bool literal(std::string_view word)
    {
        if (m_s.substr(m_at, word.size()) != word)
            return false;
        m_at += word.size();
        return true;
    }

    Result<Value> value(int depth)
    {
        if (depth > kMaxDepth)
            return error("nested too deeply");
        space();
        if (m_at >= m_s.size())
            return error("a value was expected");
        const char c = m_s[m_at];
        if (c == '{')
            return object(depth);
        if (c == '[')
            return array(depth);
        if (c == '"') {
            auto s = string();
            if (!s)
                return std::unexpected(s.error());
            return Value(std::move(*s));
        }
        if (literal("true"))
            return Value(true);
        if (literal("false"))
            return Value(false);
        if (literal("null"))
            return Value(nullptr);
        if (c == '-' || (c >= '0' && c <= '9'))
            return number();
        return error("a value was expected");
    }

    Result<Value> number()
    {
        const std::size_t start = m_at;
        if (m_s[m_at] == '-')
            ++m_at;
        while (m_at < m_s.size() && std::string_view("0123456789.eE+-").find(m_s[m_at]) != std::string_view::npos)
            ++m_at;
        double d = 0;
        const auto [end, ec] = std::from_chars(m_s.data() + start, m_s.data() + m_at, d);
        if (ec != std::errc() || end != m_s.data() + m_at)
            return error("bad number");
        return Value(d);
    }

    static void appendUtf8(std::string& out, std::uint32_t c)
    {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }

    Result<std::uint32_t> hex4()
    {
        if (m_at + 4 > m_s.size())
            return error("short \\u escape");
        std::uint32_t v = 0;
        const auto [end, ec] = std::from_chars(m_s.data() + m_at, m_s.data() + m_at + 4, v, 16);
        if (ec != std::errc() || end != m_s.data() + m_at + 4)
            return error("bad \\u escape");
        m_at += 4;
        return v;
    }

    Result<std::string> string()
    {
        ++m_at; // "
        std::string out;
        while (true) {
            if (m_at >= m_s.size())
                return error("unterminated string");
            const char c = m_s[m_at++];
            if (c == '"')
                return out;
            if (static_cast<unsigned char>(c) < 0x20)
                return error("control character in a string");
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (m_at >= m_s.size())
                return error("unterminated string");
            switch (m_s[m_at++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto u = hex4();
                if (!u)
                    return std::unexpected(u.error());
                std::uint32_t cp = *u;
                if (cp >= 0xD800 && cp < 0xDC00 && m_s.substr(m_at, 2) == "\\u") {
                    m_at += 2;
                    auto low = hex4();
                    if (!low)
                        return std::unexpected(low.error());
                    if (*low >= 0xDC00 && *low < 0xE000)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
                    else
                        return error("bad surrogate pair");
                }
                appendUtf8(out, cp);
                break;
            }
            default: return error("bad escape");
            }
        }
    }

    Result<Value> array(int depth)
    {
        ++m_at; // [
        Array out;
        space();
        if (m_at < m_s.size() && m_s[m_at] == ']') {
            ++m_at;
            return Value(std::move(out));
        }
        while (true) {
            auto v = value(depth + 1);
            if (!v)
                return v;
            out.push_back(std::move(*v));
            space();
            if (m_at < m_s.size() && m_s[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_s.size() && m_s[m_at] == ']') {
                ++m_at;
                return Value(std::move(out));
            }
            return error("',' or ']' expected");
        }
    }

    Result<Value> object(int depth)
    {
        ++m_at; // {
        Object out;
        space();
        if (m_at < m_s.size() && m_s[m_at] == '}') {
            ++m_at;
            return Value(std::move(out));
        }
        while (true) {
            space();
            if (m_at >= m_s.size() || m_s[m_at] != '"')
                return error("a member name was expected");
            auto key = string();
            if (!key)
                return std::unexpected(key.error());
            space();
            if (m_at >= m_s.size() || m_s[m_at] != ':')
                return error("':' expected");
            ++m_at;
            auto v = value(depth + 1);
            if (!v)
                return v;
            out.emplace_back(std::move(*key), std::move(*v));
            space();
            if (m_at < m_s.size() && m_s[m_at] == ',') {
                ++m_at;
                continue;
            }
            if (m_at < m_s.size() && m_s[m_at] == '}') {
                ++m_at;
                return Value(std::move(out));
            }
            return error("',' or '}' expected");
        }
    }

    std::string_view m_s;
    std::size_t m_at = 0;
};

void writeString(std::string& out, std::string_view s)
{
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            else
                out.push_back(c);
        }
    }
    out.push_back('"');
}

void writeValue(std::string& out, const Value& v)
{
    if (v.isNull()) {
        out += "null";
    } else if (v.isBool()) {
        out += v.asBool() ? "true" : "false";
    } else if (v.isNumber()) {
        const double d = v.asNumber();
        if (!std::isfinite(d))
            out += "null";
        else if (d == std::floor(d) && std::abs(d) < 9007199254740992.0)
            out += std::format("{}", static_cast<std::int64_t>(d));
        else
            out += std::format("{}", d);
    } else if (v.isString()) {
        writeString(out, v.asString());
    } else if (v.isArray()) {
        out.push_back('[');
        bool first = true;
        for (const Value& item : v.asArray()) {
            if (!first)
                out.push_back(',');
            first = false;
            writeValue(out, item);
        }
        out.push_back(']');
    } else {
        out.push_back('{');
        bool first = true;
        for (const auto& [key, item] : v.asObject()) {
            if (!first)
                out.push_back(',');
            first = false;
            writeString(out, key);
            out.push_back(':');
            writeValue(out, item);
        }
        out.push_back('}');
    }
}

} // namespace

const Array& Value::asArray() const noexcept
{
    return isArray() ? std::get<Array>(m_v) : kEmptyArray;
}

const Object& Value::asObject() const noexcept
{
    return isObject() ? std::get<Object>(m_v) : kEmptyObject;
}

const Value& Value::operator[](std::string_view key) const noexcept
{
    for (const auto& [k, v] : asObject())
        if (k == key)
            return v;
    return kNull;
}

Value& Value::set(std::string_view key, Value value)
{
    if (!isObject())
        m_v = Object{};
    auto& members = std::get<Object>(m_v);
    for (auto& [k, v] : members)
        if (k == key) {
            v = std::move(value);
            return v;
        }
    members.emplace_back(std::string(key), std::move(value));
    return members.back().second;
}

Result<Value> parse(std::string_view text)
{
    return Reader(text).document();
}

std::string write(const Value& value)
{
    std::string out;
    writeValue(out, value);
    return out;
}

} // namespace viewer::json
