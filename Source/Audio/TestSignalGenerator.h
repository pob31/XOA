#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cmath>

//==============================================================================
// XOA - output test-signal generator (WP7, FR-21). Ported header-only from
// WFS-DIY's TestSignalGenerator with two XOA changes:
//
//   1. DETERMINISTIC seed. prepare() seeds the pink-noise RNG from a fixed
//      constant (WFS-DIY seeded from the wall clock), so a render is fully
//      reproducible - the offline tests can assert bit-equality across two
//      fresh generators.
//   2. A SpeakerId mode. It steps a pink-noise burst across every output in
//      turn (0.75 s on / 0.25 s gap), declicked at the burst edges, and exposes
//      getCurrentSpeakerIndex() so the UI can name the speaker under test. The
//      other modes keep WFS-DIY's single-target-channel behaviour.
//
// Injected by AudioEngine AFTER the decode + compensation, with REPLACE
// semantics on the target channel (it overwrites, so the operator hears only
// the signal). Thread model: control setters are message-thread + atomic;
// renderNextBlock() and prepare() own the generator state (prepare only runs
// with the device stopped).
//==============================================================================

namespace xoa
{

class TestSignalGenerator
{
public:
    enum class SignalType
    {
        Off,
        PinkNoise,
        Tone,
        Sweep,
        DiracPulse,
        SpeakerId
    };

    TestSignalGenerator() = default;

    //==========================================================================
    // Setup (message thread / device start).
    //==========================================================================
    void prepare (double newSampleRate, int /*maxBlockSize*/)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

        for (auto& s : pinkNoiseState) s = 0.0f;
        random.setSeed (kSeed);          // deterministic (WFS-DIY used the clock)

        phase = 0.0f;
        phaseIncrement = frequency / (float) sampleRate;
        sweepPosition = 0.0f;
        pulsePosition = 0.0f;
        speakerIdPosition = 0.0f;
        fadePosition.store (0.0f);
        currentSpeakerIndex.store (-1);
    }

    //==========================================================================
    // Control (message thread; atomics).
    //==========================================================================
    void setSignalType (SignalType type)
    {
        if (currentType.exchange (type) != type)
        {
            phase = 0.0f;
            sweepPosition = 0.0f;
            pulsePosition = 0.0f;
            speakerIdPosition = 0.0f;
            // 500 ms fade-in for the continuous tones; none for the transient /
            // stepping modes (they carry their own envelopes).
            fadePosition.store ((type == SignalType::PinkNoise || type == SignalType::Tone) ? 0.0f : 1.0f);
        }
    }

    void setFrequency (float hz)
    {
        frequency = juce::jlimit (20.0f, 20000.0f, hz);
        phaseIncrement = frequency / (float) sampleRate;
    }

    void setLevel (float dB)        { levelLinear.store (juce::Decibels::decibelsToGain (dB)); }
    void setOutputChannel (int ch)  { targetChannel.store (ch); }

    SignalType getSignalType() const noexcept { return currentType.load(); }
    float      getLevelDb()    const           { return juce::Decibels::gainToDecibels (levelLinear.load()); }
    float      getFrequency()  const noexcept { return frequency; }
    int        getOutputChannel() const noexcept { return targetChannel.load(); }

    /** SpeakerId mode: the output currently under test, or -1 in a gap/inactive. */
    int getCurrentSpeakerIndex() const noexcept { return currentSpeakerIndex.load(); }

    bool isActive() const noexcept
    {
        const auto t = currentType.load();
        if (t == SignalType::Off)       return false;
        if (t == SignalType::SpeakerId) return true;      // steps every output itself
        return targetChannel.load() >= 0;
    }

    void reset()
    {
        targetChannel.store (-1);
        currentType.store (SignalType::Off);
        fadePosition.store (0.0f);
        currentSpeakerIndex.store (-1);
    }

    //==========================================================================
    // Audio thread. REPLACES the target channel(s) with the generated signal.
    //==========================================================================
    void renderNextBlock (juce::AudioBuffer<float>& outputBuffer, int startSample, int numSamples)
    {
        const SignalType type = currentType.load();
        const float level = levelLinear.load();

        if (type == SignalType::Off || numSamples <= 0)
            return;

        if (type == SignalType::SpeakerId)
        {
            renderSpeakerId (outputBuffer, startSample, numSamples, level);
            return;
        }

        const int channel = targetChannel.load();
        if (channel < 0 || channel >= outputBuffer.getNumChannels())
            return;

        float* data = outputBuffer.getWritePointer (channel, startSample);
        float fade = fadePosition.load();
        const float fadeStep = 1.0f / (kFadeDuration * (float) sampleRate);

        for (int i = 0; i < numSamples; ++i)
        {
            float sample = 0.0f;
            switch (type)
            {
                case SignalType::PinkNoise:  sample = generatePinkNoise(); break;
                case SignalType::Tone:
                    sample = std::sin (phase * juce::MathConstants<float>::twoPi);
                    phase += phaseIncrement;
                    if (phase >= 1.0f) phase -= 1.0f;
                    break;
                case SignalType::Sweep:      sample = generateSweep(); break;
                case SignalType::DiracPulse: sample = generateDirac(); break;
                default:                     sample = 0.0f; break;
            }

            data[i] = sample * level * fade;
            if (fade < 1.0f) fade = juce::jmin (1.0f, fade + fadeStep);
        }

        fadePosition.store (fade);
    }

private:
    //==========================================================================
    void renderSpeakerId (juce::AudioBuffer<float>& outputBuffer, int startSample,
                          int numSamples, float level)
    {
        const int numOut = outputBuffer.getNumChannels();
        if (numOut <= 0)
        {
            currentSpeakerIndex.store (-1);
            return;
        }

        const double dt = 1.0 / sampleRate;
        const double cycle = (double) numOut * kSpeakerIdSlot;   // one full sweep of the rig
        int lastSpeaker = -1;

        for (int i = 0; i < numSamples; ++i)
        {
            const int    speaker   = (int) (speakerIdPosition / kSpeakerIdSlot);
            const double withinSlot = speakerIdPosition - (double) speaker * kSpeakerIdSlot;

            if (withinSlot < kSpeakerIdBurst && speaker >= 0 && speaker < numOut)
            {
                // Raised-edge envelope over the burst (declick both ends).
                float env = 1.0f;
                if (withinSlot < kDeclick)
                    env = (float) (withinSlot / kDeclick);
                else if (withinSlot > kSpeakerIdBurst - kDeclick)
                    env = (float) ((kSpeakerIdBurst - withinSlot) / kDeclick);

                outputBuffer.getWritePointer (speaker, startSample)[i] = generatePinkNoise() * level * env;
                lastSpeaker = speaker;
            }
            // else: gap - leave the (decoded) output untouched on every channel.

            speakerIdPosition += dt;
            if (speakerIdPosition >= cycle)
                speakerIdPosition -= cycle;
        }

        currentSpeakerIndex.store (lastSpeaker);
    }

