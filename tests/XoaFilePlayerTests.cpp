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

#include "Audio/FilePlayer.h"

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

//==============================================================================
// F5 - the FilePlayer streaming path. A constant per-channel signal is
// resampling-invariant, so a rendered block must equal (c+1)*0.1 on channel c
// regardless of read-ahead warm-up or rate correction.
//==============================================================================

// Write a numChannels x numSamples WAV whose channel c is the constant (c+1)*0.1.
juce::File writeConstantWav (const juce::File& dir, int numChannels, int numSamples, double sr)
{
    juce::AudioBuffer<float> buffer (numChannels, numSamples);
    for (int c = 0; c < numChannels; ++c)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (c),
                                           0.1f * (float) (c + 1), numSamples);

    const auto file = dir.getChildFile ("player-" + juce::String (numChannels) + "ch.wav");
    file.deleteFile();

    auto fileStream = std::make_unique<juce::FileOutputStream> (file);
    std::unique_ptr<juce::OutputStream> os (std::move (fileStream));
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions {}
                             .withSampleRate (sr)
                             .withNumChannels (numChannels)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor (os, options);
    CHECK (writer != nullptr);
    if (writer != nullptr)
        writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
    return file;
}

// Render blocks until non-silent (read-ahead warm-up) or a bounded number of
// attempts elapse. Returns true if audio arrived.
bool renderUntilAudio (xoa::FilePlayer& player, juce::AudioBuffer<float>& block, int numSamples)
{
    for (int attempt = 0; attempt < 400; ++attempt)
    {
        player.renderNextBlock (block, numSamples);
        if (block.getMagnitude (0, 0, numSamples) > 1.0e-6f)
            return true;
        juce::Thread::sleep (2);
    }
    return false;
}

void testFilePlayerOrderDetection()
{
    CHECK (xoa::FilePlayer::detectAmbiOrder (4) == 1);
    CHECK (xoa::FilePlayer::detectAmbiOrder (16) == 3);
    CHECK (xoa::FilePlayer::detectAmbiOrder (121) == 10);
    CHECK (xoa::FilePlayer::detectAmbiOrder (7) == 0);     // not a perfect square
    CHECK (xoa::FilePlayer::detectAmbiOrder (144) == 0);   // order 11 > bus order -> no fit
}

void testFilePlayerStreaming (const juce::File& dir)
{
    const int numCh = 4, block = 512;
    const double sr = 48000.0;
    const int lengthSamples = 48000;   // 1 second

    const auto file = writeConstantWav (dir, numCh, lengthSamples, sr);

    xoa::FilePlayer player;
    const auto r = player.open (file);
    CHECK (r.ok);
    CHECK (r.numChannels == numCh);
    CHECK (r.fileSampleRate == sr);
    CHECK (r.lengthSamples == lengthSamples);
    CHECK (r.detectedOrder == 1);           // (1+1)^2 = 4
    CHECK (player.getNumChannels() == numCh);

    player.prepareToPlay (sr, block);

    // Stopped -> silence.
    juce::AudioBuffer<float> buf (numCh, block);
    buf.clear();
    player.renderNextBlock (buf, block);
    CHECK (buf.getMagnitude (0, 0, block) == 0.0f);

    // Playing -> the constants (after read-ahead warm-up).
    player.play();
    CHECK (player.isPlaying());
    CHECK (renderUntilAudio (player, buf, block));
    for (int c = 0; c < numCh; ++c)
    {
        const float expected = 0.1f * (float) (c + 1);
        CHECK (std::abs (buf.getSample (c, 0) - expected) < 1.0e-3f);
        CHECK (std::abs (buf.getSample (c, block - 1) - expected) < 1.0e-3f);
    }

    // Seek + length report.
    CHECK (std::abs (player.getLengthSeconds() - 1.0) < 1.0e-3);
    player.seekSeconds (0.5);
    CHECK (std::abs (player.getPositionSeconds() - 0.5) < 0.05);

    // Loop smoke: seek near the end, keep rendering past it, still get audio.
    player.setLooping (true);
    player.seekSeconds (0.98);
    CHECK (renderUntilAudio (player, buf, block));

    player.stop();
    player.close();
    CHECK (player.getNumChannels() == 0);
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
    testFilePlayerOrderDetection();
    testFilePlayerStreaming (tmp.dir);
}
