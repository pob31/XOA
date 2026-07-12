#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <type_traits>
#include <vector>

#include "XoaConstants.h"
#include "Parameters/XoaParameterIDs.h"
#include "Parameters/XoaValueTreeState.h"

//==============================================================================
// XOA - per-speaker compensation parameters (WP7, FR-15). Two channels:
//
//   SpeakerCompRtParams  a trivially-copyable POD published via RtSnapshot:
//                        per-speaker delay (ms) and a single linear gain that
//                        folds trim x distance attenuation x mute/solo. Because
//                        it is a value POD, publishing again mid-block is safe
//                        and mute/solo/trim edits need no debounce.
//   SpeakerEqParams      the 6-band EQ config, message-side only (fed to the
//                        processor's RT-owned biquads via setEqParameters).
//
// Both are pure functions of the store, computed on the message thread. Delay
// is carried in MILLISECONDS (sample-rate independent), so a device restart at
// a new rate needs no recompose. Distance compensation is derived from speaker
// positions (not stored schema): delay aligns every speaker to the FARTHEST
// (delay_s = (r_max - r_s)/c), and the gain law is attenuate-only referenced to
// the farthest speaker (clamp(20 log10(r_s/r_max), -24, 0) dB - headroom-safe).
//==============================================================================

namespace xoa
{

/** Total per-output delay-line capacity: the speakerDelay trim range (500 ms)
    plus the distance-alignment headroom (~300 ms at ~100 m). */
constexpr double kMaxCompDelayMs = 800.0;

// kSpeedOfSound now lives in XoaConstants.h (shared with the WP8 NFC filters).

struct SpeakerCompRtParams
{
    int numSpeakers = 0;
    juce::uint32 epoch = 0;
    float delayMs[xoa::kMaxSpeakers] = {};      // trim + distance alignment
    float gainLinear[xoa::kMaxSpeakers] = {};   // trim x distance atten x mute/solo (0 = silenced)
};

static_assert (std::is_trivially_copyable_v<SpeakerCompRtParams>,
               "SpeakerCompRtParams must be a POD for RtSnapshot");

struct SpeakerEqBand
{
    int   shape  = 0;        // OutputEQBiquadFilter shape 0..7
    float freq   = 1000.0f;
    float gainDb = 0.0f;
    float q      = 0.707f;
    float slope  = 1.0f;
};

struct SpeakerEqParams
{
    int  numSpeakers = 0;
    bool enabled[xoa::kMaxSpeakers] = {};
    SpeakerEqBand bands[xoa::kMaxSpeakers][xoa::kNumEqBands];
};

//==============================================================================
inline SpeakerCompRtParams composeSpeakerCompParams (const XoaValueTreeState& store,
                                                     juce::uint32 epoch)
{
    SpeakerCompRtParams p;
    const int L = juce::jmin (store.getNumSpeakers(), xoa::kMaxSpeakers);
    p.numSpeakers = L;
    p.epoch = epoch;

    const int mode = store.getIntParameter (ids::distanceCompMode);   // 0 off, 1 delay, 2 delay+gain

    std::vector<double> radius ((size_t) L, 0.0);
    double rMax = 0.0;
    bool anySolo = false;
    for (int s = 0; s < L; ++s)
    {
        const double x = store.getFloatParameter (ids::speakerPositionX, s);
        const double y = store.getFloatParameter (ids::speakerPositionY, s);
        const double z = store.getFloatParameter (ids::speakerPositionZ, s);
        radius[(size_t) s] = std::sqrt (x * x + y * y + z * z);
        rMax = juce::jmax (rMax, radius[(size_t) s]);
        if (static_cast<bool> (store.getParameter (ids::speakerSolo, s)))
            anySolo = true;
    }

    for (int s = 0; s < L; ++s)
    {
        // Delay: trim + (mode >= 1) distance alignment to the farthest speaker.
        double delayMs = store.getFloatParameter (ids::speakerDelay, s);
        if (mode >= 1)
            delayMs += (rMax - radius[(size_t) s]) / kSpeedOfSound * 1000.0;
        p.delayMs[s] = (float) juce::jlimit (0.0, kMaxCompDelayMs, delayMs);

        // Gain: trim (dB) + (mode >= 2) attenuate-only distance law, x mute/solo.
        double gainDb = store.getFloatParameter (ids::speakerGain, s);
        if (mode >= 2 && rMax > 0.0)
            gainDb += juce::jlimit (-24.0, 0.0,
                                    20.0 * std::log10 (juce::jmax (radius[(size_t) s], 0.1) / rMax));

        const bool muted  = static_cast<bool> (store.getParameter (ids::speakerMute, s));
        const bool soloed = static_cast<bool> (store.getParameter (ids::speakerSolo, s));
        const bool audible = (! anySolo || soloed) && ! muted;   // mute wins over solo
        p.gainLinear[s] = audible ? (float) std::pow (10.0, gainDb / 20.0) : 0.0f;
    }
    return p;
}

inline SpeakerEqParams composeSpeakerEqParams (const XoaValueTreeState& store)
{
    SpeakerEqParams p;
    const int L = juce::jmin (store.getNumSpeakers(), xoa::kMaxSpeakers);
    p.numSpeakers = L;

    for (int s = 0; s < L; ++s)
    {
        p.enabled[s] = static_cast<bool> (store.getParameter (ids::speakerEqEnabled, s));
        for (int b = 0; b < xoa::kNumEqBands; ++b)
        {
            auto& band = p.bands[s][b];
            band.shape  = (int) store.getEqBandParameter (s, b, ids::eqShape);
            band.freq   = (float) (double) store.getEqBandParameter (s, b, ids::eqFrequency);
            band.gainDb = (float) (double) store.getEqBandParameter (s, b, ids::eqGain);
            band.q      = (float) (double) store.getEqBandParameter (s, b, ids::eqQ);
            band.slope  = (float) (double) store.getEqBandParameter (s, b, ids::eqSlope);
        }
    }
    return p;
}

} // namespace xoa
