/*
    XoaFilePlayerTests.cpp - WP6 file-I/O layer tests.

    C1 spike (DEVPLAN WP6: "verify JUCE WavAudioFormat/CAF actually reads
    121-128-channel files on all three OSes before building on it"): this
    suite IS the spike - green CI on the three OSes is the verdict that
    FR-8 file playback can be built on stock juce_audio_formats.

    Format scope (per plan): WAV (float32, any channel count up to
    kMaxFileChannels) and FLAC (<= 8 channels - a FLAC format-spec cap, not
    a JUCE limitation) everywhere; CAF is macOS-only via CoreAudioFormat and
    is exercised manually there (no cross-platform writer exists to test
    with headlessly).

    Sample values encode both the channel index and the sample index as
    exactly-representable dyadic floats, so a round-trip proves channel
    mapping and sample ordering, not just "some data came back".
*/

#include <juce_audio_formats/juce_audio_formats.h>

#include "XoaConstants.h"
#include "XoaTestFramework.h"

#include <memory>

namespace
{

//==============================================================================
// (ch+1)/256 + i/65536: both terms dyadic, sum uses 16 mantissa bits -> exact
// in float32, and (value * 2^23) is an integer -> exact in 24-bit FLAC too.
float encodedSample (int channel, int sampleIndex)
{
    return (float) (channel + 1) / 256.0f + (float) sampleIndex / 65536.0f;
}

juce::AudioBuffer<float> makeEncodedBuffer (int numChannels, int numSamples)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* data = buffer.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
            data[i] = encodedSample (ch, i);
    }
    return buffer;
}

/** Write with the given format, read back, and verify shape + content.
    Returns the worst absolute error seen (or a huge value on hard failure)
    so callers can assert format-appropriate tolerances. */
double writeReadRoundTrip (juce::AudioFormat& format, const juce::File& file,
                           int numChannels, int numSamples, int bitsPerSample,
                           bool expectFloatData)
{
    const auto source = makeEncodedBuffer (numChannels, numSamples);

    file.deleteFile();
    {
        auto fileStream = std::make_unique<juce::FileOutputStream> (file);
        CHECK (fileStream->openedOk());
        if (! fileStream->openedOk())
            return 1.0e9;

        std::unique_ptr<juce::OutputStream> out (std::move (fileStream));
        const auto options =
            juce::AudioFormatWriterOptions{}
                .withSampleRate (48000.0)
                .withNumChannels (numChannels)
                .withBitsPerSample (bitsPerSample)
                .withSampleFormat (expectFloatData
                                       ? juce::AudioFormatWriterOptions::SampleFormat::floatingPoint
                                       : juce::AudioFormatWriterOptions::SampleFormat::automatic);

        auto writer = format.createWriterFor (out, options);
        CHECK (writer != nullptr);
        if (writer == nullptr)
            return 1.0e9;

        CHECK (writer->writeFromAudioSampleBuffer (source, 0, numSamples));
    }   // writer destructor flushes the header

    std::unique_ptr<juce::AudioFormatReader> reader (
        format.createReaderFor (new juce::FileInputStream (file), true));
    CHECK (reader != nullptr);
    if (reader == nullptr)
        return 1.0e9;

    CHECK ((int) reader->numChannels == numChannels);
    CHECK (reader->lengthInSamples == (juce::int64) numSamples);
    CHECK (reader->usesFloatingPointData == expectFloatData);
    if ((int) reader->numChannels != numChannels
        || reader->lengthInSamples < (juce::int64) numSamples)
        return 1.0e9;

    juce::AudioBuffer<float> loaded (numChannels, numSamples);
    CHECK (reader->read (&loaded, 0, numSamples, 0, true, true));

    double worstError = 0.0;
    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float* data = loaded.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            worstError = juce::jmax (worstError,
                                     std::abs ((double) data[i]
                                               - (double) encodedSample (ch, i)));
    }
    return worstError;
}

//==============================================================================
void testWav121Channels (const juce::File& dir)
{
    juce::WavAudioFormat wav;
    const double err = writeReadRoundTrip (wav, dir.getChildFile ("spike-121.wav"),
                                           xoa::kNumSHChannels, 480, 32, true);
    CHECK (err == 0.0);   // float32 WAV must round-trip bit-exactly
}

void testWav128Channels (const juce::File& dir)
{
    juce::WavAudioFormat wav;
    const double err = writeReadRoundTrip (wav, dir.getChildFile ("spike-128.wav"),
                                           xoa::kMaxFileChannels, 480, 32, true);
    CHECK (err == 0.0);
}

void testFlac8Channels (const juce::File& dir)
{
    juce::FlacAudioFormat flac;
    const double err = writeReadRoundTrip (flac, dir.getChildFile ("spike-8.flac"),
                                           8, 480, 24, false);
    // The encoded values are exact 24-bit fixed-point values; allow one
    // quantization step of slack for the int<->float scaling convention.
    CHECK (err <= 1.0 / 8388608.0);
}

void testGarbageFileRejected (const juce::File& dir)
{
    const auto file = dir.getChildFile ("not-audio.wav");
    file.replaceWithText ("XOA: definitely not a RIFF header");

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> wavReader (
        wav.createReaderFor (new juce::FileInputStream (file), true));
    CHECK (wavReader == nullptr);

    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> flacReader (
        flac.createReaderFor (new juce::FileInputStream (file), true));
    CHECK (flacReader == nullptr);
}

} // namespace

//==============================================================================
void runXoaFilePlayerTests()
{
    ScopedTempDir tmp;
    testWav121Channels (tmp.dir);
    testWav128Channels (tmp.dir);
    testFlac8Channels (tmp.dir);
    testGarbageFileRejected (tmp.dir);
}
