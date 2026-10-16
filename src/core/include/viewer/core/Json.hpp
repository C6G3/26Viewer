#pragma once

#include "viewer/core/Error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace viewer::json {

class Value;
using Array = std::vector<Value>;
/// Members in the order they were added (or read).
using Object = std::vector<std::pair<std::string, Value>>;

/// A JSON value (RFC 8259). Numbers are doubles; whole numbers are written
/// without a fraction.
class Value {
public:
    Value() = default; ///< null
    Value(std::nullptr_t) {}
    Value(bool b) : m_v(b) {}
    Value(int n) : m_v(static_cast<double>(n)) {}
    Value(std::int64_t n) : m_v(static_cast<double>(n)) {}
    Value(std::size_t n) : m_v(static_cast<double>(n)) {}
    Value(double n) : m_v(n) {}
    Value(const char* s) : m_v(std::string(s)) {}
    Value(std::string_view s) : m_v(std::string(s)) {}
    Value(std::string s) : m_v(std::move(s)) {}
    Value(Array a) : m_v(std::move(a)) {}
    Value(Object o) : m_v(std::move(o)) {}

    [[nodiscard]] bool isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(m_v); }
    [[nodiscard]] bool isBool() const noexcept { return std::holds_alternative<bool>(m_v); }
    [[nodiscard]] bool isNumber() const noexcept { return std::holds_alternative<double>(m_v); }
    [[nodiscard]] bool isString() const noexcept { return std::holds_alternative<std::string>(m_v); }
    [[nodiscard]] bool isArray() const noexcept { return std::holds_alternative<Array>(m_v); }
    [[nodiscard]] bool isObject() const noexcept { return std::holds_alternative<Object>(m_v); }

    [[nodiscard]] bool asBool(bool fallback = false) const noexcept { return isBool() ? std::get<bool>(m_v) : fallback; }
    [[nodiscard]] double asNumber(double fallback = 0) const noexcept
    {
        return isNumber() ? std::get<double>(m_v) : fallback;
    }
    [[nodiscard]] std::string asString(std::string_view fallback = {}) const
    {
        return isString() ? std::get<std::string>(m_v) : std::string(fallback);
    }
    [[nodiscard]] const Array& asArray() const noexcept;
    [[nodiscard]] const Object& asObject() const noexcept;

    /// The member called `key` of an object, or null (also for non-objects).
    [[nodiscard]] const Value& operator[](std::string_view key) const noexcept;
    /// Adds or replaces a member (turns a null into an object first).
    Value& set(std::string_view key, Value value);

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> m_v;
};

/// Reads one JSON value (surrounding white space allowed).
[[nodiscard]] Result<Value> parse(std::string_view text);

/// The value as compact JSON on one line (non-ASCII written as UTF-8; control
/// characters escaped), as JSON-RPC over a stream needs.
[[nodiscard]] std::string write(const Value& value);

} // namespace viewer::json
