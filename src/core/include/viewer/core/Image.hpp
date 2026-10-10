#pragma once

#include "viewer/core/Bytes.hpp"
#include "viewer/core/Error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// A decoded image ready for display: 8-bit RGBA, rows top to bottom.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; ///< width * height * 4 bytes.
    std::string format;             ///< Source pixel format, e.g. "DXT5".
    std::uint32_t mipCount = 1;
    std::uint32_t layers = 1;       ///< Cube faces / array slices / depth.
};

namespace image {

/// GPU / D3D pixel formats our decoders understand.
enum class PixelFormat {
    BC1, ///< DXT1
    BC2, ///< DXT3
    BC3, ///< DXT5
    BC4, ///< ATI1
    BC5, ///< ATI2 (two-channel normal maps)
    A8R8G8B8,
    X8R8G8B8,
    A8B8G8R8,
    R8G8B8,
    R5G6B5,
    A1R5G5B5,
    A4R4G4B4,
    L8,
    A8,
    A8L8,
};

[[nodiscard]] std::string_view pixelFormatName(PixelFormat format) noexcept;

/// Bytes needed for one surface (one mip level) of the given size.
[[nodiscard]] std::size_t surfaceSize(PixelFormat format, std::uint32_t width, std::uint32_t height) noexcept;

/// Maps a FourCC ("DXT1") or a numeric D3DFORMAT value (21 = A8R8G8B8) to a PixelFormat.
[[nodiscard]] std::optional<PixelFormat> fromD3DFormat(std::uint32_t d3dFormatOrFourCC) noexcept;

/// Decodes the top mip of a raw surface into RGBA8.
Result<Image> decodeSurface(ByteSpan pixels, PixelFormat format, std::uint32_t width, std::uint32_t height);

/// Decodes a complete .dds file (DX9 header and DX10 extended header).
Result<Image> decodeDds(ByteSpan file);

} // namespace image
} // namespace viewer