    float generatePinkNoise()
    {
        // Paul Kellett's refined method (7-pole, ~1/f from 20 Hz to Nyquist).
        const float white = random.nextFloat() * 2.0f - 1.0f;

        pinkNoiseState[0] = 0.99886f * pinkNoiseState[0] + white * 0.0555179f;
        pinkNoiseState[1] = 0.99332f * pinkNoiseState[1] + white * 0.0750759f;
        pinkNoiseState[2] = 0.96900f * pinkNoiseState[2] + white * 0.1538520f;
        pinkNoiseState[3] = 0.86650f * pinkNoiseState[3] + white * 0.3104856f;
        pinkNoiseState[4] = 0.55000f * pinkNoiseState[4] + white * 0.5329522f;
        pinkNoiseState[5] = -0.7616f * pinkNoiseState[5] - white * 0.0168980f;

        const float pink = pinkNoiseState[0] + pinkNoiseState[1] + pinkNoiseState[2]
                         + pinkNoiseState[3] + pinkNoiseState[4] + pinkNoiseState[5]
                         + pinkNoiseState[6] + white * 0.5362f;

        pinkNoiseState[6] = white * 0.115926f;
        return pink * 0.11f;   // approximate normalisation
    }

    float generateSweep()
    {
        // Log sweep 20 Hz -> 20 kHz over 1 s, then a 3 s gap.
        if (sweepPosition < kSweepDuration)
        {
            const float t = sweepPosition / kSweepDuration;
            const float logStart = std::log (kSweepStartHz);
            const float logEnd   = std::log (kSweepEndHz);
            const float freq = std::exp (logStart + t * (logEnd - logStart));

            const float sample = std::sin (phase * juce::MathConstants<float>::twoPi);
            phase += freq / (float) sampleRate;
            if (phase >= 1.0f) phase -= 1.0f;

            sweepPosition += 1.0f / (float) sampleRate;
            return sample;
        }

        sweepPosition += 1.0f / (float) sampleRate;
        if (sweepPosition >= kSweepDuration + kSweepGap)
        {
            sweepPosition = 0.0f;
            phase = 0.0f;
        }
        return 0.0f;
    }

    float generateDirac()
    {
        const float sample = (pulsePosition < kPulseDuration) ? kPulseAmplitude : 0.0f;
        pulsePosition += 1.0f / (float) sampleRate;
        if (pulsePosition >= kPulseDuration + kPulseGap)
            pulsePosition = 0.0f;
        return sample;
    }

    //==========================================================================
    static constexpr int   kSeed = 0x0A0Au;

    static constexpr float kFadeDuration  = 0.5f;      // 500 ms fade-in (tones)

    static constexpr float kSweepDuration = 1.0f;
    static constexpr float kSweepGap      = 3.0f;
    static constexpr float kSweepStartHz  = 20.0f;
    static constexpr float kSweepEndHz    = 20000.0f;

    static constexpr float kPulseDuration = 0.005f;    // 5 ms burst
    static constexpr float kPulseGap      = 1.0f;      // 1 s between pulses
    static constexpr float kPulseAmplitude = 2.0f;

    static constexpr double kSpeakerIdBurst = 0.75;    // seconds on
    static constexpr double kSpeakerIdSlot  = 1.0;     // burst + 0.25 s gap
    static constexpr double kDeclick        = 0.010;   // 10 ms edge fade

    // Control (atomic, message thread <-> audio thread).
    std::atomic<SignalType> currentType { SignalType::Off };
    std::atomic<int>        targetChannel { -1 };
    std::atomic<float>      levelLinear { 0.01f };      // -40 dB default
    std::atomic<float>      fadePosition { 0.0f };
    std::atomic<int>        currentSpeakerIndex { -1 };

    // Generator state (audio thread + prepare).
    double sampleRate = 48000.0;
    float  frequency = 1000.0f;
    float  phase = 0.0f;
    float  phaseIncrement = 1000.0f / 48000.0f;
    float  sweepPosition = 0.0f;
    float  pulsePosition = 0.0f;
    double speakerIdPosition = 0.0;
    float  pinkNoiseState[7] = {};
    juce::Random random;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TestSignalGenerator)
};

} // namespace xoa
