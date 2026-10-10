// icongen — renders an SVG into a multi-resolution Windows .ico file.
//
//   icongen <input.svg> <output.ico>
//
// Used by the build (src/app/CMakeLists.txt) so the .exe icon is always
// generated from resources/icons/app.svg; no binary icon is committed.
// Each size is stored as a PNG inside the ICO container (supported since
// Windows Vista), rendered separately so small sizes stay crisp.

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>
#include <vector>

namespace {

void put16(QByteArray& out, quint16 v)
{
    out.append(static_cast<char>(v & 0xFF));
    out.append(static_cast<char>(v >> 8));
}

void put32(QByteArray& out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(static_cast<char>((v >> (8 * i)) & 0xFF));
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: icongen <input.svg> <output.ico>\n");
        return 2;
    }
    QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));
    if (!svg.isValid()) {
        std::fprintf(stderr, "icongen: cannot load %s\n", argv[1]);
        return 1;
    }

    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::vector<QByteArray> images;
    for (int size : sizes) {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        svg.render(&painter);
        painter.end();

        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.convertToFormat(QImage::Format_ARGB32).save(&buffer, "PNG");
        images.push_back(png);
    }

    // ICONDIR + ICONDIRENTRY[n] + image data.
    QByteArray ico;
    put16(ico, 0); // reserved
    put16(ico, 1); // type: icon
    put16(ico, static_cast<quint16>(images.size()));
    quint32 offset = 6 + 16 * static_cast<quint32>(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        const int size = sizes[i];
        ico.append(static_cast<char>(size >= 256 ? 0 : size)); // 0 means 256
        ico.append(static_cast<char>(size >= 256 ? 0 : size));
        ico.append('\0'); // palette colours
        ico.append('\0'); // reserved
        put16(ico, 1);    // colour planes
        put16(ico, 32);   // bits per pixel
        put32(ico, static_cast<quint32>(images[i].size()));
        put32(ico, offset);
        offset += static_cast<quint32>(images[i].size());
    }
    for (const auto& png : images)
        ico.append(png);

    QFile file(QString::fromLocal8Bit(argv[2]));
    if (!file.open(QIODevice::WriteOnly) || file.write(ico) != ico.size()) {
        std::fprintf(stderr, "icongen: cannot write %s\n", argv[2]);
        return 1;
    }
    return 0;
}
