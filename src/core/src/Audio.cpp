#include "viewer/core/Audio.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <map>

namespace viewer::audio {

namespace {

void putU32(ByteBuffer& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>(v >> (8 * i)));
}

void putU16(ByteBuffer& out, std::uint16_t v)
{
    out.push_back(static_cast<std::byte>(v));
    out.push_back(static_cast<std::byte>(v >> 8));
}

void putTag(ByteBuffer& out, const char (&tag)[5])
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::byte>(tag[i]));
}

class WavBank final : public SoundBank {
public:
    WavBank(AudioClip clip, std::uint64_t dataSize) : m_clip(std::move(clip))
    {
        m_info.name = "audio";
        m_info.sampleRate = m_clip.sampleRate;
        m_info.channels = m_clip.channels;
        m_info.frames = m_clip.frames();
        m_info.codec = "PCM";
        m_info.dataSize = dataSize;
    }
    std::string_view formatName() const noexcept override { return "Wave audio"; }
    std::span<const SoundInfo> sounds() const noexcept override { return {&m_info, 1}; }
    Result<AudioClip> decode(std::size_t index) const override
    {
        if (index != 0)
            return fail(ErrorCode::NotFound, "sound {} out of range", index);
        return m_clip;
    }

private:
    AudioClip m_clip;
    SoundInfo m_info;
};

class WavFormat final : public SoundBankFormat {
public:
    std::string_view name() const noexcept override { return "Wave audio"; }
    bool canOpen(std::string_view, ByteSpan header) const override
    {
        return header.size() >= 12 && std::memcmp(header.data(), "RIFF", 4) == 0
               && std::memcmp(header.data() + 8, "WAVE", 4) == 0;
    }
    Result<std::unique_ptr<SoundBank>> open(std::shared_ptr<const ByteSource> source) const override
    {
        if (source->size() > (512ull << 20))
            return fail(ErrorCode::Unsupported, "wave file too large to preview");
        auto bytes = source->read(0, static_cast<std::size_t>(source->size()));
        if (!bytes)
            return std::unexpected(bytes.error());
        auto clip = decodeWav(*bytes);
        if (!clip)
            return std::unexpected(clip.error());
        return std::unique_ptr<SoundBank>(std::make_unique<WavBank>(std::move(*clip), bytes->size()));
    }
};

} // namespace

std::vector<std::string> soundFileNames(std::span<const SoundInfo> sounds)
{
    std::vector<std::string> names;
    std::map<std::string, int> seen; // lower-case name -> times used (Windows names ignore case)
    for (std::size_t i = 0; i < sounds.size(); ++i) {
        std::string name;
        for (char c : sounds[i].name)
            name += (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
                        ? '_'
                        : c;
        while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
            name.pop_back(); // Windows drops these
        if (name.empty())
            name = std::format("sound_{}", i + 1);
        std::string key = name;
        for (char& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (const int n = ++seen[key]; n > 1)
            name += std::format("_{}", n);
        names.push_back(std::move(name));
    }
    return names;
}

ByteBuffer encodeWav(const AudioClip& clip)
{
    const auto dataBytes = static_cast<std::uint32_t>(clip.samples.size() * 2);
    ByteBuffer out;
    out.reserve(44 + dataBytes);
    putTag(out, "RIFF");
    putU32(out, 36 + dataBytes);
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    putU32(out, 16);
    putU16(out, 1); // PCM
    putU16(out, clip.channels);
    putU32(out, clip.sampleRate);
    putU32(out, clip.sampleRate * clip.channels * 2u);
    putU16(out, static_cast<std::uint16_t>(clip.channels * 2));
    putU16(out, 16);
    putTag(out, "data");
    putU32(out, dataBytes);
    for (const std::int16_t s : clip.samples)
        putU16(out, static_cast<std::uint16_t>(s));
    return out;
}

Result<AudioClip> decodeWav(ByteSpan file)
{
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0)
        return fail(ErrorCode::InvalidFormat, "not a RIFF/WAVE file");
    AudioClip clip;
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    std::size_t p = 12;
    while (p + 8 <= file.size()) {
        const auto size = loadScalar<std::uint32_t>(file, p + 4);
        const std::size_t body = p + 8;
        if (size > file.size() - body)
            return fail(ErrorCode::InvalidFormat, "truncated wave chunk");
        if (std::memcmp(file.data() + p, "fmt ", 4) == 0 && size >= 16) {
            format = loadScalar<std::uint16_t>(file, body);
            clip.channels = loadScalar<std::uint16_t>(file, body + 2);
            clip.sampleRate = loadScalar<std::uint32_t>(file, body + 4);
            bits = loadScalar<std::uint16_t>(file, body + 14);
        } else if (std::memcmp(file.data() + p, "data", 4) == 0) {
            if (format != 1 || (bits != 8 && bits != 16) || clip.channels == 0)
                return fail(ErrorCode::Unsupported, "only 8/16-bit PCM wave files are supported (format {}, {} bits)",
                            format, bits);
            if (bits == 16) {
                clip.samples.resize(size / 2);
                for (std::size_t i = 0; i < clip.samples.size(); ++i)
                    clip.samples[i] = static_cast<std::int16_t>(loadScalar<std::uint16_t>(file, body + 2 * i));
            } else {
                clip.samples.resize(size);
                for (std::size_t i = 0; i < size; ++i)
                    clip.samples[i] = static_cast<std::int16_t>((std::to_integer<int>(file[body + i]) - 128) << 8);
            }
            return clip;
        }
        p = body + size + (size & 1); // chunks are word aligned
    }
    return fail(ErrorCode::InvalidFormat, "wave file has no data chunk");
}

std::shared_ptr<const SoundBankFormat> wavFormat()
{
    static const auto format = std::make_shared<WavFormat>();
    return format;
}

} // namespace viewer::audio
