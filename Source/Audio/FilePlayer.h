#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include <memory>

#include "XoaConstants.h"

//==============================================================================
// XOA - multichannel Ambisonics file playback (FR-8).
//
// Streams a WAV/FLAC (and CAF on macOS) of up to kMaxFileChannels through a
// juce::AudioTransportSource fed by a background read-ahead thread: a
// 121-channel order-10 file is far too large to preload, and the transport
// gives seek/loop and sample-rate correction (any file rate -> the device
// rate, FR-4) for free.
//
// AmbiX metadata detection is a channel-count heuristic (JUCE parses no HOA
// chunks): the largest order whose (N+1)^2 fits the channel count. The manual
// order/convention override in the parameter store always wins (FR-8).
//
// Threading: open/close/transport control are message-thread; renderNextBlock
// is the audio thread. AudioTransportSource::getNextAudioBlock takes a short
// CriticalSection (contended only during a message-thread setSource/stop) - the
// documented M1 tradeoff; a lock-free SPSC ring is the post-M1 option.
//==============================================================================

namespace xoa
{

class FilePlayer
{
public:
    struct OpenResult
    {
        bool ok = false;
        juce::String error;
        int numChannels = 0;
        double fileSampleRate = 0.0;
        juce::int64 lengthSamples = 0;
        int detectedOrder = 0;              // AmbiX heuristic; 0 if not a perfect square fit
        juce::StringArray warnings;
    };

    FilePlayer();
    ~FilePlayer();

    /** Detect the largest Ambisonic order whose (N+1)^2 <= channelCount,
        capped at the bus order. 0 when channelCount isn't a perfect square. */
    static int detectAmbiOrder (int channelCount) noexcept;

    //==========================================================================
    // Message thread.
    //==========================================================================
    OpenResult open (const juce::File& file);
    void close();

    void play();
    void stop();
    bool isPlaying() const;
    void setLooping (bool shouldLoop);

    void   seekSeconds (double seconds);
    double getPositionSeconds() const;
    double getLengthSeconds() const;
    int    getNumChannels() const noexcept { return numChannels; }

    //==========================================================================
    // Audio thread.
    //==========================================================================
    void prepareToPlay (double deviceSampleRate, int blockSize);
    /** Fill dest with the next numSamples (cleared when stopped or empty). */
    void renderNextBlock (juce::AudioBuffer<float>& dest, int numSamples) noexcept;
    void releaseResources();

private:
    juce::AudioFormatManager formatManager;
    juce::TimeSliceThread    readAheadThread { "xoa file read-ahead" };
    juce::AudioTransportSource transport;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    int    numChannels = 0;
    double deviceSampleRate = 0.0;
    int    blockSize = 0;
    bool   looping = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilePlayer)
};

} // namespace xoa
