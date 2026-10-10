#include "viewer/core/Archive.hpp"

namespace viewer {

Result<std::shared_ptr<const ByteSource>> Archive::openEntry(std::size_t index) const
{
    auto bytes = read(index);
    if (!bytes)
        return std::unexpected(bytes.error());
    return memorySource(std::move(*bytes));
}

} // namespace viewer
