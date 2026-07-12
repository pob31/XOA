#include "XoaValueTreeState.h"

#include "../XoaConstants.h"
#include "Helpers/XoaCoordinates.h"
#include "XoaConstraints.h"
#include "XoaParameterDefaults.h"

namespace xoa
{

namespace d = xoa::defaults;

//==============================================================================
// Construction. Ordering that matters:
//  - initializeDefaultState() reassigns `state`, so state.addListener(this)
//    must come AFTER it or the listener is orphaned on the old tree.
//  - The write interceptor only fires for writes routed through the base's
//    writeProperty (setParameter etc.); the defaults are built with raw
//    ValueTree::setProperty and are NOT clamped by it — they are trusted to
//    be in range (XoaParameterDefaults.h). Registering it before the schema
//    build is harmless, not load-bearing.
//==============================================================================

XoaValueTreeState::XoaValueTreeState()
    : TreeParameterStore (numUndoDomains, { "Config", "Inputs", "Speakers", "Decoder" })
{
    setWriteInterceptor ([] (const juce::Identifier& property, const juce::var& proposed,
                             const juce::ValueTree&) -> juce::var
    {
        return constraints::clampToBounds (property, proposed);
    });

    initializeDefaultState();
    state.addListener (this);
}

//==============================================================================
// Default schema
//==============================================================================

void XoaValueTreeState::initializeDefaultState()
{
    state = juce::ValueTree (ids::root);
    state.setProperty (ids::schemaVersion, d::kSchemaVersion, nullptr);

    juce::ValueTree config (ids::config);
    config.setProperty (ids::showName, d::showNameDefault, nullptr);
    config.setProperty (ids::masterGain, d::masterGainDefault, nullptr);
    config.setProperty (ids::oscEnabled, d::oscEnabledDefault, nullptr);
    config.setProperty (ids::oscReceivePort, static_cast<int> (d::oscReceivePortDefault), nullptr);
    config.setProperty (ids::oscSendPort, static_cast<int> (d::oscSendPortDefault), nullptr);
    config.setProperty (ids::oscSendAddress, d::oscSendAddressDefault, nullptr);
    config.setProperty (ids::audioDeviceState, juce::String(), nullptr);
    config.setProperty (ids::rotationYaw, d::rotationYawDefault, nullptr);
    config.setProperty (ids::rotationPitch, d::rotationPitchDefault, nullptr);
    config.setProperty (ids::rotationRoll, d::rotationRollDefault, nullptr);
    config.setProperty (ids::playbackFilePath, d::playbackFilePathDefault, nullptr);
    config.setProperty (ids::playbackLoop, d::playbackLoopDefault, nullptr);
    config.setProperty (ids::playbackContentOrder, static_cast<int> (d::playbackContentOrderDefault), nullptr);
    config.setProperty (ids::playbackConvention, static_cast<int> (d::playbackConventionDefault), nullptr);
    config.setProperty (ids::distanceCompMode, static_cast<int> (d::distanceCompModeDefault), nullptr);
    config.setProperty (ids::monoInputsEnabled, d::monoInputsEnabledDefault, nullptr);
    state.appendChild (config, nullptr);

    juce::ValueTree inputs (ids::inputs);
    inputs.setProperty (ids::inputCount, kDefaultInputs, nullptr);
    for (int i = 0; i < kDefaultInputs; ++i)
        inputs.appendChild (createDefaultInput (i), nullptr);
    state.appendChild (inputs, nullptr);

    juce::ValueTree speakers (ids::speakers);
    speakers.setProperty (ids::speakerCount, kDefaultSpeakers, nullptr);
    for (int i = 0; i < kDefaultSpeakers; ++i)
        speakers.appendChild (createDefaultSpeaker (i), nullptr);
    state.appendChild (speakers, nullptr);

    juce::ValueTree decoder (ids::decoder);
    decoder.setProperty (ids::decoderType, static_cast<int> (d::decoderTypeDefault), nullptr);
    decoder.setProperty (ids::decoderWeighting, static_cast<int> (d::decoderWeightingDefault), nullptr);
    decoder.setProperty (ids::decoderDualBandEnabled, d::decoderDualBandEnabledDefault, nullptr);
    decoder.setProperty (ids::decoderCrossoverFrequency, d::decoderCrossoverFrequencyDefault, nullptr);
    decoder.setProperty (ids::decoderNormalization, static_cast<int> (d::decoderNormalizationDefault), nullptr);
    state.appendChild (decoder, nullptr);

    state.appendChild (juce::ValueTree (ids::monitoring), nullptr);
}

juce::ValueTree XoaValueTreeState::createDefaultInput (int index) const
{
    juce::ValueTree input (ids::input);
    input.setProperty (ids::idProp, index + 1, nullptr);

    juce::ValueTree channel (ids::channel);
    channel.setProperty (ids::inputName, d::getDefaultInputName (index), nullptr);
    channel.setProperty (ids::inputGain, d::inputGainDefault, nullptr);
    channel.setProperty (ids::inputMute, d::inputMuteDefault, nullptr);
    input.appendChild (channel, nullptr);

    juce::ValueTree position (ids::position);
    position.setProperty (ids::inputPositionX, d::inputPositionXDefault, nullptr);
    position.setProperty (ids::inputPositionY, d::inputPositionYDefault, nullptr);
    position.setProperty (ids::inputPositionZ, d::inputPositionZDefault, nullptr);
    position.setProperty (ids::inputCoordinateMode, static_cast<int> (d::coordinateModeDefault), nullptr);
    position.setProperty (ids::inputMaxSpeed, d::inputMaxSpeedDefault, nullptr);
    position.setProperty (ids::inputTrackingSmooth, d::inputTrackingSmoothDefault, nullptr);
    input.appendChild (position, nullptr);

    juce::ValueTree encoder (ids::encoder);
    encoder.setProperty (ids::inputSpread, d::inputSpreadDefault, nullptr);
    encoder.setProperty (ids::inputNfcEnabled, d::inputNfcEnabledDefault, nullptr);
    input.appendChild (encoder, nullptr);

    return input;
}

juce::ValueTree XoaValueTreeState::createDefaultSpeaker (int index) const
{
    juce::ValueTree speaker (ids::speaker);
    speaker.setProperty (ids::idProp, index + 1, nullptr);

    juce::ValueTree channel (ids::channel);
    channel.setProperty (ids::speakerName, d::getDefaultSpeakerName (index), nullptr);
    channel.setProperty (ids::speakerGain, d::speakerGainDefault, nullptr);
    channel.setProperty (ids::speakerDelay, d::speakerDelayDefault, nullptr);
    channel.setProperty (ids::speakerMute, d::speakerMuteDefault, nullptr);
    channel.setProperty (ids::speakerSolo, d::speakerSoloDefault, nullptr);
    speaker.appendChild (channel, nullptr);

    // Fresh projects place speakers on the M1 ring (radius kDefaultRigRadius,
    // z = 0), spaced at the default-count pitch — computed through
    // XoaCoordinates so the converter is dogfooded from day one. Speakers
    // grown beyond the default count wrap around the same ring; positioning
    // them is the user's (or an importer's) job anyway.
    const double azimuthDeg = index * (360.0 / kDefaultSpeakers);
    const auto pos = coords::sphericalToCartesian (
        { d::kDefaultRigRadius, coords::normalizeAzimuthDegrees (azimuthDeg), 0.0 });

    juce::ValueTree position (ids::position);
    position.setProperty (ids::speakerPositionX, pos.x, nullptr);
    position.setProperty (ids::speakerPositionY, pos.y, nullptr);
    position.setProperty (ids::speakerPositionZ, pos.z, nullptr);
    position.setProperty (ids::speakerCoordinateMode, static_cast<int> (d::coordinateModeDefault), nullptr);
    speaker.appendChild (position, nullptr);

    juce::ValueTree eq (ids::eq);
    eq.setProperty (ids::speakerEqEnabled, d::speakerEqEnabledDefault, nullptr);
    for (int b = 0; b < kNumEqBands; ++b)
    {
        juce::ValueTree band (ids::band);
        band.setProperty (ids::idProp, b + 1, nullptr);
        band.setProperty (ids::eqShape, static_cast<int> (d::eqShapeDefault), nullptr);
        band.setProperty (ids::eqFrequency, d::kEqBandDefaultHz[b], nullptr);
        band.setProperty (ids::eqGain, d::eqGainDefault, nullptr);
        band.setProperty (ids::eqQ, d::eqQDefault, nullptr);
        band.setProperty (ids::eqSlope, d::eqSlopeDefault, nullptr);
        eq.appendChild (band, nullptr);
    }
    speaker.appendChild (eq, nullptr);

    return speaker;
}

//==============================================================================
// Scope routing
//==============================================================================

XoaValueTreeState::Scope XoaValueTreeState::getParameterScope (const juce::Identifier& id)
{
    if (id == ids::inputCount || id == ids::speakerCount)
        return Scope::structural;

    const juce::String name = id.toString();
    if (name.startsWith ("input"))
        return Scope::input;
    if (name.startsWith ("speaker") || name.startsWith ("eq"))
        return Scope::speaker;
    if (name.startsWith ("decoder"))
        return Scope::decoder;
    return Scope::config;
}

juce::ValueTree XoaValueTreeState::getTreeForParameter (const juce::Identifier& id,
                                                        int channelIndex) const
{
    switch (getParameterScope (id))
    {
        case Scope::structural:
            return id == ids::inputCount ? getInputsSection() : getSpeakersSection();

        case Scope::config:
            return getConfigSection();

        case Scope::decoder:
            return getDecoderSection();

        case Scope::input:
        case Scope::speaker:
        {
            const auto channelTree = getParameterScope (id) == Scope::input
                                         ? getInputTree (channelIndex)
                                         : getSpeakerTree (channelIndex);
            if (! channelTree.isValid())
                return {};
            // Probe the subsection children for the one carrying the
            // property. Per-band eq* properties live one level deeper (Band
            // children) and correctly fall through to an invalid tree here —
            // use the EQ band helpers for those.
            for (int i = 0; i < channelTree.getNumChildren(); ++i)
            {
                auto child = channelTree.getChild (i);
                if (child.hasProperty (id))
                    return child;
            }
            return {};
        }
    }
    return {};
}

int XoaValueTreeState::resolveChannelIndex (const juce::ValueTree& changedNode) const
{
    for (auto node = changedNode; node.isValid(); node = node.getParent())
        if (node.getType() == ids::input || node.getType() == ids::speaker)
            return static_cast<int> (node.getProperty (ids::idProp, 0)) - 1;
    return -1;
}

void XoaValueTreeState::handlePostWrite (juce::ValueTree&, const juce::Identifier&,
                                         const juce::var&, int)
{
    // Deliberately empty (see header). Cross-parameter invariants register
    // here when the schema grows some.
}

//==============================================================================
// Structural mutators
//==============================================================================

void XoaValueTreeState::setParameter (const juce::Identifier& id, const juce::var& value,
                                      int channelIndex)
{
    if (id == ids::inputCount)  { setNumInputs (static_cast<int> (value)); return; }
    if (id == ids::speakerCount) { setNumSpeakers (static_cast<int> (value)); return; }
    TreeParameterStore::setParameter (id, value, channelIndex);
}

void XoaValueTreeState::setParameterWithoutUndo (const juce::Identifier& id,
                                                 const juce::var& value, int channelIndex)
{
    if (id == ids::inputCount)
    {
        applyChannelCount (getInputsSection(), ids::inputCount,
                           juce::jlimit (1, kMaxInputs, static_cast<int> (value)),
                           nullptr, true);
        return;
    }
    if (id == ids::speakerCount)
    {
        applyChannelCount (getSpeakersSection(), ids::speakerCount,
                           juce::jlimit (1, kMaxSpeakers, static_cast<int> (value)),
                           nullptr, false);
        return;
    }
    TreeParameterStore::setParameterWithoutUndo (id, value, channelIndex);
}

void XoaValueTreeState::setNumInputs (int count)
{
    count = juce::jlimit (1, kMaxInputs, count);
    ScopedDomain domain (*this, inputsDomain);
    beginUndoTransaction ("Set input count");
    applyChannelCount (getInputsSection(), ids::inputCount, count, getActiveUndoManager(), true);
}

void XoaValueTreeState::setNumSpeakers (int count)
{
    count = juce::jlimit (1, kMaxSpeakers, count);
    ScopedDomain domain (*this, speakersDomain);
    beginUndoTransaction ("Set speaker count");
    applyChannelCount (getSpeakersSection(), ids::speakerCount, count, getActiveUndoManager(), false);
}

void XoaValueTreeState::applyChannelCount (juce::ValueTree section, const juce::Identifier& countId,
                                           int targetCount, juce::UndoManager* undoManager,
                                           bool isInputs)
{
    if (! section.isValid())
        return;

    // Shrink from the tail / grow with full-default children — both through
    // the UndoManager so undo restores removed channels WITH their edited
    // values. The count property is written last, through the choke point.
    for (int current = section.getNumChildren(); current > targetCount; --current)
        section.removeChild (current - 1, undoManager);

    for (int current = section.getNumChildren(); current < targetCount; ++current)
        section.appendChild (isInputs ? createDefaultInput (current)
                                      : createDefaultSpeaker (current),
                             undoManager);

    writeProperty (section, countId, targetCount, undoManager);
}

int XoaValueTreeState::getNumInputs() const
{
    return getInputsSection().getNumChildren();
}

int XoaValueTreeState::getNumSpeakers() const
{
    return getSpeakersSection().getNumChildren();
}

void XoaValueTreeState::reconcileChannelSection (bool isInputs, juce::UndoManager* undoManager)
{
    auto section = isInputs ? getInputsSection() : getSpeakersSection();
    if (! section.isValid())
        return;

    const int maxChannels = isInputs ? kMaxInputs : kMaxSpeakers;
    for (int n = section.getNumChildren(); n > maxChannels; --n)
        section.removeChild (n - 1, undoManager);

    for (int i = 0; i < section.getNumChildren(); ++i)
    {
        auto child = section.getChild (i);
        if (static_cast<int> (child.getProperty (ids::idProp, -1)) != i + 1)
            writeProperty (child, ids::idProp, i + 1, undoManager);
    }

    writeProperty (section, isInputs ? ids::inputCount : ids::speakerCount,
                   section.getNumChildren(), undoManager);
}

//==============================================================================
// EQ band access
//==============================================================================

juce::ValueTree XoaValueTreeState::getSpeakerEqBand (int speakerIndex, int bandIndex) const
{
    const auto eqNode = getSpeakerTree (speakerIndex).getChildWithName (ids::eq);
    auto bandNode = eqNode.getChild (bandIndex);
    return bandNode.getType() == ids::band ? bandNode : juce::ValueTree();
}

juce::var XoaValueTreeState::getEqBandParameter (int speakerIndex, int bandIndex,
                                                 const juce::Identifier& id) const
{
    const auto bandNode = getSpeakerEqBand (speakerIndex, bandIndex);
    return bandNode.isValid() ? bandNode.getProperty (id) : juce::var();
}

void XoaValueTreeState::setEqBandParameter (int speakerIndex, int bandIndex,
                                            const juce::Identifier& id, const juce::var& value)
{
    auto bandNode = getSpeakerEqBand (speakerIndex, bandIndex);
    if (! bandNode.isValid())
        return;
    writeProperty (bandNode, id, value, getActiveUndoManager());
}

//==============================================================================
// Section access
//==============================================================================

juce::ValueTree XoaValueTreeState::getConfigSection() const     { return state.getChildWithName (ids::config); }
juce::ValueTree XoaValueTreeState::getInputsSection() const     { return state.getChildWithName (ids::inputs); }
juce::ValueTree XoaValueTreeState::getSpeakersSection() const   { return state.getChildWithName (ids::speakers); }
juce::ValueTree XoaValueTreeState::getDecoderSection() const    { return state.getChildWithName (ids::decoder); }
juce::ValueTree XoaValueTreeState::getMonitoringSection() const { return state.getChildWithName (ids::monitoring); }

juce::ValueTree XoaValueTreeState::getInputTree (int channelIndex) const
{
    const auto section = getInputsSection();
    return channelIndex >= 0 && channelIndex < section.getNumChildren()
               ? section.getChild (channelIndex)
               : juce::ValueTree();
}

juce::ValueTree XoaValueTreeState::getSpeakerTree (int channelIndex) const
{
    const auto section = getSpeakersSection();
    return channelIndex >= 0 && channelIndex < section.getNumChildren()
               ? section.getChild (channelIndex)
               : juce::ValueTree();
}

} // namespace xoa
