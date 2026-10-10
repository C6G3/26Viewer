// Blur ".baf" audio bank.
//
//   "BANK" u32 headerSize (0x34) u32 version (4) u32 soundCount, u16 0,
//          char[32] bank name
//   then soundCount "WAVE" records, each: "WAVE" u32 recordSize u32 type,
//          char[32] name, then (offsets from the record start):
//     type 7 (76 bytes, ADPCM):  +44 u32 dataOffset (from file start)
//                                +48 u32 dataSize  +52 f32 volume  +56 u32 25
//                                +64 u32 sampleRate +68 u32 frames
//                                +72 u16 loop  +74 u8 bits (4)  +75 u8 channels
//     type 3 (80 bytes, PCM16):  +44 offset +48 size +52 volume +56 25
//                                +60 rate +64 bits (16) +68 channels
//   then each sound's data in record order, as "DATA" u32 (size + 8) + data;
//   streaming banks pad with zeros so every data block starts on a 0x800
//   boundary, and pad the file to 0x800.
//
// The ADPCM is PlayStation-style ADPCM in 33-byte frames, recovered from the
// decoder in Blur.exe (0x540C80; filter table 0xE858F0, scale table 0xE85918).

#include "BafBank.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>

namespace viewer::games::blur {

namespace {

constexpr std::size_t kFrameBytes = 33;
constexpr std::size_t kFrameSamples = 64;

struct Sound {
    SoundInfo info;
    std::uint32_t type = 0;
    std::uint64_t offset = 0;
};

class BafBank final : public SoundBank {
public:
    BafBank(std::shared_ptr<const ByteSource> source, std::string name, std::vector<Sound> sounds)
        : m_source(std::move(source)), m_name(std::move(name)), m_sounds(std::move(sounds))
    {
        for (const auto& s : m_sounds)
            m_infos.push_back(s.info);
    }

    std::string_view formatName() const noexcept override { return "Blur audio bank"; }
    std::span<const SoundInfo> sounds() const noexcept override { return m_infos; }

    Result<AudioClip> decode(std::size_t index) const override
    {
        if (index >= m_sounds.size())
            return fail(ErrorCode::NotFound, "sound {} out of range", index);
        const Sound& s = m_sounds[index];
        auto data = m_source->read(s.offset, static_cast<std::size_t>(s.info.dataSize));
        if (!data)
            return std::unexpected(data.error());
        AudioClip clip;
        clip.sampleRate = s.info.sampleRate;
        clip.channels = s.info.channels;
        if (s.type == 3) { // 16-bit PCM
            const std::size_t count = data->size() / 2;
            clip.samples.resize(count);
            for (std::size_t i = 0; i < count; ++i)
                clip.samples[i] = static_cast<std::int16_t>(loadScalar<std::uint16_t>(*data, 2 * i));
        } else {
            clip.samples = baf::decodeAdpcm(*data, s.info.channels, s.info.frames);
        }
        return clip;
    }

