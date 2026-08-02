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
    config.setProperty (ids::oscTcpEnabled, d::oscTcpEnabledDefault, nullptr);
    config.setProperty (ids::oscTcpPort, static_cast<int> (d::oscTcpPortDefault), nullptr);
    config.setProperty (ids::oscAcceptAnyHost, d::oscAcceptAnyHostDefault, nullptr);
    config.setProperty (ids::oscFeedbackEnabled, d::oscFeedbackEnabledDefault, nullptr);
    config.setProperty (ids::oscMeterEnabled, d::oscMeterEnabledDefault, nullptr);
    config.setProperty (ids::audioDeviceState, juce::String(), nullptr);
    config.setProperty (ids::rotationYaw, d::rotationYawDefault, nullptr);
    config.setProperty (ids::rotationPitch, d::rotationPitchDefault, nullptr);
    config.setProperty (ids::rotationRoll, d::rotationRollDefault, nullptr);
    config.setProperty (ids::distanceCompMode, static_cast<int> (d::distanceCompModeDefault), nullptr);
    config.setProperty (ids::listenerX, d::listenerXDefault, nullptr);
    config.setProperty (ids::listenerY, d::listenerYDefault, nullptr);
    config.setProperty (ids::listenerZ, d::listenerZDefault, nullptr);
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

    // Audio patch (stage 2): identity-diagonal defaults so a fresh project
    // behaves exactly like the pre-patch identity mapping.
    juce::ValueTree audioPatch (ids::audioPatch);
    audioPatch.appendChild (createDefaultPatchTree (true, kDefaultInputs), nullptr);
    audioPatch.appendChild (createDefaultPatchTree (false, kDefaultSpeakers), nullptr);
    state.appendChild (audioPatch, nullptr);

    lastReconciledSpans.assign ((size_t) kDefaultInputs, 1);
}

