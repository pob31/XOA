/*
    XoaLocalizationTests.cpp - WP10 C3.

    Validates the localization scaffold without a GUI:
      - en.json and fr.json parse as objects;
      - every key present in fr.json also exists (as a string leaf) in en.json,
        i.e. fr's keys are a subset of the English superset (the overlay base);
      - required structural keys exist in en.json;
      - LocalizationManager's en-fallback overlay works: a fr-only value wins, a
        key fr omits resolves to the English value, and the available-language
        scan finds both files.

    The language files are read from the source tree via XOA_LANG_SOURCE_DIR (the
    Resources/ dir that contains lang/), so the test needs no staged bundle.
*/

#include <juce_core/juce_core.h>

#include "XoaTestFramework.h"
#include "Localization/LocalizationManager.h"

namespace
{
juce::File resourcesDir()
{
    return juce::File (XOA_LANG_SOURCE_DIR);
}

juce::File langFile (const juce::String& locale)
{
    return resourcesDir().getChildFile ("lang").getChildFile (locale + ".json");
}

// Recursively assert that every property in `sub` exists in `super`, and that
// wherever `sub` holds a string leaf, `super` holds a string leaf at the same
// path. Records a CHECK failure (with the dotted path) for any violation.
void assertSubsetOf (const juce::var& sub, const juce::var& super, const juce::String& path)
{
    auto* subObj = sub.getDynamicObject();
    if (subObj == nullptr)
        return;

    auto* superObj = super.getDynamicObject();
    if (superObj == nullptr)
    {
        std::fprintf (stderr, "FAIL localization: '%s' is an object in fr but not in en\n",
                      path.toRawUTF8());
        ++failures;
        return;
    }

    for (const auto& prop : subObj->getProperties())
    {
        const juce::String childPath = path.isEmpty() ? prop.name.toString()
                                                      : path + "." + prop.name.toString();
        if (! superObj->hasProperty (prop.name))
        {
            std::fprintf (stderr, "FAIL localization: fr key '%s' missing from en.json\n",
                          childPath.toRawUTF8());
            ++failures;
            continue;
        }

        const juce::var& subChild = prop.value;
        const juce::var superChild = superObj->getProperty (prop.name);

        if (subChild.getDynamicObject() != nullptr)
            assertSubsetOf (subChild, superChild, childPath);
        else
            CHECK (subChild.isString() && superChild.isString());
    }
}
} // namespace

void runXoaLocalizationTests()
{
    const juce::var en = juce::JSON::parse (langFile ("en"));
    const juce::var fr = juce::JSON::parse (langFile ("fr"));

    CHECK (en.isObject());
    CHECK (fr.isObject());

    // fr's keys are a subset of en's (en is the fallback superset).
    assertSubsetOf (fr, en, {});

    // Required structural keys the app depends on.
    for (const char* key : { "statusBar.helpMode", "statusBar.oscMode",
                             "tabs.systemConfig", "tabs.network", "tabs.inputs",
                             "tabs.speakersDecoder", "tabs.monitoring", "tabs.map",
                             "common.ok", "common.cancel" })
    {
        auto& loc = LocalizationManager::getInstance();
        loc.setResourceDirectory (resourcesDir());
        CHECK (loc.loadLanguage ("en"));
        CHECK (loc.hasKey (key));
    }

    auto& loc = LocalizationManager::getInstance();
    loc.setResourceDirectory (resourcesDir());

    // Overlay: load fr; a fr-translated prose value wins, an untranslated label
    // falls back to English.
    CHECK (loc.loadLanguage ("fr", LocalizationManager::TranslationTier::Minimal));
    CHECK (loc.getCurrentLocale() == "fr");
    CHECK (loc.get ("common.cancel") == juce::String::fromUTF8 ("Annuler")); // fr wins
    CHECK (loc.get ("statusBar.helpMode") == juce::String::fromUTF8 ("Aide")); // fr wins
    CHECK (loc.get ("tabs.inputs") == "Inputs");   // fr omits tabs -> en fallback

    // Switching back to English.
    CHECK (loc.loadLanguage ("en"));
    CHECK (loc.get ("common.cancel") == "Cancel");

    // Available-language scan finds both loose files (and not the raw key path).
    const juce::StringArray langs = loc.getAvailableLanguages();
    CHECK (langs.contains ("en"));
    CHECK (langs.contains ("fr"));

    loc.shutdown();
}