    PropertyList describe() const override
    {
        double seconds = 0;
        for (const auto& s : m_infos)
            seconds += s.seconds();
        return {{"Bank", m_name},
                {"Sounds", std::to_string(m_infos.size())},
                {"Total length", std::format("{:.1f} s", seconds)}};
    }

private:
    std::shared_ptr<const ByteSource> m_source;
    std::string m_name;
    std::vector<Sound> m_sounds;
    std::vector<SoundInfo> m_infos;
};

std::string fixedString(ByteSpan data, std::size_t offset, std::size_t size)
{
    std::string s;
    for (std::size_t i = offset; i < offset + size && i < data.size(); ++i) {
        const char c = static_cast<char>(data[i]);
        if (c == '\0')
            break;
        s.push_back(c);
    }
    return s;
}

} // namespace

bool BafFormat::canOpen(std::string_view, ByteSpan header) const
{
    return header.size() >= 0x34 && std::memcmp(header.data(), "BANK", 4) == 0;
}

Result<std::unique_ptr<SoundBank>> BafFormat::open(std::shared_ptr<const ByteSource> source) const
{
    if (source->size() < 0x34)
        return fail(ErrorCode::InvalidFormat, "file too small for a BANK header");
    auto header = source->read(0, 0x34);
    if (!header)
        return std::unexpected(header.error());
    if (std::memcmp(header->data(), "BANK", 4) != 0)
        return fail(ErrorCode::InvalidFormat, "not a Blur audio bank (no BANK header)");
    const auto headerSize = loadScalar<std::uint32_t>(*header, 4);
    const auto count = loadScalar<std::uint32_t>(*header, 12);
    if (headerSize < 0x34 || count > 100000)
        return fail(ErrorCode::InvalidFormat, "implausible BANK header");

    // The WAVE records follow the header; read them in one go (80 bytes each at most).
    const auto recordsEnd = std::min<std::uint64_t>(source->size(), headerSize + std::uint64_t{count} * 80);
    auto records = source->read(0, static_cast<std::size_t>(recordsEnd));
    if (!records)
        return std::unexpected(records.error());
    const ByteSpan r = *records;

    std::vector<Sound> sounds;
    std::size_t p = headerSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (p + 76 > r.size() || std::memcmp(r.data() + p, "WAVE", 4) != 0)
            return fail(ErrorCode::InvalidFormat, "WAVE record {} missing", i);
        const auto size = loadScalar<std::uint32_t>(r, p + 4);
        const auto type = loadScalar<std::uint32_t>(r, p + 8);
        if (size < 76 || p + size > r.size())
            return fail(ErrorCode::InvalidFormat, "bad WAVE record {}", i);
        Sound s;
        s.type = type;
        s.info.name = fixedString(r, p + 12, 32);
        s.offset = loadScalar<std::uint32_t>(r, p + 44);
        s.info.dataSize = loadScalar<std::uint32_t>(r, p + 48);
        if (type == 3) {
            s.info.sampleRate = loadScalar<std::uint32_t>(r, p + 60);
            s.info.channels = static_cast<std::uint16_t>(loadScalar<std::uint32_t>(r, p + 68));
            s.info.codec = "PCM 16-bit";
            s.info.frames = s.info.channels ? s.info.dataSize / (2u * s.info.channels) : 0;
        } else {
            s.info.sampleRate = loadScalar<std::uint32_t>(r, p + 64);
            s.info.frames = loadScalar<std::uint32_t>(r, p + 68);
            s.info.looped = loadScalar<std::uint16_t>(r, p + 72) != 0;
            s.info.channels = std::to_integer<std::uint16_t>(r[p + 75]);
            s.info.codec = "ADPCM 4-bit";
        }
        if (s.info.channels == 0 || s.info.channels > 8 || s.offset + s.info.dataSize > source->size())
            return fail(ErrorCode::InvalidFormat, "sound '{}' has bad data bounds", s.info.name);
        sounds.push_back(std::move(s));
        p += size;
    }
    return std::unique_ptr<SoundBank>(
        std::make_unique<BafBank>(std::move(source), fixedString(*header, 0x12, 32), std::move(sounds)));
}

std::vector<std::int16_t> baf::decodeAdpcm(ByteSpan data, unsigned channels, std::uint64_t frames)
{
    // Filter coefficients (Blur.exe 0xE858F0) — the classic PS-ADPCM table.
    static constexpr std::array<std::array<float, 2>, 5> kFilters{{
        {0.0f, 0.0f}, {0.9375f, 0.0f}, {1.796875f, -0.8125f}, {1.53125f, -0.859375f}, {1.90625f, -0.9375f}}};

    if (channels == 0)
        return {};
    const std::size_t frameCount = data.size() / kFrameBytes;
    const std::size_t perChannel = frameCount / channels;
    const auto outFrames = static_cast<std::size_t>(std::min<std::uint64_t>(frames, perChannel * kFrameSamples));
    std::vector<std::int16_t> out(outFrames * channels);

    for (unsigned c = 0; c < channels; ++c) {
        // History is kept unclamped in floating point, like the game.
        float h1 = 0.0f;
        float h2 = 0.0f;
        std::size_t written = 0;
        for (std::size_t f = 0; f < perChannel && written < outFrames; ++f) {
            const std::size_t base = (f * channels + c) * kFrameBytes;
            const auto head = std::to_integer<unsigned>(data[base]);
            const unsigned filter = head >> 4;
            const float scale = std::ldexp(4096.0f, -static_cast<int>(head & 0x0F)); // nibble << 12 >> shift
            const auto& k = filter < kFilters.size() ? kFilters[filter] : kFilters[0];
            for (std::size_t i = 1; i < kFrameBytes && written < outFrames; ++i) {
                const auto byte = std::to_integer<unsigned>(data[base + i]);
                for (const unsigned nibble : {byte & 0x0Fu, byte >> 4}) {
                    if (written >= outFrames)
                        break;
                    const int signedNibble = nibble >= 8 ? static_cast<int>(nibble) - 16 : static_cast<int>(nibble);
                    const float sample = static_cast<float>(signedNibble) * scale + h1 * k[0] + h2 * k[1];
                    h2 = h1;
                    h1 = sample;
                    out[written * channels + c] =
                        static_cast<std::int16_t>(std::clamp(std::lround(sample), -32768L, 32767L));
                    ++written;
                }
            }
        }
    }
    return out;
}

