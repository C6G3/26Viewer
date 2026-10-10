#pragma once

#include <expected>
#include <format>
#include <string>
#include <utility>

namespace viewer {

/// Broad failure categories, so callers can react without parsing messages.
enum class ErrorCode {
    Io,             ///< File could not be opened/read/written.
    InvalidFormat,  ///< Data does not match the expected format.
    Unsupported,    ///< Recognised, but a variant we cannot handle yet.
    Decompression,  ///< Compressed stream is corrupt.
    OutOfBounds,    ///< A read would run past the end of a buffer.
    NotFound,
};

struct Error {
    ErrorCode code{};
    std::string message;
};

/// Every fallible operation returns Result<T>: either a value or an Error.
/// (std::expected is C++23's standard "value or error" type — no exceptions
/// cross module boundaries.)
template <typename T>
using Result = std::expected<T, Error>;

/// Convenience: `return fail(ErrorCode::Io, "cannot open {}", path);`
template <typename... Args>
[[nodiscard]] std::unexpected<Error> fail(ErrorCode code, std::format_string<Args...> fmt, Args&&... args)
{
    return std::unexpected(Error{code, std::format(fmt, std::forward<Args>(args)...)});
}

} // namespace viewer
