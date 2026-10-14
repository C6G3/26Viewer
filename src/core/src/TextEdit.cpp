#include "viewer/core/TextEdit.hpp"

#include <algorithm>
#include <charconv>
#include <format>

namespace viewer {

namespace {

constexpr char kBegin = '\x01';
constexpr char kEnd = '\x02';
constexpr char kIdEnd = '\x03';
constexpr char kList = '\x04';

struct Token {
    enum Kind { Word, String, Punct, Newline } kind;
    std::size_t begin;
    std::size_t end;
    std::size_t line;
};

bool isPunct(char c) noexcept
{
    switch (c) {
    case '{': case '}': case '[': case ']': case '=': case ',': case '|':
    case '(': case ')': case '<': case '>': case ';': case '/':
        return true;
    default:
        return false;
    }
}

bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

/// Words, "strings", punctuation and line breaks; comments and spaces are skipped,
/// and runs of line breaks count as one.
std::vector<Token> tokenize(std::string_view s)
{
    std::vector<Token> out;
    std::size_t line = 1;
    const auto newline = [&](std::size_t at) {
        if (!out.empty() && out.back().kind != Token::Newline)
            out.push_back({Token::Newline, at, at + 1, line});
        ++line;
    };
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '\n') {
            newline(i);
            ++i;
        } else if (isSpace(c)) {
            ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n')
                ++i;
        } else if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i < s.size() && !(s[i] == '*' && i + 1 < s.size() && s[i + 1] == '/')) {
                if (s[i] == '\n')
                    ++line; // a comment's line breaks don't end a line of values
                ++i;
            }
            i = std::min(s.size(), i + 2);
        } else if (c == '"') {
            const std::size_t begin = i++;
            while (i < s.size() && s[i] != '"' && s[i] != '\n')
                i += s[i] == '\\' && i + 1 < s.size() && s[i + 1] != '\n' ? 2 : 1;
            if (i < s.size() && s[i] == '"')
                ++i;
            out.push_back({Token::String, begin, i, line});
        } else if (isPunct(c)) {
            out.push_back({Token::Punct, i, i + 1, line});
            ++i;
        } else {
            const std::size_t begin = i;
            while (i < s.size() && s[i] != '\n' && !isSpace(s[i]) && s[i] != '"' && !isPunct(s[i]))
                ++i;
            out.push_back({Token::Word, begin, i, line});
        }
    }
    if (!out.empty() && out.back().kind == Token::Newline)
        out.pop_back();

    // Inside brackets ([…] and {…}) a comma and a line break both just separate
    // items, so a list or struct may be written on one line or several: commas
    // become line breaks, and breaks next to a bracket go.
    std::vector<Token> tokens;
    tokens.reserve(out.size());
    int depth = 0;
    for (const Token& t : out) {
        const char c = t.kind == Token::Punct ? s[t.begin] : '\0';
        const bool opening = c == '[' || c == '{', closing = c == ']' || c == '}';
        if (depth > 0 && (t.kind == Token::Newline || c == ',')) {
            const bool afterOpening = !tokens.empty() && tokens.back().kind == Token::Punct
                                      && (s[tokens.back().begin] == '[' || s[tokens.back().begin] == '{');
            if (!tokens.empty() && tokens.back().kind != Token::Newline && !afterOpening)
                tokens.push_back({Token::Newline, t.begin, t.end, t.line});
            continue;
        }
        if (closing && depth > 0 && !tokens.empty() && tokens.back().kind == Token::Newline)
            tokens.pop_back();
        tokens.push_back(t);
        depth += opening ? 1 : closing && depth > 0 ? -1 : 0;
    }
    return tokens;
}

/// The lists of a text: each list's items, and the lists inside each item.
struct ListNode {
    std::size_t open = 0; ///< Token index of its "[".
    std::vector<std::vector<ListNode>> items;
};

std::vector<ListNode> listTree(const std::vector<Token>& tokens, std::string_view text)
{
    std::vector<ListNode> top;
    struct Frame {
        ListNode node;
        int braces;      ///< Brace depth at its "[" (separators count only there).
        bool itemOpen;   ///< The current item has tokens.
    };
    std::vector<Frame> stack;
    int braces = 0;
    const auto attach = [&](ListNode node) {
        if (stack.empty()) {
            top.push_back(std::move(node));
            return;
        }
        Frame& f = stack.back();
        if (!f.itemOpen) {
            f.node.items.emplace_back();
            f.itemOpen = true;
        }
        f.node.items.back().push_back(std::move(node));
    };
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        const char c = t.kind == Token::Punct ? text[t.begin] : '\0';
        if (c == '[') {
            if (!stack.empty() && !stack.back().itemOpen) {
                stack.back().node.items.emplace_back();
                stack.back().itemOpen = true;
            }
            stack.push_back({ListNode{i, {}}, braces, false});
            continue;
        }
        if (c == ']' && !stack.empty() && braces == stack.back().braces) {
            ListNode done = std::move(stack.back().node);
            stack.pop_back();
            attach(std::move(done));
            continue;
        }
        if (c == '{')
            ++braces;
        else if (c == '}')
            --braces;
        if (stack.empty())
            continue;
        Frame& f = stack.back();
        const bool separator = braces == f.braces && (c == ',' || t.kind == Token::Newline);
        if (separator) {
            f.itemOpen = false;
        } else if (!f.itemOpen) {
            f.node.items.emplace_back();
            f.itemOpen = true;
        }
    }
    while (!stack.empty()) { // unclosed: close them (the value check reports the mistake)
        ListNode done = std::move(stack.back().node);
        stack.pop_back();
        attach(std::move(done));
    }
    return top;
}

