#pragma once

#include "viewer/core/Audio.hpp"

namespace viewer::games::blur {

/// Blur ".baf" audio banks ("BANK" + one "WAVE" record per sound). See docs/formats/blur.md.
class BafFormat final : public SoundBankFormat {
public:
    std::string_view name() const noexcept override { return "Blur audio bank"; }
    bool canOpen(std::string_view extension, ByteSpan header) const override;
    Result<std::unique_ptr<SoundBank>> open(std::shared_ptr<const ByteSource> source) const override;
    bool canRebuild() const noexcept override { return true; }
    /// Re-encodes replaced sounds (ADPCM, or PCM for PCM sounds) and lays the data out
    /// like the game does: "DATA" + size before each sound, 2 KiB-aligned in streaming banks.
    Result<ByteBuffer> rebuild(ByteSpan original, std::span<const std::optional<AudioClip>> replacements) const override;
};

namespace baf {

/// Decodes Blur's 4-bit ADPCM: 33-byte frames (1 header byte = filter << 4 |
/// shift, then 32 bytes = 64 samples, low nibble first), channels alternating
/// frame by frame. `frames` caps the output length per channel.
[[nodiscard]] std::vector<std::int16_t> decodeAdpcm(ByteSpan data, unsigned channels, std::uint64_t frames);

/// The inverse: encodes interleaved 16-bit samples, choosing for each frame the
/// filter and shift that come closest when played back by decodeAdpcm. The last
/// frame is padded with silence.
[[nodiscard]] ByteBuffer encodeAdpcm(std::span<const std::int16_t> samples, unsigned channels);

} // namespace baf
} // namespace viewer::games::blur
