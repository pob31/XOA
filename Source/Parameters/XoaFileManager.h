#pragma once

#include "spatcore/control/state/XmlPersistence.h"

#include "XoaValueTreeState.h"

//==============================================================================
// XOA — project file I/O: the app-shaped layer over spatcore's XmlPersistence.
//
// A project is a folder:
//   <Folder>/<Folder>.xoa      manifest (plain XML, no comment header)
//   <Folder>/config.xml        <XOAConfig schemaVersion>   <- Config section
//   <Folder>/inputs.xml        <XOAInputs schemaVersion>   <- Inputs section
//   <Folder>/speakers.xml      <XOASpeakers schemaVersion> <- Speakers section
//   <Folder>/decoder.xml       <XOADecoder schemaVersion>  <- Decoder section
//   <Folder>/backups/          per-save rolling backups, keep-last-N
//
// save/load are headless (folder-relative, no dialogs) — the WFS-DIY pattern
// that makes the control-replay/test paths trivial. Loading merges into the
// live tree ("missing = keep" backfill; the injected validator rejects
// unparseable/out-of-range values), preceded by a channel-count pre-pass so
// file channels beyond the current count land on full-default targets.
//==============================================================================

namespace xoa
{

class XoaFileManager
{
public:
    explicit XoaFileManager (XoaValueTreeState& stateToManage);

    enum class Section { config, inputs, speakers, decoder };

    static constexpr int kBackupKeepCount = 10;
    static constexpr const char* kManifestExtension = ".xoa";

    //==========================================================================
    // Project folder lifecycle
    //==========================================================================

    void setProjectFolder (const juce::File& folder);
    juce::File getProjectFolder() const                { return projectFolder; }
    bool hasValidProjectFolder() const                 { return projectFolder.isDirectory(); }

    /** Create the folder structure + manifest, apply the show name, save all
        sections. */
    bool createProject (const juce::File& folder, const juce::String& showName);

    /** Accepts the .xoa manifest file or the project folder itself. Loads
        every section file present. */
    bool loadProject (const juce::File& manifestOrFolder);

    /** Save all sections (writes the manifest if missing). */
    bool saveProject();

    //==========================================================================
    // Per-section quintets (headless; export/import take explicit files)
    //==========================================================================

    bool saveConfig()                          { return saveSection (Section::config); }
    bool loadConfig()                          { return loadSection (Section::config); }
    bool loadConfigBackup (int index)          { return loadSectionBackup (Section::config, index); }
    bool exportConfig (const juce::File& f)    { return saveSectionTo (Section::config, f, false); }
    bool importConfig (const juce::File& f)    { return loadSectionFrom (Section::config, f); }

    bool saveInputs()                          { return saveSection (Section::inputs); }
    bool loadInputs()                          { return loadSection (Section::inputs); }
    bool loadInputsBackup (int index)          { return loadSectionBackup (Section::inputs, index); }
    bool exportInputs (const juce::File& f)    { return saveSectionTo (Section::inputs, f, false); }
    bool importInputs (const juce::File& f)    { return loadSectionFrom (Section::inputs, f); }

    bool saveSpeakers()                        { return saveSection (Section::speakers); }
    bool loadSpeakers()                        { return loadSection (Section::speakers); }
    bool loadSpeakersBackup (int index)        { return loadSectionBackup (Section::speakers, index); }
    bool exportSpeakers (const juce::File& f)  { return saveSectionTo (Section::speakers, f, false); }
    bool importSpeakers (const juce::File& f)  { return loadSectionFrom (Section::speakers, f); }

    bool saveDecoder()                         { return saveSection (Section::decoder); }
    bool loadDecoder()                         { return loadSection (Section::decoder); }
    bool loadDecoderBackup (int index)         { return loadSectionBackup (Section::decoder, index); }
    bool exportDecoder (const juce::File& f)   { return saveSectionTo (Section::decoder, f, false); }
    bool importDecoder (const juce::File& f)   { return loadSectionFrom (Section::decoder, f); }

    //==========================================================================
    // WFS-DIY speaker-layout import (FR-16)
    //==========================================================================

    /** Maps the WFS-DIY stage frame (+X map-right, +Y upstage, +Z up) onto
        XOA's listener frame (+X front, +Y left, +Z up).
        rotate90 (default): the listener faces upstage, so front = +Y_wfs, left =
        -X_wfs -> (x,y,z)_xoa = (y, -x, z)_wfs (a proper -90deg rotation, det +1).
        verbatim: identity (XOA +X then points at WFS map-right). */
    enum class WfsAxisRemap { rotate90, verbatim };

    struct WfsImportResult
    {
        bool ok = false;
        int speakersImported = 0;
        juce::StringArray warnings;
        juce::String error;
    };

    /** Import speaker geometry (name, gain, delay, remapped position, display
        coordinate mode) and per-band EQ (name-for-name) from a WFS-DIY
        outputs.xml. WFS DSP knobs and <Options> are ignored. */
    WfsImportResult importWfsSpeakerLayout (const juce::File& wfsOutputsXml,
                                            WfsAxisRemap remap = WfsAxisRemap::rotate90);

    //==========================================================================
    // Paths and diagnostics
    //==========================================================================

    juce::File fileForSection (Section) const;
    juce::File manifestFile() const;
    juce::File backupFolder() const;
    juce::String getLastError() const                  { return lastError; }

private:
    bool saveSection (Section);
    bool saveSectionTo (Section, const juce::File&, bool withBackup);
    bool loadSection (Section);
    bool loadSectionBackup (Section, int index);
    bool loadSectionFrom (Section, const juce::File&);
    bool writeManifest();

    juce::ValueTree liveSection (Section) const;
    int undoDomainFor (Section) const;
    static const char* sectionFileStem (Section);
    static juce::Identifier fileRootType (Section);
    static juce::Identifier sectionNodeType (Section);
    static void sanitizeForSave (juce::ValueTree& sectionCopy);
    bool fail (const juce::String& message);

    XoaValueTreeState& state;
    spatcore::control::state::XmlPersistence persistence;
    juce::File projectFolder;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XoaFileManager)
};

} // namespace xoa