ByteBuffer baf::encodeAdpcm(std::span<const std::int16_t> samples, unsigned channels)
{
    static constexpr std::array<std::array<float, 2>, 5> kFilters{{
        {0.0f, 0.0f}, {0.9375f, 0.0f}, {1.796875f, -0.8125f}, {1.53125f, -0.859375f}, {1.90625f, -0.9375f}}};
    if (channels == 0)
        return {};
    const std::size_t frames = samples.size() / channels;
    const std::size_t blocks = (frames + kFrameSamples - 1) / kFrameSamples;
    ByteBuffer out(blocks * channels * kFrameBytes);

    for (unsigned c = 0; c < channels; ++c) {
        float h1 = 0.0f; // decoder history, kept exactly as the game's decoder keeps it
        float h2 = 0.0f;
        std::array<float, kFrameSamples> x{};
        for (std::size_t b = 0; b < blocks; ++b) {
            for (std::size_t i = 0; i < kFrameSamples; ++i) {
                const std::size_t frame = b * kFrameSamples + i;
                x[i] = frame < frames ? static_cast<float>(samples[frame * channels + c]) : 0.0f;
            }
            // Try every filter; for each, the shift that fits its largest residual and its
            // neighbours, simulating the decoder; keep the combination with the least error.
            struct Choice {
                unsigned filter = 0, shift = 0;
                double error = 1e300;
            } best;
            for (unsigned f = 0; f < kFilters.size(); ++f) {
                const auto& k = kFilters[f];
                float p1 = h1, p2 = h2, peak = 0.0f;
                for (float v : x) { // residuals against the ideal signal
                    peak = std::max(peak, std::abs(v - (p1 * k[0] + p2 * k[1])));
                    p2 = p1;
                    p1 = v;
                }
                int fit = 12;
                while (fit > 0 && peak > 7.0f * std::ldexp(4096.0f, -fit))
                    --fit;
                for (int shift = std::max(0, fit - 1); shift <= std::min(12, fit + 1); ++shift) {
                    const float scale = std::ldexp(4096.0f, -shift);
                    float s1 = h1, s2 = h2;
                    double error = 0;
                    for (float v : x) {
                        const float predicted = s1 * k[0] + s2 * k[1];
                        const float n = std::clamp(std::round((v - predicted) / scale), -8.0f, 7.0f);
                        const float y = n * scale + predicted;
                        const double d = v - std::clamp(y, -32768.0f, 32767.0f);
                        error += d * d;
                        s2 = s1;
                        s1 = y;
                    }
                    if (error < best.error)
                        best = {f, static_cast<unsigned>(shift), error};
                }
            }
            // Emit the frame with the chosen filter and shift.
            std::byte* frame = out.data() + (b * channels + c) * kFrameBytes;
            frame[0] = static_cast<std::byte>(best.filter << 4 | best.shift);
            const auto& k = kFilters[best.filter];
            const float scale = std::ldexp(4096.0f, -static_cast<int>(best.shift));
            for (std::size_t i = 0; i < kFrameSamples; ++i) {
                const float predicted = h1 * k[0] + h2 * k[1];
                const int n = static_cast<int>(std::clamp(std::round((x[i] - predicted) / scale), -8.0f, 7.0f));
                const float y = static_cast<float>(n) * scale + predicted;
                h2 = h1;
                h1 = y;
                const auto nibble = static_cast<unsigned>(n & 0x0F);
                std::byte& target = frame[1 + i / 2];
                target |= static_cast<std::byte>(i % 2 ? nibble << 4 : nibble); // low nibble first
            }
        }
    }
    return out;
}

