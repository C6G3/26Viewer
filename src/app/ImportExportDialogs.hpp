#pragma once

#include "Exporter.hpp"

#include <QString>

#include <optional>

class QWidget;

namespace viewer::app {

/// Asks which picture format textures are exported as (PNG / DDS / JPG / BMP);
/// remembers the choice. Empty if cancelled.
std::optional<ImageExport> chooseImageExport(QWidget* parent);

/// Asks which pixel format (DXT1 / DXT3 / DXT5 / uncompressed…) an imported
/// picture is stored in; the texture's current format is the default.
std::optional<image::PixelFormat> chooseTextureFormat(QWidget* parent, const TextureTarget& target,
                                                      const Picture& picture, const QString& pictureName,
                                                      const QString& textureName);

/// "DXT5 — smooth transparency…": what a pixel format is good for.
QString pixelFormatDescription(image::PixelFormat format);

} // namespace viewer::app
