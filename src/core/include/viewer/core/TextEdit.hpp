#pragma once

#include "viewer/core/Error.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// Decompiled text that knows where its values are, so an edited copy can be
/// compiled back: everything but the values must stay as it is — except the
/// lists the decompiler lets grow or shrink.
///
/// Decompilers write each editable value between markers (mark()), and a
/// marker before the "[" of each list that may change length (markList()),
/// then finish() takes the markers out and remembers where everything was.
struct EditableText {
    std::string text;
    /// Each value's place in `text` (begin, end), in order.
    std::vector<std::pair<std::size_t, std::size_t>> values;
    /// Each resizable list: where its "[" is, and the decompiler's id for it.
    std::vector<std::pair<std::size_t, std::size_t>> lists;

    /// `text` wrapped in markers; `id` is the decompiler's own number for the
    /// value (where it lives in the file), handed back by changedValues().
    [[nodiscard]] static std::string mark(std::string_view text, std::size_t id);
    /// Goes right before a list's "[": the list may get more or fewer items.
    [[nodiscard]] static std::string markList(std::size_t id);
    /// Takes the markers out of `marked`. `ids` receives each value's id, in order.
    [[nodiscard]] static EditableText finish(std::string_view marked, std::vector<std::size_t>* ids = nullptr);
    /// Characters a reader sees in `marked` (markers don't count).
    [[nodiscard]] static std::size_t visibleLength(std::string_view marked) noexcept;
};

/// A value the user changed.
struct ChangedValue {
    std::size_t index = 0; ///< Into EditableText::values.
    std::string text;      ///< As typed (outer spaces trimmed).
    std::size_t line = 0;  ///< 1-based, in the edited text.
};

/// Lines `edited` up with `original`: words, punctuation and line breaks
/// outside the values must match (spacing, blank lines and comments may
/// differ). Returns the values whose text changed, or an error naming the
/// first line that doesn't match ("line 12: …").
[[nodiscard]] Result<std::vector<ChangedValue>> changedValues(const EditableText& original, std::string_view edited);

/// A list whose number of items the user changed.
struct ResizedList {
    std::size_t id = 0;    ///< The decompiler's id (markList).
    std::size_t items = 0; ///< How many it has now.
    std::size_t line = 0;  ///< Of its "[" in the edited text.
};

/// The lists of `original` whose item count differs in `edited` (items are
/// separated by commas or line breaks). Only the outermost changed lists are
/// returned — lists inside them are compared once they have been resized.
/// A list without a markList() marker that changed is an error.
[[nodiscard]] Result<std::vector<ResizedList>> resizedLists(const EditableText& original, std::string_view edited);

/// Reads a "double-quoted" string with \" \\ \n \r \t \xNN escapes.
[[nodiscard]] std::optional<std::string> unquote(std::string_view text);

/// Parses a number; the whole text must be the number. Accepts 0x… for integers.
[[nodiscard]] std::optional<double> parseReal(std::string_view text);
[[nodiscard]] std::optional<long long> parseInteger(std::string_view text);
[[nodiscard]] std::optional<unsigned long long> parseUnsigned(std::string_view text);

} // namespace viewer
