#include "XoaFileManager.h"

#include "../XoaConstants.h"
#include "XoaConstraints.h"
#include "XoaParameterDefaults.h"

#ifndef XOA_VERSION_STRING
 #define XOA_VERSION_STRING "0.0.0"
#endif

namespace xoa
{

using spatcore::control::state::XmlPersistence;

namespace
{
    juce::String describeReadError (XmlPersistence::ReadError error, const juce::File& file)
    {
        switch (error)
        {
            case XmlPersistence::ReadError::fileNotFound:         return "File not found: " + file.getFullPathName();
            case XmlPersistence::ReadError::parseFailed:          return "Not parseable XML: " + file.getFullPathName();
            case XmlPersistence::ReadError::treeConversionFailed: return "XML carries no valid state: " + file.getFullPathName();
            case XmlPersistence::ReadError::none:                 break;
        }
        return {};
    }
} // namespace

//==============================================================================

XoaFileManager::XoaFileManager (XoaValueTreeState& stateToManage)
    : state (stateToManage),
      persistence (XmlPersistence::Options {
          "XOA - tenth-order Ambisonics processor",   // headerTitle
          ids::idProp,                                // child-match key for merges
          &constraints::validateLoadedProperty })     // per-property merge gate
{
}

//==============================================================================
// Section plumbing
//==============================================================================

const char* XoaFileManager::sectionFileStem (Section s)
{
    switch (s)
    {
        case Section::config:   return "config";
        case Section::inputs:   return "inputs";
        case Section::speakers: return "speakers";
        case Section::decoder:  return "decoder";
    }
    return "";
}

juce::Identifier XoaFileManager::fileRootType (Section s)
{
    switch (s)
    {
        case Section::config:   return ids::configFileRoot;
        case Section::inputs:   return ids::inputsFileRoot;
        case Section::speakers: return ids::speakersFileRoot;
        case Section::decoder:  return ids::decoderFileRoot;
    }
    return {};
}

juce::Identifier XoaFileManager::sectionNodeType (Section s)
{
    switch (s)
    {
        case Section::config:   return ids::config;
        case Section::inputs:   return ids::inputs;
        case Section::speakers: return ids::speakers;
        case Section::decoder:  return ids::decoder;
    }
    return {};
}

juce::ValueTree XoaFileManager::liveSection (Section s) const
{
    switch (s)
    {
        case Section::config:   return state.getConfigSection();
        case Section::inputs:   return state.getInputsSection();
        case Section::speakers: return state.getSpeakersSection();
        case Section::decoder:  return state.getDecoderSection();
    }
    return {};
}

int XoaFileManager::undoDomainFor (Section s) const
{
    switch (s)
    {
        case Section::config:   return XoaValueTreeState::configDomain;
        case Section::inputs:   return XoaValueTreeState::inputsDomain;
        case Section::speakers: return XoaValueTreeState::speakersDomain;
        case Section::decoder:  return XoaValueTreeState::decoderDomain;
    }
    return XoaValueTreeState::configDomain;
}

juce::File XoaFileManager::fileForSection (Section s) const
{
    return projectFolder.getChildFile (juce::String (sectionFileStem (s)) + ".xml");
}

juce::File XoaFileManager::manifestFile() const
{
    return projectFolder.getChildFile (projectFolder.getFileName() + kManifestExtension);
}

juce::File XoaFileManager::backupFolder() const
{
    return projectFolder.getChildFile ("backups");
}

bool XoaFileManager::fail (const juce::String& message)
{
    lastError = message;
    return false;
}

//==============================================================================
// Project lifecycle
//==============================================================================

void XoaFileManager::setProjectFolder (const juce::File& folder)
{
    projectFolder = folder;
}

bool XoaFileManager::createProject (const juce::File& folder, const juce::String& showName)
{
    if (folder == juce::File())
        return fail ("No project folder given");

    setProjectFolder (folder);
    if (! folder.isDirectory() && ! folder.createDirectory())
        return fail ("Could not create project folder: " + folder.getFullPathName());
    backupFolder().createDirectory();

    if (showName.isNotEmpty())
        state.setParameterWithoutUndo (ids::showName, showName);

    if (! writeManifest())
        return false;
    return saveProject();
}

bool XoaFileManager::loadProject (const juce::File& manifestOrFolder)
{
    juce::File folder = manifestOrFolder;
    if (manifestOrFolder.existsAsFile() && manifestOrFolder.hasFileExtension (kManifestExtension))
        folder = manifestOrFolder.getParentDirectory();

    if (! folder.isDirectory())
        return fail ("Not a project folder: " + folder.getFullPathName());

    setProjectFolder (folder);

    bool allOk = true;
    for (auto s : { Section::config, Section::inputs, Section::speakers, Section::decoder })
    {
        const auto file = fileForSection (s);
        if (file.existsAsFile())
            allOk = loadSectionFrom (s, file) && allOk;
    }
    return allOk;
}

bool XoaFileManager::saveProject()
{
    if (! hasValidProjectFolder())
        return fail ("No valid project folder");

    if (! manifestFile().existsAsFile() && ! writeManifest())
        return false;

    bool allOk = true;
    for (auto s : { Section::config, Section::inputs, Section::speakers, Section::decoder })
        allOk = saveSection (s) && allOk;
    return allOk;
}

bool XoaFileManager::writeManifest()
{
    juce::XmlElement manifest (ids::projectManifest.toString());
    manifest.setAttribute ("projectName", projectFolder.getFileName());
    manifest.setAttribute ("appVersion", XOA_VERSION_STRING);
    manifest.setAttribute ("schemaVersion", defaults::kSchemaVersion);
    manifest.setAttribute ("createdDate", juce::Time::getCurrentTime().toISO8601 (true));

    if (! manifest.writeTo (manifestFile()))
        return fail ("Could not write manifest: " + manifestFile().getFullPathName());
    return true;
}

//==============================================================================
// Section save/load
//==============================================================================

bool XoaFileManager::saveSection (Section s)
{
    if (! hasValidProjectFolder())
        return fail ("No valid project folder");
    return saveSectionTo (s, fileForSection (s), true);
}

bool XoaFileManager::saveSectionTo (Section s, const juce::File& file, bool withBackup)
{
    if (withBackup && file.existsAsFile())
        XmlPersistence::createBackup (file, backupFolder());

    juce::ValueTree fileRoot (fileRootType (s));
    fileRoot.setProperty (ids::schemaVersion, defaults::kSchemaVersion, nullptr);

    auto sectionCopy = liveSection (s).createCopy();
    sanitizeForSave (sectionCopy);
    fileRoot.appendChild (sectionCopy, nullptr);

    if (persistence.writeTreeToFile (fileRoot, file) != XmlPersistence::WriteResult::ok)
        return fail ("Could not write " + file.getFullPathName());

    if (withBackup)
        XmlPersistence::cleanupBackups (backupFolder(),
                                        { "config", "inputs", "speakers", "decoder" },
                                        kBackupKeepCount);
    return true;
}

bool XoaFileManager::loadSection (Section s)
{
    if (! hasValidProjectFolder())
        return fail ("No valid project folder");
    return loadSectionFrom (s, fileForSection (s));
}

bool XoaFileManager::loadSectionBackup (Section s, int index)
{
    const auto backups = XmlPersistence::listBackups (backupFolder(), sectionFileStem (s));
    if (index < 0 || index >= backups.size())
        return fail ("No backup #" + juce::String (index) + " for " + sectionFileStem (s));
    return loadSectionFrom (s, backups[index]);
}

bool XoaFileManager::loadSectionFrom (Section s, const juce::File& file)
{
    const auto result = persistence.readTreeFromFile (file);
    if (result.error != XmlPersistence::ReadError::none)
        return fail (describeReadError (result.error, file));

    if (result.tree.getType() != fileRootType (s))
        return fail ("Wrong section root '" + result.tree.getType().toString()
                     + "' in " + file.getFullPathName());

    const int fileVersion = static_cast<int> (result.tree.getProperty (ids::schemaVersion, 1));
    if (fileVersion > defaults::kSchemaVersion)
        return fail ("File schema version " + juce::String (fileVersion)
                     + " is newer than this build (" + juce::String (defaults::kSchemaVersion)
                     + "): " + file.getFullPathName());

    auto fileSection = result.tree.getChildWithName (sectionNodeType (s));
    if (! fileSection.isValid())
        return fail ("No <" + sectionNodeType (s).toString() + "> section in "
                     + file.getFullPathName());

    // Count pre-pass: grow the live channel list to at least the file's child
    // count BEFORE merging, so every file child (matched by type+id) lands on
    // a full-default-schema target rather than being appended as a raw, sparse
    // copy that misses newer defaults. Size from the ACTUAL child count, not
    // the (possibly absent or lying) count attribute — a file that omits the
    // count but lists N children must still get N full-default targets.
    const auto channelType = sectionNodeType (s) == ids::inputs ? ids::input : ids::speaker;
    if (s == Section::inputs || s == Section::speakers)
    {
        int fileChildren = 0;
        for (int i = 0; i < fileSection.getNumChildren(); ++i)
            if (fileSection.getChild (i).getType() == channelType)
                ++fileChildren;

        const int declared = static_cast<int> (fileSection.getProperty (
            s == Section::inputs ? ids::inputCount : ids::speakerCount, 0));
        const int target = juce::jmax (declared, fileChildren);
        if (s == Section::inputs)
            state.setNumInputs (target);
        else
            state.setNumSpeakers (target);
    }

    auto live = liveSection (s);
    const auto domain = undoDomainFor (s);
    XoaValueTreeState::ScopedDomain scopedDomain (
        state, static_cast<XoaValueTreeState::UndoDomain> (domain));
    state.beginUndoTransaction ("Load " + juce::String (sectionFileStem (s)));
    persistence.mergeTreeRecursive (live, fileSection, state.getUndoManagerForDomain (domain));

    // Post-merge reconciliation: a foreign file may carry non-contiguous ids
    // or more children than the ceiling, which the merge appends verbatim.
    // Restore the section invariants (child count <= kMax, id == ordinal+1,
    // count property == child count) so channel addressing stays consistent.
    if (s == Section::inputs)
        state.reconcileChannelSection (true, state.getUndoManagerForDomain (domain));
    else if (s == Section::speakers)
        state.reconcileChannelSection (false, state.getUndoManagerForDomain (domain));

    return true;
}

void XoaFileManager::sanitizeForSave (juce::ValueTree& sectionCopy)
{
    // Transient runtime flags never persist: solo is a monitoring gesture,
    // not a project setting.
    if (sectionCopy.getType() != ids::speakers)
        return;

    for (int i = 0; i < sectionCopy.getNumChildren(); ++i)
    {
        auto channelNode = sectionCopy.getChild (i).getChildWithName (ids::channel);
        if (channelNode.isValid() && channelNode.hasProperty (ids::speakerSolo))
            channelNode.setProperty (ids::speakerSolo, false, nullptr);
    }
}

//==============================================================================
// WFS-DIY speaker-layout import (FR-16, WP2 slice)
//==============================================================================

XoaFileManager::WfsImportResult XoaFileManager::importWfsSpeakerLayout (const juce::File& wfsOutputsXml)
{
    // WFS-DIY outputs.xml attribute names, verbatim (case-sensitive).
    static const juce::Identifier wfsOutputConfig   { "OutputConfig" };
    static const juce::Identifier wfsOutputs        { "Outputs" };
    static const juce::Identifier wfsOutput         { "Output" };
    static const juce::Identifier wfsChannel        { "Channel" };
    static const juce::Identifier wfsPosition       { "Position" };
    static const juce::Identifier wfsName           { "outputName" };
    static const juce::Identifier wfsAttenuation    { "outputAttenuation" };
    static const juce::Identifier wfsDelayLatency   { "outputDelayLatency" };
    static const juce::Identifier wfsPositionX      { "outputPositionX" };
    static const juce::Identifier wfsPositionY      { "outputPositionY" };
    static const juce::Identifier wfsPositionZ      { "outputPositionZ" };
    static const juce::Identifier wfsCoordinateMode { "outputCoordinateMode" };

    WfsImportResult result;

    const auto read = persistence.readTreeFromFile (wfsOutputsXml);
    if (read.error != XmlPersistence::ReadError::none)
    {
        result.error = describeReadError (read.error, wfsOutputsXml);
        return result;
    }

    if (read.tree.getType() != wfsOutputConfig)
    {
        result.error = "Not a WFS-DIY outputs file (root '" + read.tree.getType().toString()
                       + "', expected 'OutputConfig')";
        return result;
    }

    const auto outputs = read.tree.getChildWithName (wfsOutputs);
    if (! outputs.isValid())
    {
        result.error = "No <Outputs> element in " + wfsOutputsXml.getFullPathName();
        return result;
    }

    juce::Array<juce::ValueTree> outputNodes;
    for (int i = 0; i < outputs.getNumChildren(); ++i)
        if (outputs.getChild (i).getType() == wfsOutput)
            outputNodes.add (outputs.getChild (i));

    if (outputNodes.isEmpty())
    {
        result.error = "No <Output> entries in " + wfsOutputsXml.getFullPathName();
        return result;
    }

    int importCount = outputNodes.size();
    if (importCount > kMaxSpeakers)
    {
        result.warnings.add ("File has " + juce::String (importCount) + " outputs; importing the first "
                             + juce::String (kMaxSpeakers) + " (kMaxSpeakers)");
        importCount = kMaxSpeakers;
    }

    // setNumSpeakers runs its own "Set speaker count" transaction; the value
    // writes below form a second one. Undoing an import therefore takes two
    // steps (values, then count) — acceptable for WP2.
    state.setNumSpeakers (importCount);

    XoaValueTreeState::ScopedDomain domain (state, XoaValueTreeState::speakersDomain);
    state.beginUndoTransaction ("Import WFS-DIY speaker layout");

    for (int i = 0; i < importCount; ++i)
    {
        const auto output = outputNodes[i];
        const auto channelNode = output.getChildWithName (wfsChannel);
        const auto positionNode = output.getChildWithName (wfsPosition);

        if (channelNode.isValid())
        {
            if (channelNode.hasProperty (wfsName))
                state.setParameter (ids::speakerName, channelNode.getProperty (wfsName), i);
            if (channelNode.hasProperty (wfsAttenuation))
                state.setParameter (ids::speakerGain, channelNode.getProperty (wfsAttenuation), i);
            if (channelNode.hasProperty (wfsDelayLatency))
                state.setParameter (ids::speakerDelay, channelNode.getProperty (wfsDelayLatency), i);
        }

        if (positionNode.isValid())
        {
            // WP5 TO-DO (G8): WFS-DIY's cartesian axis semantics vs XOA's
            // X = front / Y = left / Z = up frame are not yet verified — the
            // verbatim copy below may need an axis remap once the WFS stage
            // frame is pinned down against real rigs. WP2's contract is
            // "parses + geometry lands"; orientation correctness is WP5's
            // import-semantics job.
            if (positionNode.hasProperty (wfsPositionX))
                state.setParameter (ids::speakerPositionX, positionNode.getProperty (wfsPositionX), i);
            if (positionNode.hasProperty (wfsPositionY))
                state.setParameter (ids::speakerPositionY, positionNode.getProperty (wfsPositionY), i);
            if (positionNode.hasProperty (wfsPositionZ))
                state.setParameter (ids::speakerPositionZ, positionNode.getProperty (wfsPositionZ), i);

            const int coordinateMode = static_cast<int> (positionNode.getProperty (wfsCoordinateMode, 0));
            if (coordinateMode != 0)
                result.warnings.add ("Output " + output.getProperty (ids::idProp).toString()
                                     + ": outputCoordinateMode=" + juce::String (coordinateMode)
                                     + " (non-cartesian) — position imported verbatim; polar semantics"
                                       " are interpreted at WP5");
        }

        // WFS DSP knobs (outputOrientation, outputAngleOn/Off, outputPitch,
        // outputHFdamping), <Options>, and <EQ> are deliberately ignored in
        // the WP2 slice.
    }

    result.ok = true;
    result.speakersImported = importCount;
    return result;
}

} // namespace xoa