Result<ByteBuffer> BafFormat::rebuild(ByteSpan original, std::span<const std::optional<AudioClip>> replacements) const
{
    if (original.size() < 0x34 || std::memcmp(original.data(), "BANK", 4) != 0)
        return fail(ErrorCode::InvalidFormat, "not a Blur audio bank (no BANK header)");
    const auto headerSize = loadScalar<std::uint32_t>(original, 4);
    const auto count = loadScalar<std::uint32_t>(original, 12);
    if (replacements.size() > count)
        return fail(ErrorCode::InvalidFormat, "{} replacements for a bank of {} sounds", replacements.size(), count);

    struct Record {
        std::size_t at = 0;
        std::uint32_t type = 0, offset = 0, size = 0;
    };
    std::vector<Record> records;
    std::size_t p = headerSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (p + 76 > original.size() || std::memcmp(original.data() + p, "WAVE", 4) != 0)
            return fail(ErrorCode::InvalidFormat, "WAVE record {} missing", i);
        const Record r{p, loadScalar<std::uint32_t>(original, p + 8), loadScalar<std::uint32_t>(original, p + 44),
                       loadScalar<std::uint32_t>(original, p + 48)};
        if (std::size_t{r.offset} + r.size > original.size())
            return fail(ErrorCode::InvalidFormat, "sound {} has bad data bounds", i);
        records.push_back(r);
        p += loadScalar<std::uint32_t>(original, p + 4);
    }
    const std::size_t recordsEnd = p;
    // Streaming banks start every sound's data on a 2 KiB boundary.
    const std::uint32_t firstData = records.empty() ? 0 : std::ranges::min(records, {}, &Record::offset).offset;
    const bool aligned = !records.empty() && firstData % 0x800 == 0 && firstData > recordsEnd + 8;

    const auto write = [&](const std::vector<std::optional<ByteBuffer>>& newData) {
        ByteBuffer out(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(recordsEnd));
        const auto pad = [&](std::size_t to) { out.resize(out.size() + (to - out.size() % to) % to); };
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (aligned)
                out.resize(out.size() + (0x800 - (out.size() + 8) % 0x800) % 0x800);
            const ByteSpan data = i < newData.size() && newData[i]
                                      ? ByteSpan(*newData[i])
                                      : original.subspan(records[i].offset, records[i].size);
            const auto putU32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(out.data() + at, &v, 4); };
            out.insert(out.end(), {std::byte{'D'}, std::byte{'A'}, std::byte{'T'}, std::byte{'A'}, {}, {}, {}, {}});
            putU32(out.size() - 4, static_cast<std::uint32_t>(data.size() + 8));
            putU32(records[i].at + 44, static_cast<std::uint32_t>(out.size()));
            putU32(records[i].at + 48, static_cast<std::uint32_t>(data.size()));
            out.insert(out.end(), data.begin(), data.end());
        }
        if (aligned)
            pad(0x800);
        return out;
    };

    // Only rewrite banks whose layout this writer reproduces exactly.
    if (const ByteBuffer same = write({}); !std::ranges::equal(same, original))
        return fail(ErrorCode::Unsupported, "this bank's data layout isn't supported for rebuilding yet");

    std::vector<std::optional<ByteBuffer>> newData(records.size());
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        if (!replacements[i])
            continue;
        const AudioClip& clip = *replacements[i];
        if (clip.channels == 0 || clip.channels > 8 || clip.sampleRate == 0)
            return fail(ErrorCode::InvalidFormat, "replacement for sound {} has no usable audio", i + 1);
        if (records[i].type == 3) { // 16-bit PCM stays PCM
            ByteBuffer pcm(clip.samples.size() * 2);
            std::memcpy(pcm.data(), clip.samples.data(), pcm.size());
            newData[i] = std::move(pcm);
        } else {
            newData[i] = baf::encodeAdpcm(clip.samples, clip.channels);
        }
    }
    ByteBuffer out = write(newData);
    // New format details for the replaced sounds (names, volume and loop flags are kept).
    for (std::size_t i = 0; i < replacements.size(); ++i) {
        if (!replacements[i])
            continue;
        const AudioClip& clip = *replacements[i];
        const std::size_t at = records[i].at;
        const auto putU32 = [&](std::size_t offset, std::uint32_t v) { std::memcpy(out.data() + at + offset, &v, 4); };
        if (records[i].type == 3) {
            putU32(60, clip.sampleRate);
            putU32(64, 16);
            putU32(68, clip.channels);
        } else {
            putU32(64, clip.sampleRate);
            putU32(68, static_cast<std::uint32_t>(clip.frames()));
            out[at + 75] = static_cast<std::byte>(clip.channels);
        }
    }
    return out;
}

} // namespace viewer::games::blur