std::string describe(std::string_view text, const Token& t)
{
    if (t.kind == Token::Newline)
        return "the end of the line (or a comma)";
    return std::format("\"{}\"", text.substr(t.begin, t.end - t.begin));
}

} // namespace

std::string EditableText::mark(std::string_view text, std::size_t id)
{
    return std::format("{}{}{}{}{}", kBegin, text, kEnd, id, kIdEnd);
}

EditableText EditableText::finish(std::string_view marked, std::vector<std::size_t>* ids)
{
    EditableText out;
    out.text.reserve(marked.size());
    std::size_t begin = 0;
    for (std::size_t i = 0; i < marked.size(); ++i) {
        const char c = marked[i];
        if (c == kBegin) {
            begin = out.text.size();
        } else if (c == kEnd || c == kList) {
            std::size_t id = 0;
            for (++i; i < marked.size() && marked[i] != kIdEnd; ++i)
                id = id * 10 + static_cast<std::size_t>(marked[i] - '0');
            if (c == kList) {
                out.lists.emplace_back(out.text.size(), id);
                continue;
            }
            out.values.emplace_back(begin, out.text.size());
            if (ids)
                ids->push_back(id);
        } else {
            out.text.push_back(c);
        }
    }
    return out;
}

std::string EditableText::markList(std::size_t id)
{
    return std::format("{}{}{}", kList, id, kIdEnd);
}

std::size_t EditableText::visibleLength(std::string_view marked) noexcept
{
    std::size_t n = 0;
    bool inId = false;
    for (const char c : marked) {
        if (c == kEnd || c == kList)
            inId = true;
        else if (c == kIdEnd)
            inId = false;
        else if (c != kBegin && !inId)
            ++n;
    }
    return n;
}

Result<std::vector<ChangedValue>> changedValues(const EditableText& original, std::string_view edited)
{
    const std::string_view before = original.text;
    if (edited.starts_with("\xEF\xBB\xBF")) // a byte order mark from an editor
        edited.remove_prefix(3);
    const std::vector<Token> a = tokenize(before);
    const std::vector<Token> b = tokenize(edited);

    // The original as a run of fixed tokens and values (a value may span several tokens).
    struct Element {
        bool value;
        std::size_t index; // value index
        std::size_t first, last; // token range in `a`, inclusive
    };
    std::vector<Element> elements;
    std::size_t v = 0;
    for (std::size_t t = 0; t < a.size(); ++t) {
        while (v < original.values.size() && original.values[v].second <= a[t].begin)
            ++v;
        const bool inValue = v < original.values.size() && a[t].begin >= original.values[v].first
                             && a[t].end <= original.values[v].second;
        if (inValue && !elements.empty() && elements.back().value && elements.back().index == v)
            elements.back().last = t;
        else
            elements.push_back({inValue, v, t, t});
    }

    const auto same = [&](const Token& x, const Token& y) {
        return x.kind == y.kind
               && (x.kind == Token::Newline // a line break or a comma
                   || before.substr(x.begin, x.end - x.begin) == edited.substr(y.begin, y.end - y.begin));
    };
    const auto lineAt = [&](std::size_t j) {
        return j < b.size() ? b[j].line : (b.empty() ? 1 : b.back().line);
    };

    std::vector<ChangedValue> changes;
    std::size_t j = 0;
    for (std::size_t k = 0; k < elements.size(); ++k) {
        const Element& e = elements[k];
        if (!e.value) {
            if (j >= b.size() || !same(a[e.first], b[j])) {
                const std::string found = j < b.size() ? describe(edited, b[j]) : std::string("the end of the text");
                return fail(ErrorCode::InvalidFormat,
                            "line {}: expected {} here but found {}. Only values can be changed: names, "
                            "brackets and the number of items in a list must stay as they are",
                            lineAt(j), describe(before, a[e.first]), found);
            }
            ++j;
            continue;
        }
        // A value runs up to the next fixed token (or the end of its line).
        const Token* stop = k + 1 < elements.size() && !elements[k + 1].value ? &a[elements[k + 1].first] : nullptr;
        const std::size_t start = j;
        if (stop) {
            // Brackets inside the value ("-> Thing[3]") are the value's own.
            int nested = 0;
            for (; j < b.size(); ++j) {
                const char c = b[j].kind == Token::Punct ? edited[b[j].begin] : '\0';
                if (nested == 0 && (b[j].kind == Token::Newline || same(*stop, b[j])))
                    break;
                if (c == '[' || c == '{' || c == '(')
                    ++nested;
                else if ((c == ']' || c == '}' || c == ')') && nested > 0)
                    --nested;
                else if (b[j].kind == Token::Newline)
                    break;
            }
        } else if (j < b.size() && b[j].kind != Token::Newline) {
            ++j;
        }
        if (j == start)
            return fail(ErrorCode::InvalidFormat, "line {}: a value is missing here", lineAt(start));
        bool changed = j - start != e.last - e.first + 1;
        for (std::size_t t = 0; !changed && t <= e.last - e.first; ++t)
            changed = !same(a[e.first + t], b[start + t]);
        if (changed)
            changes.push_back({e.index, std::string(edited.substr(b[start].begin, b[j - 1].end - b[start].begin)),
                               b[start].line});
    }
    if (j < b.size())
        return fail(ErrorCode::InvalidFormat, "line {}: {} was added after the end; only values can be changed",
                    b[j].line, describe(edited, b[j]));
    return changes;
}

