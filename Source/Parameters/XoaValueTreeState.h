#pragma once

#include "spatcore/control/state/TreeParameterStore.h"

#include "XoaParameterIDs.h"

//==============================================================================
// XOA — the parameter store: schema subclass of spatcore's TreeParameterStore.
//
// Tree shape (all defaults built by the constructor):
//
//   XOA {schemaVersion}
//   ├── Config    {showName, masterGain, osc*, audioDeviceState}
//   ├── Inputs    {inputCount}  -> Input{id=k+1}  x N {Channel, Position, Encoder}
//   ├── Speakers  {speakerCount}-> Speaker{id=k+1} x M {Channel, Position, EQ->Band x6}
//   ├── Decoder   {decoderType, decoderWeighting, decoderDualBandEnabled, ...}
//   └── Monitoring {}           (reserved, empty)
//
// Addressing: (Identifier, channelIndex). Scope is routed by name prefix
// ("input*" / "speaker*" / "eq*" / "decoder*" / else Config); the two count
// ids are structural and divert to setNumInputs/setNumSpeakers. Per-band EQ
// properties need two indices and therefore live OUTSIDE this address space —
// use the get/setEqBandParameter helpers.
//==============================================================================

namespace xoa
{

class XoaValueTreeState : public spatcore::control::state::TreeParameterStore
{
public:
    /** One undo history per editing surface. Monitoring has no parameters in
        v1 and deliberately gets no domain. */
    enum UndoDomain : int
    {
        configDomain = 0,
        inputsDomain,
        speakersDomain,
        decoderDomain,
        numUndoDomains
    };

    XoaValueTreeState();

    //==========================================================================
    // Structural mutators — the only paths that add/remove channel children.
    //==========================================================================

    void setNumInputs (int count);      // clamped [1, kMaxInputs]; undoable (Inputs domain)
    void setNumSpeakers (int count);    // clamped [1, kMaxSpeakers]; undoable (Speakers domain)
    int getNumInputs() const;
    int getNumSpeakers() const;

    /** Restore the channel-section invariants after a file merge that may have
        appended foreign children: clamp the child list to the [1, kMax]
        ceiling (trimming from the tail) and renumber every channel's id to
        ordinal+1 so id == ordinal+1 holds for resolveChannelIndex, then
        restamp the count property. The caller supplies the section's undo
        manager (the load runs inside one transaction). No-op for a
        well-formed section. */
    void reconcileChannelSection (bool isInputs, juce::UndoManager* undoManager);

    /** Count ids divert to the structural mutators; everything else falls
        through to the base implementations. */
    void setParameter (const juce::Identifier& id, const juce::var& value,
                       int channelIndex = -1) override;
    void setParameterWithoutUndo (const juce::Identifier& id, const juce::var& value,
                                  int channelIndex = -1) override;

    //==========================================================================
    // EQ band access (two indices — outside the (id, channelIndex) seam)
    //==========================================================================

    juce::ValueTree getSpeakerEqBand (int speakerIndex, int bandIndex) const;   // both 0-based
    juce::var getEqBandParameter (int speakerIndex, int bandIndex,
                                  const juce::Identifier& id) const;
    void setEqBandParameter (int speakerIndex, int bandIndex,
                             const juce::Identifier& id, const juce::var& value);
    /** As setEqBandParameter but bypasses undo - the write path for continuous
        external streams (OSC) that must not flood the per-domain undo history. */
    void setEqBandParameterWithoutUndo (int speakerIndex, int bandIndex,
                                        const juce::Identifier& id, const juce::var& value);

    //==========================================================================
    // Section access (XoaFileManager and engines)
    //==========================================================================

    juce::ValueTree getConfigSection() const;
    juce::ValueTree getInputsSection() const;
    juce::ValueTree getSpeakersSection() const;
    juce::ValueTree getDecoderSection() const;
    juce::ValueTree getMonitoringSection() const;
    juce::ValueTree getInputTree (int channelIndex) const;
    juce::ValueTree getSpeakerTree (int channelIndex) const;

    /** Typed shadow of the base RAII domain switch. */
    struct ScopedDomain : ScopedUndoDomain
    {
        ScopedDomain (XoaValueTreeState& s, UndoDomain d) : ScopedUndoDomain (s, d) {}
    };

protected:
    juce::ValueTree getTreeForParameter (const juce::Identifier& id,
                                         int channelIndex) const override;
    int resolveChannelIndex (const juce::ValueTree& changedNode) const override;

    /** Post-write invariant seam — deliberately empty in v1. The schema has
        no cross-parameter invariants yet; channel counts are handled on the
        setParameter route plus the file manager's load count pre-pass, which
        keeps structural writes out of undo-replay (reentrancy hazard). */
    void handlePostWrite (juce::ValueTree& changedNode, const juce::Identifier& property,
                          const juce::var& value, int channelIndex) override;

private:
    enum class Scope { config, input, speaker, decoder, structural };
    static Scope getParameterScope (const juce::Identifier& id);

    void initializeDefaultState();
    juce::ValueTree createDefaultInput (int index) const;
    juce::ValueTree createDefaultSpeaker (int index) const;
    void applyChannelCount (juce::ValueTree section, const juce::Identifier& countId,
                            int targetCount, juce::UndoManager* undoManager, bool isInputs);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaValueTreeState)
};

} // namespace xoa