juce::ValueTree XoaValueTreeState::createDefaultInput (int index) const
{
    juce::ValueTree input (ids::input);
    input.setProperty (ids::idProp, index + 1, nullptr);

    juce::ValueTree channel (ids::channel);
    channel.setProperty (ids::inputName, d::getDefaultInputName (index), nullptr);
    channel.setProperty (ids::inputGain, d::inputGainDefault, nullptr);
    channel.setProperty (ids::inputMute, d::inputMuteDefault, nullptr);
    channel.setProperty (ids::inputFormat, static_cast<int> (d::inputFormatDefault), nullptr);
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

void XoaValueTreeState::handlePostWrite (juce::ValueTree& node, const juce::Identifier& property,
                                         const juce::var& value, int channelIndex)
{
    // Stage-2 invariant: a format change re-spans the stem channels, so the
    // total ceiling must be re-enforced and the input patch rows remapped.
    // reconcileAudioPatch self-guards against its own writes re-entering.
    if (property == ids::inputFormat)
        reconcileAudioPatch();

    if (postWriteObserver)
        postWriteObserver (node, property, value, channelIndex);
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

    // A count change re-spans the patch rows (and, for inputs, may push the
    // stem-channel total over its ceiling). Patch maintenance is deliberately
    // outside undo, so an undo of the count change relies on the next
    // reconcile rather than replay.
    reconcileAudioPatch();
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

void XoaValueTreeState::setEqBandParameterWithoutUndo (int speakerIndex, int bandIndex,
                                                       const juce::Identifier& id, const juce::var& value)
{
    auto bandNode = getSpeakerEqBand (speakerIndex, bandIndex);
    if (! bandNode.isValid())
        return;
    writeProperty (bandNode, id, value, nullptr);
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

//==============================================================================
// Stem formats and spans (D44/D45)
//==============================================================================

int XoaValueTreeState::channelCountForFormat (int format) noexcept
{
    const int order = juce::jlimit (0, kAmbisonicOrder, format);
    return order <= 0 ? 1 : (order + 1) * (order + 1);
}

int XoaValueTreeState::getInputFormat (int inputIndex) const
{
    return juce::jlimit (0, kAmbisonicOrder, getIntParameter (ids::inputFormat, inputIndex));
}

int XoaValueTreeState::getInputChannelCount (int inputIndex) const
{
    return channelCountForFormat (getInputFormat (inputIndex));
}

int XoaValueTreeState::getStemChannelOffset (int inputIndex) const
{
    int offset = 0;
    const int n = juce::jmin (inputIndex, getNumInputs());
    for (int i = 0; i < n; ++i)
        offset += getInputChannelCount (i);
    return offset;
}

int XoaValueTreeState::getTotalStemChannels() const
{
    return getStemChannelOffset (getNumInputs());
}

std::vector<int> XoaValueTreeState::currentInputSpans() const
{
    std::vector<int> spans ((size_t) getNumInputs());
    for (size_t i = 0; i < spans.size(); ++i)
        spans[i] = getInputChannelCount ((int) i);
    return spans;
}

void XoaValueTreeState::clampStemSpans()
{
    auto spans = currentInputSpans();
    int total = 0;
    for (const int s : spans)
        total += s;
    if (total <= kMaxStemChannels)
        return;

    // Over the ceiling: step formats down, last HOA input first, one order at
    // a time, until the sum fits. Mono-only always fits (kMaxInputs mono
    // channels < kMaxStemChannels), so this terminates.
    for (int i = (int) spans.size() - 1; i >= 0 && total > kMaxStemChannels; --i)
    {
        int format = getInputFormat (i);
        while (format > 0 && total > kMaxStemChannels)
        {
            --format;
            const int newSpan = channelCountForFormat (format);
            total -= spans[(size_t) i] - newSpan;
            spans[(size_t) i] = newSpan;
        }

        if (format != getInputFormat (i))
            if (auto tree = getTreeForParameter (ids::inputFormat, i); tree.isValid())
                writeProperty (tree, ids::inputFormat, format, nullptr);
    }
}

//==============================================================================
// Audio patch (D41-D43)
//==============================================================================

juce::ValueTree XoaValueTreeState::getAudioPatchSection() const { return state.getChildWithName (ids::audioPatch); }
juce::ValueTree XoaValueTreeState::getInputPatchTree() const    { return getAudioPatchSection().getChildWithName (ids::inputPatch); }
juce::ValueTree XoaValueTreeState::getOutputPatchTree() const   { return getAudioPatchSection().getChildWithName (ids::outputPatch); }

namespace
{
    // A patchData row of `cols` zeros ("0,0,...") — the matrix's own row
    // format; empty-string rows are avoided so token alignment can never
    // depend on how a tokenizer treats empties.
    juce::String zeroPatchRow (int cols)
    {
        juce::StringArray cells;
        cells.ensureStorageAllocated (cols);
        for (int c = 0; c < cols; ++c)
            cells.add ("0");
        return cells.joinIntoString (",");
    }

    juce::String patchRowWithBit (int cols, int bit)
    {
        juce::StringArray cells;
        cells.ensureStorageAllocated (cols);
        for (int c = 0; c < cols; ++c)
            cells.add (c == bit ? "1" : "0");
        return cells.joinIntoString (",");
    }

    /** Hardware column of the row's '1' (1:1 patches), or -1 when unpatched. */
    int patchedColumnOfRow (const juce::String& row)
    {
        const auto cells = juce::StringArray::fromTokens (row, ",", "");
        for (int c = 0; c < cells.size(); ++c)
            if (cells[c].getIntValue() == 1)
                return c;
        return -1;
    }

    /** Highest hardware column carrying a '1' anywhere in patchData, or -1. */
    int highestPatchedColumn (const juce::String& patchData)
    {
        int highest = -1;
        const auto rows = juce::StringArray::fromTokens (patchData, ";", "");
        for (const auto& row : rows)
        {
            const auto cells = juce::StringArray::fromTokens (row, ",", "");
            for (int c = cells.size(); --c > highest;)
                if (cells[c].getIntValue() == 1)
                    { highest = c; break; }
        }
        return highest;
    }

    /** Fill placeholder rows with the identity-diagonal continuation: a new
        row at flattened index k gets hardware column k when free — so a
        project that never opens the patch window keeps behaving identity-
        mapped as channels grow. Occupied or out-of-range diagonals stay
        unpatched. `rowsArr` placeholders are empty strings on entry. */
    void fillNewRowsWithIdentity (juce::StringArray& rowsArr, int cols)
    {
        std::vector<bool> used ((size_t) xoa::kMaxHardwareChannels, false);
        for (const auto& row : rowsArr)
            if (const int c = patchedColumnOfRow (row); c >= 0 && c < xoa::kMaxHardwareChannels)
                used[(size_t) c] = true;

        for (int r = 0; r < rowsArr.size(); ++r)
        {
            if (rowsArr[r].isNotEmpty())
                continue;
            if (r < xoa::kMaxHardwareChannels && ! used[(size_t) r])
            {
                used[(size_t) r] = true;
                rowsArr.set (r, patchRowWithBit (juce::jmax (cols, r + 1), r));
            }
            else
            {
                rowsArr.set (r, zeroPatchRow (cols));
            }
        }
    }
} // namespace

juce::String XoaValueTreeState::buildIdentityPatchData (int numRows)
{
    const int cols = juce::jlimit (1, kMaxHardwareChannels, juce::jmax (64, numRows));
    juce::StringArray rowStrings;
    for (int r = 0; r < numRows; ++r)
        rowStrings.add (patchRowWithBit (cols, r));
    return rowStrings.joinIntoString (";");
}

juce::ValueTree XoaValueTreeState::createDefaultPatchTree (bool isInput, int numRows) const
{
    juce::ValueTree tree (isInput ? ids::inputPatch : ids::outputPatch);
    tree.setProperty (ids::rows, numRows, nullptr);
    tree.setProperty (ids::cols, juce::jlimit (1, kMaxHardwareChannels, juce::jmax (64, numRows)), nullptr);
    tree.setProperty (isInput ? ids::activeHardwareInputs : ids::activeHardwareOutputs, 0, nullptr);
    tree.setProperty (ids::patchData, buildIdentityPatchData (numRows), nullptr);
    return tree;
}

void XoaValueTreeState::recomputePatchCols()
{
    auto applyPolicy = [] (juce::ValueTree tree, const juce::Identifier& activeId)
    {
        if (! tree.isValid())
            return;
        const int active  = (int) tree.getProperty (activeId, 0);
        const int highest = highestPatchedColumn (tree.getProperty (ids::patchData).toString());
        const int cols    = juce::jlimit (1, kMaxHardwareChannels,
                                          juce::jmax (64, juce::jmax (active, highest + 1)));
        if ((int) tree.getProperty (ids::cols, 0) != cols)
            tree.setProperty (ids::cols, cols, nullptr);
    };

    applyPolicy (getInputPatchTree(),  ids::activeHardwareInputs);
    applyPolicy (getOutputPatchTree(), ids::activeHardwareOutputs);
}

void XoaValueTreeState::updateHardwareChannelCount (int activeInputs, int activeOutputs)
{
    if (auto tree = getInputPatchTree(); tree.isValid())
        if ((int) tree.getProperty (ids::activeHardwareInputs, -1) != activeInputs)
            tree.setProperty (ids::activeHardwareInputs, activeInputs, nullptr);

    if (auto tree = getOutputPatchTree(); tree.isValid())
        if ((int) tree.getProperty (ids::activeHardwareOutputs, -1) != activeOutputs)
            tree.setProperty (ids::activeHardwareOutputs, activeOutputs, nullptr);

    recomputePatchCols();
}

void XoaValueTreeState::reconcileAudioPatch()
{
    if (reconcilingPatch)
        return;
    const juce::ScopedValueSetter<bool> guard (reconcilingPatch, true);

    // Formats first: the row remap below must see post-clamp spans.
    clampStemSpans();

    auto section = getAudioPatchSection();
    const auto spans = currentInputSpans();

    if (! section.isValid())
    {
        lastReconciledSpans = spans;
        return;
    }

    // INPUT side: remap the flattened rows by per-input block so a format
    // change keeps every other input's patches, and the changed input keeps
    // the rows the old and new spans share.
    if (auto tree = getInputPatchTree(); tree.isValid())
    {
        const int cols = juce::jmax (1, (int) tree.getProperty (ids::cols, 64));
        const auto oldRows = juce::StringArray::fromTokens (tree.getProperty (ids::patchData).toString(), ";", "");

        // The span cache says which old rows belonged to which input. It is
        // trustworthy only when it accounts for exactly the rows present;
        // otherwise (first run, foreign file) fall back to one-row-per-input
        // prefix preservation.
        auto oldSpans = lastReconciledSpans;
        int cachedTotal = 0;
        for (const int s : oldSpans)
            cachedTotal += s;
        if (cachedTotal != oldRows.size())
            oldSpans.assign ((size_t) oldRows.size(), 1);

        // Kept rows are copied per input block; rows an input gained are left
        // as empty placeholders for the identity fill below.
        juce::StringArray newRows;
        int oldStart = 0;
        for (size_t i = 0; i < spans.size(); ++i)
        {
            const int oldSpan = i < oldSpans.size() ? oldSpans[i] : 0;
            const int keep = juce::jmin (oldSpan, spans[i]);
            for (int r = 0; r < spans[i]; ++r)
                newRows.add (r < keep && oldStart + r < oldRows.size() ? oldRows[oldStart + r]
                                                                       : juce::String());
            oldStart += oldSpan;
        }
        fillNewRowsWithIdentity (newRows, cols);

        const juce::String newData = newRows.joinIntoString (";");
        if ((int) tree.getProperty (ids::rows, -1) != newRows.size())
            tree.setProperty (ids::rows, newRows.size(), nullptr);
        if (tree.getProperty (ids::patchData).toString() != newData)
            tree.setProperty (ids::patchData, newData, nullptr);
    }

    // OUTPUT side: one row per speaker — truncate, pad with the identity
    // continuation (a grown speaker keeps sounding on its ordinal output
    // when that column is free, matching pre-patch behaviour).
    if (auto tree = getOutputPatchTree(); tree.isValid())
    {
        const int numSpeakers = getNumSpeakers();
        const int cols = juce::jmax (1, (int) tree.getProperty (ids::cols, 64));
        auto rowsArr = juce::StringArray::fromTokens (tree.getProperty (ids::patchData).toString(), ";", "");

        while (rowsArr.size() > numSpeakers)
            rowsArr.remove (rowsArr.size() - 1);
        while (rowsArr.size() < numSpeakers)
            rowsArr.add (juce::String());
        fillNewRowsWithIdentity (rowsArr, cols);

        const juce::String newData = rowsArr.joinIntoString (";");
        if ((int) tree.getProperty (ids::rows, -1) != numSpeakers)
            tree.setProperty (ids::rows, numSpeakers, nullptr);
        if (tree.getProperty (ids::patchData).toString() != newData)
            tree.setProperty (ids::patchData, newData, nullptr);
    }

    recomputePatchCols();
    lastReconciledSpans = spans;
}

} // namespace xoa