Result<std::vector<ResizedList>> resizedLists(const EditableText& original, std::string_view edited)
{
    if (edited.starts_with("\xEF\xBB\xBF"))
        edited.remove_prefix(3);
    const std::vector<Token> a = tokenize(original.text);
    const std::vector<Token> b = tokenize(edited);
    const std::vector<ListNode> before = listTree(a, original.text);
    const std::vector<ListNode> after = listTree(b, edited);

    std::vector<ResizedList> out;
    std::string error;
    const auto compare = [&](auto&& self, const std::vector<ListNode>& x, const std::vector<ListNode>& y) -> void {
        if (x.size() != y.size() || !error.empty())
            return; // the structure differs elsewhere: changedValues() says where
        for (std::size_t i = 0; i < x.size(); ++i) {
            if (x[i].items.size() == y[i].items.size()) {
                for (std::size_t k = 0; k < x[i].items.size(); ++k)
                    self(self, x[i].items[k], y[i].items[k]);
                continue;
            }
            const std::size_t at = a[x[i].open].begin;
            const auto it = std::ranges::find_if(original.lists, [&](const auto& l) { return l.first == at; });
            const std::size_t line = b[y[i].open].line;
            if (it == original.lists.end()) {
                error = std::format("line {}: this list must keep its {} item(s)", line, x[i].items.size());
                return;
            }
            out.push_back({it->second, y[i].items.size(), line});
        }
    };
    compare(compare, before, after);
    if (!error.empty())
        return fail(ErrorCode::InvalidFormat, "{}", error);
    return out;
}

std::optional<std::string> unquote(std::string_view s)
{
    if (s.size() < 2 || s.front() != '"' || s.back() != '"')
        return std::nullopt;
    std::string out;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
        char c = s[i];
        if (c == '"')
            return std::nullopt;
        if (c == '\\') {
            if (i + 2 >= s.size())
                return std::nullopt;
            switch (s[++i]) {
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case 'x': {
                unsigned v = 0;
                if (i + 3 >= s.size() || std::from_chars(s.data() + i + 1, s.data() + i + 3, v, 16).ptr != s.data() + i + 3)
                    return std::nullopt;
                c = static_cast<char>(v);
                i += 2;
                break;
            }
            default: return std::nullopt;
            }
        }
        out.push_back(c);
    }
    return out;
}

std::optional<double> parseReal(std::string_view s)
{
    if (!s.empty() && s.front() == '+')
        s.remove_prefix(1);
    double v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return v;
}

std::optional<unsigned long long> parseUnsigned(std::string_view s)
{
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    unsigned long long v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (s.empty() || ec != std::errc{} || end != s.data() + s.size())
        return std::nullopt;
    return v;
}

std::optional<long long> parseInteger(std::string_view s)
{
    bool negative = false;
    if (!s.empty() && (s.front() == '-' || s.front() == '+')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    const auto v = parseUnsigned(s);
    if (!v || *v > (negative ? 9223372036854775808ull : 9223372036854775807ull))
        return std::nullopt;
    return negative ? static_cast<long long>(0 - *v) : static_cast<long long>(*v);
}

} // namespace viewer
