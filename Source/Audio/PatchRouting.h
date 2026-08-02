#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include "XoaConstants.h"
#include "Parameters/XoaValueTreeState.h"

//==============================================================================
// XOA - patch routing (stage 2, D41-D43). The message-thread composer that
// turns the AudioPatch trees into the POD the audio thread routes by:
//
//   stem-scratch row k  <-  hardware input  hwForStemChannel[k]   (-1 = silent)
//   speaker row s       ->  hardware output hwForSpeaker[s]       (-1 = discard)
//
// Rows are FLATTENED stem channels (one per channel of every input's span,
// D43), so an HOA group may sit on arbitrary, non-contiguous hardware
// channels. When a patch tree is absent the composer falls back to the
// identity mapping, which reproduces pre-patch behaviour exactly.
//
// The POD also carries the per-input spans so the audio thread can meter an
// input across its whole group without a second snapshot.
//==============================================================================

namespace xoa::rt
{

struct PatchRtState
{
    int numStemChannels = 0;                       // flattened stem rows in use
    int numInputs = 0;
    int hwForStemChannel[xoa::kMaxStemChannels] = {};
    int hwForSpeaker[xoa::kMaxSpeakers] = {};
    int stemOffset[xoa::kMaxInputs] = {};          // per input: first flattened row
    int stemSpan[xoa::kMaxInputs] = {};            // per input: row count
    juce::uint32 epoch = 0;                        // 0 = never published
};

static_assert (std::is_trivially_copyable_v<PatchRtState>,
               "PatchRtState must be a POD for RtSnapshot");

/** Hardware column of a 1:1 patch row ("0,0,1,0..."), or -1 when unpatched. */
inline int patchRowColumn (const juce::String& row) noexcept
{
    const auto cells = juce::StringArray::fromTokens (row, ",", "");
    for (int c = 0; c < cells.size(); ++c)
        if (cells[c].getIntValue() == 1)
            return c;
    return -1;
}

/** Compose the routing POD from the store's AudioPatch trees (message
    thread; also driven directly by the offline tests). */
inline PatchRtState composePatchRouting (const XoaValueTreeState& store, juce::uint32 epoch)
{
    PatchRtState p;
    p.epoch = epoch;

    // Spans first: they define how many flattened rows exist.
    p.numInputs = juce::jmin (store.getNumInputs(), xoa::kMaxInputs);
    int offset = 0;
    for (int i = 0; i < p.numInputs; ++i)
    {
        p.stemOffset[i] = offset;
        p.stemSpan[i]   = store.getInputChannelCount (i);
        offset += p.stemSpan[i];
    }
    p.numStemChannels = juce::jmin (offset, xoa::kMaxStemChannels);

    // Input side. Identity when the tree is absent; rows the tree does not
    // cover stay identity as well (the store's reconcile keeps them in step,
    // so this is a startup/foreign-file fallback, not a policy).
    for (int k = 0; k < xoa::kMaxStemChannels; ++k)
        p.hwForStemChannel[k] = k < xoa::kMaxHardwareChannels ? k : -1;

    if (auto tree = store.getInputPatchTree(); tree.isValid())
    {
        const auto rows = juce::StringArray::fromTokens (
            tree.getProperty (ids::patchData).toString(), ";", "");
        for (int k = 0; k < p.numStemChannels && k < rows.size(); ++k)
            p.hwForStemChannel[k] = patchRowColumn (rows[k]);
    }

    // Output side, same shape: one row per speaker.
    for (int s = 0; s < xoa::kMaxSpeakers; ++s)
        p.hwForSpeaker[s] = s < xoa::kMaxHardwareChannels ? s : -1;

    if (auto tree = store.getOutputPatchTree(); tree.isValid())
    {
        const auto rows = juce::StringArray::fromTokens (
            tree.getProperty (ids::patchData).toString(), ";", "");
        const int numSpeakers = juce::jmin (store.getNumSpeakers(), xoa::kMaxSpeakers);
        for (int s = 0; s < numSpeakers && s < rows.size(); ++s)
            p.hwForSpeaker[s] = patchRowColumn (rows[s]);
    }

    return p;
}

} // namespace xoa::rt
