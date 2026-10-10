#pragma once

#include "viewer/core/Archive.hpp"
#include "viewer/core/ByteSource.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace viewer {

/// Decoded audio: signed 16-bit PCM, channels interleaved (L R L R ...).
struct AudioClip {
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::vector<std::int16_t> samples;

    [[nodiscard]] std::uint64_t frames() const noexcept { return channels ? samples.size() / channels : 0; }
    [[nodiscard]] double seconds() const noexcept
    {
        return sampleRate ? static_cast<double>(frames()) / sampleRate : 0.0;
    }
};

/// What a sound bank says about one of its sounds (before decoding it).
struct SoundInfo {
    std::string name;
    std::uint32_t sampleRate = 0;
    std::uint16_t channels = 0;
    std::uint64_t frames = 0; ///< Samples per channel.
    bool looped = false;
    std::string codec;
    std::uint64_t dataSize = 0; ///< Encoded bytes.

    [[nodiscard]] double seconds() const noexcept
    {
        return sampleRate ? static_cast<double>(frames) / sampleRate : 0.0;
    }
};

/// A container of sounds (a game's audio bank, or a single .wav file).
/// decode() must be thread-safe.
class SoundBank {
public:
    virtual ~SoundBank() = default;
    [[nodiscard]] virtual std::string_view formatName() const noexcept = 0;
    [[nodiscard]] virtual std::span<const SoundInfo> sounds() const noexcept = 0;
    [[nodiscard]] virtual Result<AudioClip> decode(std::size_t index) const = 0;
    [[nodiscard]] virtual PropertyList describe() const { return {}; }
};

/// Opens sound banks of one format from a byte source (only the parts that
/// are needed are read, so huge streaming banks stay cheap).
class SoundBankFormat {
public:
    virtual ~SoundBankFormat() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    /// `header` holds up to the first 4 KiB of the file.
    [[nodiscard]] virtual bool canOpen(std::string_view extension, ByteSpan header) const = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<SoundBank>> open(std::shared_ptr<const ByteSource> source) const = 0;
};

namespace audio {

/// A RIFF/WAVE file (16-bit PCM) holding `clip`.
[[nodiscard]] ByteBuffer encodeWav(const AudioClip& clip);

/// Reads 8/16-bit PCM .wav files.
Result<AudioClip> decodeWav(ByteSpan file);

/// Treats a plain .wav file as a bank with one sound.
[[nodiscard]] std::shared_ptr<const SoundBankFormat> wavFormat();

} // namespace audio
} // namespace viewer
