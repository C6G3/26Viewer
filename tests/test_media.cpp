// Shared media helpers: WAV encode/decode, half floats, byte sources.

#include "TestUtil.hpp"

#include "viewer/core/Audio.hpp"
#include "viewer/core/ByteSource.hpp"
#include "viewer/core/Mesh.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace viewer;

TEST_CASE("WAV files round-trip 16-bit PCM")
{
    AudioClip clip;
    clip.sampleRate = 22050;
    clip.channels = 2;
    clip.samples = {0, 1, -1, 32767, -32768, 1234};
    const ByteBuffer wav = audio::encodeWav(clip);
    CHECK(wav.size() == 44 + clip.samples.size() * 2);

    auto back = audio::decodeWav(wav);
    REQUIRE(back);
    CHECK(back->sampleRate == 22050);
    CHECK(back->channels == 2);
    CHECK(back->samples == clip.samples);
    CHECK(back->frames() == 3);
}

TEST_CASE("Wave files open as a one-sound bank")
{
    AudioClip clip{8000, 1, {1, 2, 3, 4}};
    const auto format = audio::wavFormat();
    const ByteBuffer wav = audio::encodeWav(clip);
    REQUIRE(format->canOpen("wav", wav));
    auto bank = format->open(memorySource(wav));
    REQUIRE(bank);
    REQUIRE((*bank)->sounds().size() == 1);
    CHECK((*bank)->sounds()[0].frames == 4);
    CHECK((*bank)->decode(0)->samples == clip.samples);
    CHECK_FALSE((*bank)->decode(1));
}

TEST_CASE("Broken wave files are rejected")
{
    CHECK_FALSE(audio::decodeWav(test::sampleData(64)));
    ByteBuffer wav = audio::encodeWav(AudioClip{8000, 1, {1, 2}});
    wav.resize(30); // cut inside the fmt chunk
    CHECK_FALSE(audio::decodeWav(wav));
}

TEST_CASE("Half floats convert to float")
{
    CHECK(mesh::halfToFloat(0x0000) == 0.0f);
    CHECK(mesh::halfToFloat(0x3C00) == 1.0f);
    CHECK(mesh::halfToFloat(0xC000) == -2.0f);
    CHECK(mesh::halfToFloat(0x3800) == 0.5f);
    CHECK(mesh::halfToFloat(0x0001) > 0.0f); // smallest subnormal
}

TEST_CASE("Byte sources read ranges and refuse reads past the end")
{
    const auto bytes = test::sampleData(100);
    const auto source = memorySource(bytes);
    CHECK(source->size() == 100);
    CHECK(source->read(10, 5).value() == ByteBuffer(bytes.begin() + 10, bytes.begin() + 15));
    CHECK_FALSE(source->read(98, 5));

    test::TempFile file("source.bin", bytes);
    auto onDisk = fileSource(file.path);
    REQUIRE(onDisk);
    CHECK((*onDisk)->read(90, 10).value() == ByteBuffer(bytes.begin() + 90, bytes.end()));
}
