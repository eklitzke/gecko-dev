/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Logging.h"
#include "mozilla/intl/LocaleService.h"
#include "mozilla/intl/MozLocale.h"
#include "mozilla/intl/OSPreferences.h"

#include "gfxPlatformFontList.h"
#include "gfxTextRun.h"
#include "gfxUserFontSet.h"

#include "nsCRT.h"
#include "nsGkAtoms.h"
#include "nsServiceManagerUtils.h"
#include "nsUnicharUtils.h"
#include "nsUnicodeRange.h"
#include "nsUnicodeProperties.h"
#include "nsXULAppAPI.h"

#include "mozilla/Attributes.h"
#include "mozilla/Likely.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Mutex.h"
#include "mozilla/Preferences.h"
#include "mozilla/Telemetry.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/gfx/2D.h"

#include <locale.h>

using namespace mozilla;
using mozilla::intl::LocaleService;
using mozilla::intl::Locale;
using mozilla::intl::OSPreferences;

#define LOG_FONTLIST(args) MOZ_LOG(gfxPlatform::GetLog(eGfxLog_fontlist), \
                               LogLevel::Debug, args)
#define LOG_FONTLIST_ENABLED() MOZ_LOG_TEST( \
                                   gfxPlatform::GetLog(eGfxLog_fontlist), \
                                   LogLevel::Debug)
#define LOG_FONTINIT(args) MOZ_LOG(gfxPlatform::GetLog(eGfxLog_fontinit), \
                               LogLevel::Debug, args)
#define LOG_FONTINIT_ENABLED() MOZ_LOG_TEST( \
                                   gfxPlatform::GetLog(eGfxLog_fontinit), \
                                   LogLevel::Debug)

gfxPlatformFontList *gfxPlatformFontList::sPlatformFontList = nullptr;

// Character ranges that require complex-script shaping support in the font,
// and so should be masked out by ReadCMAP if the necessary layout tables
// are not present.
// Currently used by the Mac and FT2 implementations only, but probably should
// be supported on Windows as well.
const gfxFontEntry::ScriptRange gfxPlatformFontList::sComplexScriptRanges[] = {
    // Actually, now that harfbuzz supports presentation-forms shaping for
    // Arabic, we can render it without layout tables. So maybe we don't
    // want to mask the basic Arabic block here?
    // This affects the arabic-fallback-*.html reftests, which rely on
    // loading a font that *doesn't* have any GSUB table.
    { 0x0600, 0x06FF, { TRUETYPE_TAG('a','r','a','b'), 0, 0 } },
    { 0x0700, 0x074F, { TRUETYPE_TAG('s','y','r','c'), 0, 0 } },
    { 0x0750, 0x077F, { TRUETYPE_TAG('a','r','a','b'), 0, 0 } },
    { 0x08A0, 0x08FF, { TRUETYPE_TAG('a','r','a','b'), 0, 0 } },
    { 0x0900, 0x097F, { TRUETYPE_TAG('d','e','v','2'),
                        TRUETYPE_TAG('d','e','v','a'), 0 } },
    { 0x0980, 0x09FF, { TRUETYPE_TAG('b','n','g','2'),
                        TRUETYPE_TAG('b','e','n','g'), 0 } },
    { 0x0A00, 0x0A7F, { TRUETYPE_TAG('g','u','r','2'),
                        TRUETYPE_TAG('g','u','r','u'), 0 } },
    { 0x0A80, 0x0AFF, { TRUETYPE_TAG('g','j','r','2'),
                        TRUETYPE_TAG('g','u','j','r'), 0 } },
    { 0x0B00, 0x0B7F, { TRUETYPE_TAG('o','r','y','2'),
                        TRUETYPE_TAG('o','r','y','a'), 0 } },
    { 0x0B80, 0x0BFF, { TRUETYPE_TAG('t','m','l','2'),
                        TRUETYPE_TAG('t','a','m','l'), 0 } },
    { 0x0C00, 0x0C7F, { TRUETYPE_TAG('t','e','l','2'),
                        TRUETYPE_TAG('t','e','l','u'), 0 } },
    { 0x0C80, 0x0CFF, { TRUETYPE_TAG('k','n','d','2'),
                        TRUETYPE_TAG('k','n','d','a'), 0 } },
    { 0x0D00, 0x0D7F, { TRUETYPE_TAG('m','l','m','2'),
                        TRUETYPE_TAG('m','l','y','m'), 0 } },
    { 0x0D80, 0x0DFF, { TRUETYPE_TAG('s','i','n','h'), 0, 0 } },
    { 0x0E80, 0x0EFF, { TRUETYPE_TAG('l','a','o',' '), 0, 0 } },
    { 0x0F00, 0x0FFF, { TRUETYPE_TAG('t','i','b','t'), 0, 0 } },
    { 0x1000, 0x109f, { TRUETYPE_TAG('m','y','m','r'),
                        TRUETYPE_TAG('m','y','m','2'), 0 } },
    { 0x1780, 0x17ff, { TRUETYPE_TAG('k','h','m','r'), 0, 0 } },
    // Khmer Symbols (19e0..19ff) don't seem to need any special shaping
    { 0xaa60, 0xaa7f, { TRUETYPE_TAG('m','y','m','r'),
                        TRUETYPE_TAG('m','y','m','2'), 0 } },
    // Thai seems to be "renderable" without AAT morphing tables
    { 0, 0, { 0, 0, 0 } } // terminator
};

// prefs for the font info loader
#define FONT_LOADER_DELAY_PREF              "gfx.font_loader.delay"
#define FONT_LOADER_INTERVAL_PREF           "gfx.font_loader.interval"

static const char* kObservedPrefs[] = {
    "font.",
    "font.name-list.",
    "intl.accept_languages",  // hmmmm...
    nullptr
};

static const char kFontSystemWhitelistPref[] = "font.system.whitelist";

// xxx - this can probably be eliminated by reworking pref font handling code
static const char *gPrefLangNames[] = {
    #define FONT_PREF_LANG(enum_id_, str_, atom_id_) str_
    #include "gfxFontPrefLangList.h"
    #undef FONT_PREF_LANG
};

static_assert(MOZ_ARRAY_LENGTH(gPrefLangNames) == uint32_t(eFontPrefLang_Count),
              "size of pref lang name array doesn't match pref lang enum size");

class gfxFontListPrefObserver final : public nsIObserver {
    ~gfxFontListPrefObserver() {}
public:
    NS_DECL_ISUPPORTS
    NS_DECL_NSIOBSERVER
};

static gfxFontListPrefObserver* gFontListPrefObserver = nullptr;

NS_IMPL_ISUPPORTS(gfxFontListPrefObserver, nsIObserver)

#define LOCALES_CHANGED_TOPIC "intl:system-locales-changed"

NS_IMETHODIMP
gfxFontListPrefObserver::Observe(nsISupports     *aSubject,
                                 const char      *aTopic,
                                 const char16_t *aData)
{
    NS_ASSERTION(!strcmp(aTopic, NS_PREFBRANCH_PREFCHANGE_TOPIC_ID) ||
                 !strcmp(aTopic, LOCALES_CHANGED_TOPIC), "invalid topic");
    // XXX this could be made to only clear out the cache for the prefs that were changed
    // but it probably isn't that big a deal.
    gfxPlatformFontList::PlatformFontList()->ClearLangGroupPrefFonts();
    gfxFontCache::GetCache()->AgeAllGenerations();
    if (XRE_IsParentProcess() && !strcmp(aTopic, LOCALES_CHANGED_TOPIC)) {
        gfxPlatform::ForceGlobalReflow();
    }
    return NS_OK;
}

MOZ_DEFINE_MALLOC_SIZE_OF(FontListMallocSizeOf)

NS_IMPL_ISUPPORTS(gfxPlatformFontList::MemoryReporter, nsIMemoryReporter)

NS_IMETHODIMP
gfxPlatformFontList::MemoryReporter::CollectReports(
    nsIHandleReportCallback* aHandleReport, nsISupports* aData, bool aAnonymize)
{
    FontListSizes sizes;
    sizes.mFontListSize = 0;
    sizes.mFontTableCacheSize = 0;
    sizes.mCharMapsSize = 0;
    sizes.mLoaderSize = 0;

    gfxPlatformFontList::PlatformFontList()->AddSizeOfIncludingThis(&FontListMallocSizeOf,
                                                                    &sizes);

    MOZ_COLLECT_REPORT(
        "explicit/gfx/font-list", KIND_HEAP, UNITS_BYTES,
        sizes.mFontListSize,
        "Memory used to manage the list of font families and faces.");

    MOZ_COLLECT_REPORT(
        "explicit/gfx/font-charmaps", KIND_HEAP, UNITS_BYTES,
        sizes.mCharMapsSize,
        "Memory used to record the character coverage of individual fonts.");

    if (sizes.mFontTableCacheSize) {
        MOZ_COLLECT_REPORT(
            "explicit/gfx/font-tables", KIND_HEAP, UNITS_BYTES,
            sizes.mFontTableCacheSize,
            "Memory used for cached font metrics and layout tables.");
    }

    if (sizes.mLoaderSize) {
        MOZ_COLLECT_REPORT(
            "explicit/gfx/font-loader", KIND_HEAP, UNITS_BYTES,
            sizes.mLoaderSize,
            "Memory used for (platform-specific) font loader.");
    }

    return NS_OK;
}

gfxPlatformFontList::gfxPlatformFontList(bool aNeedFullnamePostscriptNames)
    : mFontFamiliesMutex("gfxPlatformFontList::mFontFamiliesMutex"), mFontFamilies(64),
      mOtherFamilyNames(16), mBadUnderlineFamilyNames(8), mSharedCmaps(8),
      mStartIndex(0), mNumFamilies(0), mFontlistInitCount(0),
      mFontFamilyWhitelistActive(false)
{
    mOtherFamilyNamesInitialized = false;

    if (aNeedFullnamePostscriptNames) {
        mExtraNames = MakeUnique<ExtraNames>();
    }
    mFaceNameListsInitialized = false;

    mLangService = nsLanguageAtomService::GetService();

    LoadBadUnderlineList();

    // pref changes notification setup
    NS_ASSERTION(!gFontListPrefObserver,
                 "There has been font list pref observer already");
    gFontListPrefObserver = new gfxFontListPrefObserver();
    NS_ADDREF(gFontListPrefObserver);
    Preferences::AddStrongObservers(gFontListPrefObserver, kObservedPrefs);

    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (obs) {
        obs->AddObserver(gFontListPrefObserver, LOCALES_CHANGED_TOPIC, false);
    }

    // Only the parent process listens for whitelist changes; it will then
    // notify its children to rebuild their font lists.
    if (XRE_IsParentProcess()) {
        Preferences::RegisterCallback(FontWhitelistPrefChanged,
                                      kFontSystemWhitelistPref);
    }

    RegisterStrongMemoryReporter(new MemoryReporter());
}

gfxPlatformFontList::~gfxPlatformFontList()
{
    mSharedCmaps.Clear();
    ClearLangGroupPrefFonts();
    NS_ASSERTION(gFontListPrefObserver, "There is no font list pref observer");
    Preferences::RemoveObservers(gFontListPrefObserver, kObservedPrefs);

    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (obs) {
        obs->RemoveObserver(gFontListPrefObserver, LOCALES_CHANGED_TOPIC);
    }

    if (XRE_IsParentProcess()) {
        Preferences::UnregisterCallback(FontWhitelistPrefChanged,
                                        kFontSystemWhitelistPref);
    }
    NS_RELEASE(gFontListPrefObserver);
}

/* static */
void
gfxPlatformFontList::FontWhitelistPrefChanged(const char *aPref,
                                              void *aClosure)
{
    MOZ_ASSERT(XRE_IsParentProcess());
    gfxPlatformFontList::PlatformFontList()->UpdateFontList();
    mozilla::dom::ContentParent::NotifyUpdatedFonts();
}

// number of CSS generic font families
const uint32_t kNumGenerics = 5;

void
gfxPlatformFontList::ApplyWhitelist()
{
    nsTArray<nsString> list;
    gfxFontUtils::GetPrefsFontList(kFontSystemWhitelistPref, list);
    uint32_t numFonts = list.Length();
    mFontFamilyWhitelistActive = (numFonts > 0);
    if (!mFontFamilyWhitelistActive) {
        return;
    }
    nsTHashtable<nsStringHashKey> familyNamesWhitelist;
    for (uint32_t i = 0; i < numFonts; i++) {
        nsString key;
        ToLowerCase(list[i], key);
        familyNamesWhitelist.PutEntry(key);
    }
    for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
        // Don't continue if we only have one font left.
        if (mFontFamilies.Count() == 1) {
            break;
        }
        nsString fontFamilyName(iter.Key());
        ToLowerCase(fontFamilyName);
        if (!familyNamesWhitelist.Contains(fontFamilyName)) {
            iter.Remove();
        }
    }
}

bool
gfxPlatformFontList::AddWithLegacyFamilyName(const nsAString& aLegacyName,
                                             gfxFontEntry* aFontEntry)
{
    bool added = false;
    nsAutoString key;
    ToLowerCase(aLegacyName, key);
    gfxFontFamily* family = mOtherFamilyNames.GetWeak(key);
    if (!family) {
        family = CreateFontFamily(aLegacyName);
        family->SetHasStyles(true); // we don't want the family to search for
                                    // faces, we're adding them directly here
        mOtherFamilyNames.Put(key, family);
        added = true;
    }
    family->AddFontEntry(aFontEntry->Clone());
    return added;
}

nsresult
gfxPlatformFontList::InitFontList()
{
    mFontlistInitCount++;

    if (LOG_FONTINIT_ENABLED()) {
        LOG_FONTINIT(("(fontinit) system fontlist initialization\n"));
    }

    // rebuilding fontlist so clear out font/word caches
    gfxFontCache *fontCache = gfxFontCache::GetCache();
    if (fontCache) {
        fontCache->AgeAllGenerations();
        fontCache->FlushShapedWordCaches();
    }

    gfxPlatform::PurgeSkiaFontCache();

    CancelInitOtherFamilyNamesTask();
    MutexAutoLock lock(mFontFamiliesMutex);
    mFontFamilies.Clear();
    mOtherFamilyNames.Clear();
    mOtherFamilyNamesInitialized = false;

    if (mExtraNames) {
        mExtraNames->mFullnames.Clear();
        mExtraNames->mPostscriptNames.Clear();
    }
    mFaceNameListsInitialized = false;
    ClearLangGroupPrefFonts();
    mReplacementCharFallbackFamily = nullptr;
    CancelLoader();

    // initialize ranges of characters for which system-wide font search should be skipped
    mCodepointsWithNoFonts.reset();
    mCodepointsWithNoFonts.SetRange(0,0x1f);     // C0 controls
    mCodepointsWithNoFonts.SetRange(0x7f,0x9f);  // C1 controls

    sPlatformFontList = this;

    nsresult rv = InitFontListForPlatform();
    if (NS_FAILED(rv)) {
        return rv;
    }

    ApplyWhitelist();
    return NS_OK;
}

void
gfxPlatformFontList::GenerateFontListKey(const nsAString& aKeyName, nsAString& aResult)
{
    aResult = aKeyName;
    ToLowerCase(aResult);
}

#define OTHERNAMES_TIMEOUT 200

void
gfxPlatformFontList::InitOtherFamilyNames(bool aDeferOtherFamilyNamesLoading)
{
    if (mOtherFamilyNamesInitialized) {
        return;
    }

    // If the font loader delay has been set to zero, we don't defer loading
    // additional family names (regardless of the aDefer... parameter), as we
    // take this to mean availability of font info is to be prioritized over
    // potential startup perf or main-thread jank.
    // (This is used so we can reliably run reftests that depend on localized
    // font-family names being available.)
    if (aDeferOtherFamilyNamesLoading &&
        Preferences::GetUint(FONT_LOADER_DELAY_PREF) > 0) {
        if (!mPendingOtherFamilyNameTask) {
            RefPtr<mozilla::CancelableRunnable> task = new InitOtherFamilyNamesRunnable();
            mPendingOtherFamilyNameTask = task;
            NS_IdleDispatchToCurrentThread(task.forget());
        }
    } else {
        InitOtherFamilyNamesInternal(false);
    }
}

// time limit for loading facename lists (ms)
#define NAMELIST_TIMEOUT  200

gfxFontEntry*
gfxPlatformFontList::SearchFamiliesForFaceName(const nsAString& aFaceName)
{
    TimeStamp start = TimeStamp::Now();
    bool timedOut = false;
    // if mFirstChar is not 0, only load facenames for families
    // that start with this character
    char16_t firstChar = 0;
    gfxFontEntry *lookup = nullptr;

    // iterate over familes starting with the same letter
    firstChar = ToLowerCase(aFaceName.CharAt(0));

    for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
        nsStringHashKey::KeyType key = iter.Key();
        RefPtr<gfxFontFamily>& family = iter.Data();

        // when filtering, skip names that don't start with the filter character
        if (firstChar && ToLowerCase(key.CharAt(0)) != firstChar) {
            continue;
        }

        family->ReadFaceNames(this, NeedFullnamePostscriptNames());

        TimeDuration elapsed = TimeStamp::Now() - start;
        if (elapsed.ToMilliseconds() > NAMELIST_TIMEOUT) {
            timedOut = true;
            break;
        }
    }

    lookup = FindFaceName(aFaceName);

    TimeStamp end = TimeStamp::Now();
    Telemetry::AccumulateTimeDelta(Telemetry::FONTLIST_INITFACENAMELISTS,
                                   start, end);
    if (LOG_FONTINIT_ENABLED()) {
        TimeDuration elapsed = end - start;
        LOG_FONTINIT(("(fontinit) SearchFamiliesForFaceName took %8.2f ms %s %s",
                      elapsed.ToMilliseconds(),
                      (lookup ? "found name" : ""),
                      (timedOut ? "timeout" : "")));
    }

    return lookup;
}

gfxFontEntry*
gfxPlatformFontList::FindFaceName(const nsAString& aFaceName)
{
    gfxFontEntry *lookup;

    // lookup in name lookup tables, return null if not found
    if (mExtraNames &&
        ((lookup = mExtraNames->mPostscriptNames.GetWeak(aFaceName)) ||
         (lookup = mExtraNames->mFullnames.GetWeak(aFaceName)))) {
        return lookup;
    }

    return nullptr;
}

gfxFontEntry*
gfxPlatformFontList::LookupInFaceNameLists(const nsAString& aFaceName)
{
    gfxFontEntry *lookup = nullptr;

    // initialize facename lookup tables if needed
    // note: this can terminate early or time out, in which case
    //       mFaceNameListsInitialized remains false
    if (!mFaceNameListsInitialized) {
        lookup = SearchFamiliesForFaceName(aFaceName);
        if (lookup) {
            return lookup;
        }
    }

    // lookup in name lookup tables, return null if not found
    if (!(lookup = FindFaceName(aFaceName))) {
        // names not completely initialized, so keep track of lookup misses
        if (!mFaceNameListsInitialized) {
            if (!mFaceNamesMissed) {
                mFaceNamesMissed = MakeUnique<nsTHashtable<nsStringHashKey>>(2);
            }
            mFaceNamesMissed->PutEntry(aFaceName);
        }
    }

    return lookup;
}

void
gfxPlatformFontList::PreloadNamesList()
{
    AutoTArray<nsString, 10> preloadFonts;
    gfxFontUtils::GetPrefsFontList("font.preload-names-list", preloadFonts);

    uint32_t numFonts = preloadFonts.Length();
    for (uint32_t i = 0; i < numFonts; i++) {
        nsAutoString key;
        GenerateFontListKey(preloadFonts[i], key);

        // only search canonical names!
        gfxFontFamily *familyEntry = mFontFamilies.GetWeak(key);
        if (familyEntry) {
            familyEntry->ReadOtherFamilyNames(this);
        }
    }

}

void
gfxPlatformFontList::LoadBadUnderlineList()
{
    AutoTArray<nsString, 10> blacklist;
    gfxFontUtils::GetPrefsFontList("font.blacklist.underline_offset", blacklist);
    uint32_t numFonts = blacklist.Length();
    for (uint32_t i = 0; i < numFonts; i++) {
        nsAutoString key;
        GenerateFontListKey(blacklist[i], key);
        mBadUnderlineFamilyNames.PutEntry(key);
    }
}

void
gfxPlatformFontList::UpdateFontList()
{
    InitFontList();
    RebuildLocalFonts();
}

void
gfxPlatformFontList::GetFontList(nsAtom *aLangGroup,
                                 const nsACString& aGenericFamily,
                                 nsTArray<nsString>& aListOfFonts)
{
    MutexAutoLock lock(mFontFamiliesMutex);
    for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
        RefPtr<gfxFontFamily>& family = iter.Data();
        if (family->FilterForFontList(aLangGroup, aGenericFamily)) {
            nsAutoString localizedFamilyName;
            family->LocalizedName(localizedFamilyName);
            aListOfFonts.AppendElement(localizedFamilyName);
        }
    }

    aListOfFonts.Sort();
    aListOfFonts.Compact();
}

void
gfxPlatformFontList::GetFontFamilyList(nsTArray<RefPtr<gfxFontFamily> >& aFamilyArray)
{
    for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
        RefPtr<gfxFontFamily>& family = iter.Data();
        aFamilyArray.AppendElement(family);
    }
}

gfxFontEntry*
gfxPlatformFontList::SystemFindFontForChar(uint32_t aCh, uint32_t aNextCh,
                                           Script aRunScript,
                                           const gfxFontStyle* aStyle)
 {
    gfxFontEntry* fontEntry = nullptr;

    // is codepoint with no matching font? return null immediately
    if (mCodepointsWithNoFonts.test(aCh)) {
        return nullptr;
    }

    // Try to short-circuit font fallback for U+FFFD, used to represent
    // encoding errors: just use cached family from last time U+FFFD was seen.
    // This helps speed up pages with lots of encoding errors, binary-as-text,
    // etc.
    if (aCh == 0xFFFD && mReplacementCharFallbackFamily) {
        fontEntry =
            mReplacementCharFallbackFamily->FindFontForStyle(*aStyle);

        // this should never fail, as we must have found U+FFFD in order to set
        // mReplacementCharFallbackFamily at all, but better play it safe
        if (fontEntry && fontEntry->HasCharacter(aCh)) {
            return fontEntry;
        }
    }

    TimeStamp start = TimeStamp::Now();

    // search commonly available fonts
    bool common = true;
    gfxFontFamily *fallbackFamily = nullptr;
    fontEntry = CommonFontFallback(aCh, aNextCh, aRunScript, aStyle,
                                   &fallbackFamily);
 
    // if didn't find a font, do system-wide fallback (except for specials)
    uint32_t cmapCount = 0;
    if (!fontEntry) {
        common = false;
        fontEntry = GlobalFontFallback(aCh, aRunScript, aStyle, cmapCount,
                                       &fallbackFamily);
    }
    TimeDuration elapsed = TimeStamp::Now() - start;

    LogModule* log = gfxPlatform::GetLog(eGfxLog_textrun);

    if (MOZ_UNLIKELY(MOZ_LOG_TEST(log, LogLevel::Warning))) {
        uint32_t unicodeRange = FindCharUnicodeRange(aCh);
        Script script = mozilla::unicode::GetScriptCode(aCh);
        MOZ_LOG(log, LogLevel::Warning,\
               ("(textrun-systemfallback-%s) char: u+%6.6x "
                 "unicode-range: %d script: %d match: [%s]"
                " time: %dus cmaps: %d\n",
                (common ? "common" : "global"), aCh,
                unicodeRange, static_cast<int>(script),
                (fontEntry ? NS_ConvertUTF16toUTF8(fontEntry->Name()).get() :
                    "<none>"),
                int32_t(elapsed.ToMicroseconds()),
                cmapCount));
    }

    // no match? add to set of non-matching codepoints
    if (!fontEntry) {
        mCodepointsWithNoFonts.set(aCh);
    } else if (aCh == 0xFFFD && fontEntry && fallbackFamily) {
        mReplacementCharFallbackFamily = fallbackFamily;
    }
 
    // track system fallback time
    static bool first = true;
    int32_t intElapsed = int32_t(first ? elapsed.ToMilliseconds() :
                                         elapsed.ToMicroseconds());
    Telemetry::Accumulate((first ? Telemetry::SYSTEM_FONT_FALLBACK_FIRST :
                                   Telemetry::SYSTEM_FONT_FALLBACK),
                          intElapsed);
    first = false;

    // track the script for which fallback occurred (incremented one make it
    // 1-based)
    Telemetry::Accumulate(Telemetry::SYSTEM_FONT_FALLBACK_SCRIPT,
                          int(aRunScript) + 1);

    return fontEntry;
}

#define NUM_FALLBACK_FONTS        8

gfxFontEntry*
gfxPlatformFontList::CommonFontFallback(uint32_t aCh, uint32_t aNextCh,
                                        Script aRunScript,
                                        const gfxFontStyle* aMatchStyle,
                                        gfxFontFamily** aMatchedFamily)
{
    AutoTArray<const char*,NUM_FALLBACK_FONTS> defaultFallbacks;
    uint32_t i, numFallbacks;

    gfxPlatform::GetPlatform()->GetCommonFallbackFonts(aCh, aNextCh,
                                                       aRunScript,
                                                       defaultFallbacks);
    numFallbacks = defaultFallbacks.Length();
    for (i = 0; i < numFallbacks; i++) {
        nsAutoString familyName;
        const char *fallbackFamily = defaultFallbacks[i];

        familyName.AppendASCII(fallbackFamily);
        gfxFontFamily *fallback = FindFamilyByCanonicalName(familyName);
        if (!fallback) {
            continue;
        }

        gfxFontEntry *fontEntry;

        // use first font in list that supports a given character
        fontEntry = fallback->FindFontForStyle(*aMatchStyle);
        if (fontEntry) {
            if (fontEntry->HasCharacter(aCh)) {
                *aMatchedFamily = fallback;
                return fontEntry;
            }
            // If we requested a styled font (bold and/or italic), and the char
            // was not available, check other faces of the family.
            if (!fontEntry->IsNormalStyle()) {
                // If style/weight/stretch was not Normal, see if we can
                // fall back to a next-best face (e.g. Arial Black -> Bold,
                // or Arial Narrow -> Regular).
                GlobalFontMatch data(aCh, aMatchStyle);
                fallback->SearchAllFontsForChar(&data);
                if (data.mBestMatch) {
                    *aMatchedFamily = fallback;
                    return data.mBestMatch;
                }
            }
        }
    }

    return nullptr;
}

gfxFontEntry*
gfxPlatformFontList::GlobalFontFallback(const uint32_t aCh,
                                        Script aRunScript,
                                        const gfxFontStyle* aMatchStyle,
                                        uint32_t& aCmapCount,
                                        gfxFontFamily** aMatchedFamily)
{
    bool useCmaps = IsFontFamilyWhitelistActive() ||
                    gfxPlatform::GetPlatform()->UseCmapsDuringSystemFallback();
    if (!useCmaps) {
        // Allow platform-specific fallback code to try and find a usable font
        gfxFontEntry* fe =
            PlatformGlobalFontFallback(aCh, aRunScript, aMatchStyle,
                                       aMatchedFamily);
        if (fe) {
            return fe;
        }
    }

    // otherwise, try to find it among local fonts
    GlobalFontMatch data(aCh, aMatchStyle);

    // iterate over all font families to find a font that support the character
    for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
      RefPtr<gfxFontFamily>& family = iter.Data();
      // evaluate all fonts in this family for a match
      family->FindFontForChar(&data);
    }

    aCmapCount = data.mCmapsTested;
    *aMatchedFamily = data.mMatchedFamily;

    return data.mBestMatch;
}

gfxFontFamily*
gfxPlatformFontList::CheckFamily(gfxFontFamily *aFamily)
{
    if (aFamily && !aFamily->HasStyles()) {
        aFamily->FindStyleVariations();
        aFamily->CheckForSimpleFamily();
    }

    if (aFamily && aFamily->GetFontList().Length() == 0) {
        // failed to load any faces for this family, so discard it
        nsAutoString key;
        GenerateFontListKey(aFamily->Name(), key);
        mFontFamilies.Remove(key);
        return nullptr;
    }

    return aFamily;
}

bool
gfxPlatformFontList::FindAndAddFamilies(const nsAString& aFamily,
                                        nsTArray<gfxFontFamily*>* aOutput,
                                        FindFamiliesFlags aFlags,
                                        gfxFontStyle* aStyle,
                                        gfxFloat aDevToCssSize)
{
    nsAutoString key;
    GenerateFontListKey(aFamily, key);

    NS_ASSERTION(mFontFamilies.Count() != 0, "system font list was not initialized correctly");

    // lookup in canonical (i.e. English) family name list
    gfxFontFamily *familyEntry = mFontFamilies.GetWeak(key);

    // if not found, lookup in other family names list (mostly localized names)
    if (!familyEntry) {
        familyEntry = mOtherFamilyNames.GetWeak(key);
    }

    // if still not found and other family names not yet fully initialized,
    // initialize the rest of the list and try again.  this is done lazily
    // since reading name table entries is expensive.
    // although ASCII localized family names are possible they don't occur
    // in practice so avoid pulling in names at startup
    if (!familyEntry && !mOtherFamilyNamesInitialized && !IsASCII(aFamily)) {
        InitOtherFamilyNames(!(aFlags & FindFamiliesFlags::eForceOtherFamilyNamesLoading));
        familyEntry = mOtherFamilyNames.GetWeak(key);
        if (!familyEntry && !mOtherFamilyNamesInitialized &&
            !(aFlags & FindFamiliesFlags::eNoAddToNamesMissedWhenSearching)) {
            // localized family names load timed out, add name to list of
            // names to check after localized names are loaded
            if (!mOtherNamesMissed) {
                mOtherNamesMissed = MakeUnique<nsTHashtable<nsStringHashKey>>(2);
            }
            mOtherNamesMissed->PutEntry(key);
        }
    }

    familyEntry = CheckFamily(familyEntry);

    // If we failed to find the requested family, check for a space in the
    // name; if found, and if the "base" name (up to the last space) exists
    // as a family, then this might be a legacy GDI-style family name for
    // an additional weight/width. Try searching the faces of the base family
    // and create any corresponding legacy families.
    if (!familyEntry && !(aFlags & FindFamiliesFlags::eNoSearchForLegacyFamilyNames)) {
        // We don't have nsAString::RFindChar, so look for a space manually
        const char16_t* data = aFamily.BeginReading();
        int32_t index = aFamily.Length();
        while (--index > 0) {
            if (data[index] == ' ') {
                break;
            }
        }
        if (index > 0) {
            gfxFontFamily* base =
                FindFamily(Substring(aFamily, 0, index),
                           FindFamiliesFlags::eNoSearchForLegacyFamilyNames);
            // If we found the "base" family name, and if it has members with
            // legacy names, this will add corresponding font-family entries to
            // the mOtherFamilyNames list; then retry the legacy-family search.
            if (base && base->CheckForLegacyFamilyNames(this)) {
                familyEntry = mOtherFamilyNames.GetWeak(key);
            }
        }
    }

    if (familyEntry) {
        aOutput->AppendElement(familyEntry);
        return true;
    }

    return false;
}

gfxFontEntry*
gfxPlatformFontList::FindFontForFamily(const nsAString& aFamily,
                                       const gfxFontStyle* aStyle)
{
    gfxFontFamily *familyEntry = FindFamily(aFamily);

    if (familyEntry)
        return familyEntry->FindFontForStyle(*aStyle);

    return nullptr;
}

void 
gfxPlatformFontList::AddOtherFamilyName(gfxFontFamily *aFamilyEntry, nsAString& aOtherFamilyName)
{
    nsAutoString key;
    GenerateFontListKey(aOtherFamilyName, key);

    if (!mOtherFamilyNames.GetWeak(key)) {
        mOtherFamilyNames.Put(key, aFamilyEntry);
        LOG_FONTLIST(("(fontlist-otherfamily) canonical family: %s, "
                      "other family: %s\n",
                      NS_ConvertUTF16toUTF8(aFamilyEntry->Name()).get(),
                      NS_ConvertUTF16toUTF8(aOtherFamilyName).get()));
        if (mBadUnderlineFamilyNames.Contains(key))
            aFamilyEntry->SetBadUnderlineFamily();
    }
}

void
gfxPlatformFontList::AddFullname(gfxFontEntry *aFontEntry, nsAString& aFullname)
{
    if (!mExtraNames->mFullnames.GetWeak(aFullname)) {
        mExtraNames->mFullnames.Put(aFullname, aFontEntry);
        LOG_FONTLIST(("(fontlist-fullname) name: %s, fullname: %s\n",
                      NS_ConvertUTF16toUTF8(aFontEntry->Name()).get(),
                      NS_ConvertUTF16toUTF8(aFullname).get()));
    }
}

void
gfxPlatformFontList::AddPostscriptName(gfxFontEntry *aFontEntry, nsAString& aPostscriptName)
{
    if (!mExtraNames->mPostscriptNames.GetWeak(aPostscriptName)) {
        mExtraNames->mPostscriptNames.Put(aPostscriptName, aFontEntry);
        LOG_FONTLIST(("(fontlist-postscript) name: %s, psname: %s\n",
                      NS_ConvertUTF16toUTF8(aFontEntry->Name()).get(),
                      NS_ConvertUTF16toUTF8(aPostscriptName).get()));
    }
}

bool
gfxPlatformFontList::GetStandardFamilyName(const nsAString& aFontName, nsAString& aFamilyName)
{
    aFamilyName.Truncate();
    gfxFontFamily *ff = FindFamily(aFontName);
    if (!ff) {
        return false;
    }
    aFamilyName.Assign(ff->Name());
    return true;
}

gfxFontFamily*
gfxPlatformFontList::GetDefaultFontFamily(const nsACString& aLangGroup,
                                          const nsACString& aGenericFamily)
{
    if (NS_WARN_IF(aLangGroup.IsEmpty()) ||
        NS_WARN_IF(aGenericFamily.IsEmpty())) {
        return nullptr;
    }

    AutoTArray<nsString,4> names;
    gfxFontUtils::AppendPrefsFontList(
        NameListPref(aGenericFamily, aLangGroup).get(), names);

    for (nsString& name : names) {
        gfxFontFamily* fontFamily = FindFamily(name);
        if (fontFamily) {
            return fontFamily;
        }
    }
    return nullptr;
}

gfxCharacterMap*
gfxPlatformFontList::FindCharMap(gfxCharacterMap *aCmap)
{
    aCmap->CalcHash();
    gfxCharacterMap *cmap = AddCmap(aCmap);
    cmap->mShared = true;
    return cmap;
}

// add a cmap to the shared cmap set
gfxCharacterMap*
gfxPlatformFontList::AddCmap(const gfxCharacterMap* aCharMap)
{
    CharMapHashKey *found =
        mSharedCmaps.PutEntry(const_cast<gfxCharacterMap*>(aCharMap));
    return found->GetKey();
}

// remove the cmap from the shared cmap set
void
gfxPlatformFontList::RemoveCmap(const gfxCharacterMap* aCharMap)
{
    // skip lookups during teardown
    if (mSharedCmaps.Count() == 0) {
        return;
    }

    // cmap needs to match the entry *and* be the same ptr before removing
    CharMapHashKey *found =
        mSharedCmaps.GetEntry(const_cast<gfxCharacterMap*>(aCharMap));
    if (found && found->GetKey() == aCharMap) {
        mSharedCmaps.RemoveEntry(found);
    }
}

void
gfxPlatformFontList::ResolveGenericFontNames(
    FontFamilyType aGenericType,
    eFontPrefLang aPrefLang,
    nsTArray<RefPtr<gfxFontFamily>>* aGenericFamilies)
{
    const char* langGroupStr = GetPrefLangName(aPrefLang);
    const char* generic = GetGenericName(aGenericType);

    if (!generic) {
        return;
    }

    AutoTArray<nsString,4> genericFamilies;

    // load family for "font.name.generic.lang"
    gfxFontUtils::AppendPrefsFontList(
        NamePref(generic, langGroupStr).get(), genericFamilies);

    // load fonts for "font.name-list.generic.lang"
    gfxFontUtils::AppendPrefsFontList(
        NameListPref(generic, langGroupStr).get(), genericFamilies);

    nsAtom* langGroup = GetLangGroupForPrefLang(aPrefLang);
    NS_ASSERTION(langGroup, "null lang group for pref lang");

    gfxPlatformFontList::GetFontFamiliesFromGenericFamilies(genericFamilies,
                                                           langGroup,
                                                           aGenericFamilies);

#if 0  // dump out generic mappings
    printf("%s ===> ", prefFontName.get());
    for (uint32_t k = 0; k < aGenericFamilies->Length(); k++) {
        if (k > 0) printf(", ");
        printf("%s", NS_ConvertUTF16toUTF8(aGenericFamilies[k]->Name()).get());
    }
    printf("\n");
#endif
}

void
gfxPlatformFontList::ResolveEmojiFontNames(
    nsTArray<RefPtr<gfxFontFamily>>* aGenericFamilies)
{
    // emoji preference has no lang name
    AutoTArray<nsString,4> genericFamilies;

    nsAutoCString prefFontListName("font.name-list.emoji");
    gfxFontUtils::AppendPrefsFontList(prefFontListName.get(), genericFamilies);

    gfxPlatformFontList::GetFontFamiliesFromGenericFamilies(genericFamilies,
                                                            nullptr,
                                                            aGenericFamilies);
}

void
gfxPlatformFontList::GetFontFamiliesFromGenericFamilies(
    nsTArray<nsString>& aGenericNameFamilies,
    nsAtom* aLangGroup,
    nsTArray<RefPtr<gfxFontFamily>>* aGenericFamilies)
{
    // lookup and add platform fonts uniquely
    for (const nsString& genericFamily : aGenericNameFamilies) {
        gfxFontStyle style;
        style.language = aLangGroup;
        style.systemFont = false;
        AutoTArray<gfxFontFamily*,10> families;
        FindAndAddFamilies(genericFamily, &families, FindFamiliesFlags(0),
                           &style);
        for (gfxFontFamily* f : families) {
            if (!aGenericFamilies->Contains(f)) {
                aGenericFamilies->AppendElement(f);
            }
        }
    }
}

nsTArray<RefPtr<gfxFontFamily>>*
gfxPlatformFontList::GetPrefFontsLangGroup(mozilla::FontFamilyType aGenericType,
                                           eFontPrefLang aPrefLang)
{
    // treat -moz-fixed as monospace
    if (aGenericType == eFamily_moz_fixed) {
        aGenericType = eFamily_monospace;
    }

    if (aGenericType == eFamily_moz_emoji) {
        // Emoji font has no lang
        PrefFontList* prefFonts = mEmojiPrefFont.get();
        if (MOZ_UNLIKELY(!prefFonts)) {
            prefFonts = new PrefFontList;
            ResolveEmojiFontNames(prefFonts);
            mEmojiPrefFont.reset(prefFonts);
        }
        return prefFonts;
    }

    PrefFontList* prefFonts =
        mLangGroupPrefFonts[aPrefLang][aGenericType].get();
    if (MOZ_UNLIKELY(!prefFonts)) {
        prefFonts = new PrefFontList;
        ResolveGenericFontNames(aGenericType, aPrefLang, prefFonts);
        mLangGroupPrefFonts[aPrefLang][aGenericType].reset(prefFonts);
    }
    return prefFonts;
}

void
gfxPlatformFontList::AddGenericFonts(mozilla::FontFamilyType aGenericType,
                                     nsAtom* aLanguage,
                                     nsTArray<gfxFontFamily*>& aFamilyList)
{
    // map lang ==> langGroup
    nsAtom* langGroup = GetLangGroup(aLanguage);

    // langGroup ==> prefLang
    eFontPrefLang prefLang = GetFontPrefLangFor(langGroup);

    // lookup pref fonts
    nsTArray<RefPtr<gfxFontFamily>>* prefFonts =
        GetPrefFontsLangGroup(aGenericType, prefLang);

    if (!prefFonts->IsEmpty()) {
        aFamilyList.AppendElements(*prefFonts);
    }
}

static nsAtom* PrefLangToLangGroups(uint32_t aIndex)
{
    // static array here avoids static constructor
    static nsAtom* gPrefLangToLangGroups[] = {
        #define FONT_PREF_LANG(enum_id_, str_, atom_id_) nsGkAtoms::atom_id_
        #include "gfxFontPrefLangList.h"
        #undef FONT_PREF_LANG
    };

    return aIndex < ArrayLength(gPrefLangToLangGroups)
         ? gPrefLangToLangGroups[aIndex]
         : nsGkAtoms::Unicode;
}

eFontPrefLang
gfxPlatformFontList::GetFontPrefLangFor(const char* aLang)
{
    if (!aLang || !aLang[0]) {
        return eFontPrefLang_Others;
    }
    for (uint32_t i = 0; i < ArrayLength(gPrefLangNames); ++i) {
        if (!PL_strcasecmp(gPrefLangNames[i], aLang)) {
            return eFontPrefLang(i);
        }
    }
    return eFontPrefLang_Others;
}

eFontPrefLang
gfxPlatformFontList::GetFontPrefLangFor(nsAtom *aLang)
{
    if (!aLang)
        return eFontPrefLang_Others;
    nsAutoCString lang;
    aLang->ToUTF8String(lang);
    return GetFontPrefLangFor(lang.get());
}

nsAtom*
gfxPlatformFontList::GetLangGroupForPrefLang(eFontPrefLang aLang)
{
    // the special CJK set pref lang should be resolved into separate
    // calls to individual CJK pref langs before getting here
    NS_ASSERTION(aLang != eFontPrefLang_CJKSet, "unresolved CJK set pref lang");

    return PrefLangToLangGroups(uint32_t(aLang));
}

const char*
gfxPlatformFontList::GetPrefLangName(eFontPrefLang aLang)
{
    if (uint32_t(aLang) < ArrayLength(gPrefLangNames)) {
        return gPrefLangNames[uint32_t(aLang)];
    }
    return nullptr;
}

eFontPrefLang
gfxPlatformFontList::GetFontPrefLangFor(uint8_t aUnicodeRange)
{
    switch (aUnicodeRange) {
        case kRangeSetLatin:   return eFontPrefLang_Western;
        case kRangeCyrillic:   return eFontPrefLang_Cyrillic;
        case kRangeGreek:      return eFontPrefLang_Greek;
        case kRangeHebrew:     return eFontPrefLang_Hebrew;
        case kRangeArabic:     return eFontPrefLang_Arabic;
        case kRangeThai:       return eFontPrefLang_Thai;
        case kRangeKorean:     return eFontPrefLang_Korean;
        case kRangeJapanese:   return eFontPrefLang_Japanese;
        case kRangeSChinese:   return eFontPrefLang_ChineseCN;
        case kRangeTChinese:   return eFontPrefLang_ChineseTW;
        case kRangeDevanagari: return eFontPrefLang_Devanagari;
        case kRangeTamil:      return eFontPrefLang_Tamil;
        case kRangeArmenian:   return eFontPrefLang_Armenian;
        case kRangeBengali:    return eFontPrefLang_Bengali;
        case kRangeCanadian:   return eFontPrefLang_Canadian;
        case kRangeEthiopic:   return eFontPrefLang_Ethiopic;
        case kRangeGeorgian:   return eFontPrefLang_Georgian;
        case kRangeGujarati:   return eFontPrefLang_Gujarati;
        case kRangeGurmukhi:   return eFontPrefLang_Gurmukhi;
        case kRangeKhmer:      return eFontPrefLang_Khmer;
        case kRangeMalayalam:  return eFontPrefLang_Malayalam;
        case kRangeOriya:      return eFontPrefLang_Oriya;
        case kRangeTelugu:     return eFontPrefLang_Telugu;
        case kRangeKannada:    return eFontPrefLang_Kannada;
        case kRangeSinhala:    return eFontPrefLang_Sinhala;
        case kRangeTibetan:    return eFontPrefLang_Tibetan;
        case kRangeSetCJK:     return eFontPrefLang_CJKSet;
        default:               return eFontPrefLang_Others;
    }
}

bool
gfxPlatformFontList::IsLangCJK(eFontPrefLang aLang)
{
    switch (aLang) {
        case eFontPrefLang_Japanese:
        case eFontPrefLang_ChineseTW:
        case eFontPrefLang_ChineseCN:
        case eFontPrefLang_ChineseHK:
        case eFontPrefLang_Korean:
        case eFontPrefLang_CJKSet:
            return true;
        default:
            return false;
    }
}

void
gfxPlatformFontList::GetLangPrefs(eFontPrefLang aPrefLangs[], uint32_t &aLen, eFontPrefLang aCharLang, eFontPrefLang aPageLang)
{
    if (IsLangCJK(aCharLang)) {
        AppendCJKPrefLangs(aPrefLangs, aLen, aCharLang, aPageLang);
    } else {
        AppendPrefLang(aPrefLangs, aLen, aCharLang);
    }

    AppendPrefLang(aPrefLangs, aLen, eFontPrefLang_Others);
}

void
gfxPlatformFontList::AppendCJKPrefLangs(eFontPrefLang aPrefLangs[], uint32_t &aLen, eFontPrefLang aCharLang, eFontPrefLang aPageLang)
{
    // prefer the lang specified by the page *if* CJK
    if (IsLangCJK(aPageLang)) {
        AppendPrefLang(aPrefLangs, aLen, aPageLang);
    }

    // if not set up, set up the default CJK order, based on accept lang settings and locale
    if (mCJKPrefLangs.Length() == 0) {

        // temp array
        eFontPrefLang tempPrefLangs[kMaxLenPrefLangList];
        uint32_t tempLen = 0;

        // Add the CJK pref fonts from accept languages, the order should be same order
        nsAutoCString list;
        Preferences::GetLocalizedCString("intl.accept_languages", list);
        if (!list.IsEmpty()) {
            const char kComma = ',';
            const char *p, *p_end;
            list.BeginReading(p);
            list.EndReading(p_end);
            while (p < p_end) {
                while (nsCRT::IsAsciiSpace(*p)) {
                    if (++p == p_end)
                        break;
                }
                if (p == p_end)
                    break;
                const char *start = p;
                while (++p != p_end && *p != kComma)
                    /* nothing */ ;
                nsAutoCString lang(Substring(start, p));
                lang.CompressWhitespace(false, true);
                eFontPrefLang fpl = gfxPlatformFontList::GetFontPrefLangFor(lang.get());
                switch (fpl) {
                    case eFontPrefLang_Japanese:
                    case eFontPrefLang_Korean:
                    case eFontPrefLang_ChineseCN:
                    case eFontPrefLang_ChineseHK:
                    case eFontPrefLang_ChineseTW:
                        AppendPrefLang(tempPrefLangs, tempLen, fpl);
                        break;
                    default:
                        break;
                }
                p++;
            }
        }

        // Try using app's locale
        nsAutoCString localeStr;
        LocaleService::GetInstance()->GetAppLocaleAsLangTag(localeStr);

        {
          Locale locale(localeStr);
          if (locale.GetLanguage().Equals("ja")) {
              AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_Japanese);
          } else if (locale.GetLanguage().Equals("zh")) {
              if (locale.GetRegion().Equals("CN")) {
                  AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_ChineseCN);
              } else if (locale.GetRegion().Equals("TW")) {
                  AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_ChineseTW);
              } else if (locale.GetRegion().Equals("HK")) {
                  AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_ChineseHK);
              }
          } else if (locale.GetLanguage().Equals("ko")) {
              AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_Korean);
          }
        }

        // Then add the known CJK prefs in order of system preferred locales
        AutoTArray<nsCString,5> prefLocales;
        prefLocales.AppendElement(NS_LITERAL_CSTRING("ja"));
        prefLocales.AppendElement(NS_LITERAL_CSTRING("zh-CN"));
        prefLocales.AppendElement(NS_LITERAL_CSTRING("zh-TW"));
        prefLocales.AppendElement(NS_LITERAL_CSTRING("zh-HK"));
        prefLocales.AppendElement(NS_LITERAL_CSTRING("ko"));

        AutoTArray<nsCString,16> sysLocales;
        AutoTArray<nsCString,16> negLocales;
        if (OSPreferences::GetInstance()->GetSystemLocales(sysLocales)) {
            LocaleService::GetInstance()->NegotiateLanguages(
                sysLocales, prefLocales, NS_LITERAL_CSTRING(""),
                LocaleService::LangNegStrategy::Filtering, negLocales);
            for (const auto& localeStr : negLocales) {
                Locale locale(localeStr);

                if (locale.GetLanguage().Equals("ja")) {
                    AppendPrefLang(tempPrefLangs, tempLen,
                                   eFontPrefLang_Japanese);
                } else if (locale.GetLanguage().Equals("zh")) {
                    if (locale.GetRegion().Equals("CN")) {
                        AppendPrefLang(tempPrefLangs, tempLen,
                                       eFontPrefLang_ChineseCN);
                    } else if (locale.GetRegion().Equals("TW")) {
                        AppendPrefLang(tempPrefLangs, tempLen,
                                       eFontPrefLang_ChineseTW);
                    } else if (locale.GetRegion().Equals("HK")) {
                        AppendPrefLang(tempPrefLangs, tempLen,
                                       eFontPrefLang_ChineseHK);
                    }
                } else if (locale.GetLanguage().Equals("ko")) {
                    AppendPrefLang(tempPrefLangs, tempLen,
                                   eFontPrefLang_Korean);
                }
            }
        }

        // last resort... (the order is same as old gfx.)
        AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_Japanese);
        AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_Korean);
        AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_ChineseCN);
        AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_ChineseHK);
        AppendPrefLang(tempPrefLangs, tempLen, eFontPrefLang_ChineseTW);

        // copy into the cached array
        uint32_t j;
        for (j = 0; j < tempLen; j++) {
            mCJKPrefLangs.AppendElement(tempPrefLangs[j]);
        }
    }

    // append in cached CJK langs
    uint32_t  i, numCJKlangs = mCJKPrefLangs.Length();

    for (i = 0; i < numCJKlangs; i++) {
        AppendPrefLang(aPrefLangs, aLen, (eFontPrefLang) (mCJKPrefLangs[i]));
    }

}

void
gfxPlatformFontList::AppendPrefLang(eFontPrefLang aPrefLangs[], uint32_t& aLen, eFontPrefLang aAddLang)
{
    if (aLen >= kMaxLenPrefLangList) return;

    // make sure
    uint32_t  i = 0;
    while (i < aLen && aPrefLangs[i] != aAddLang) {
        i++;
    }

    if (i == aLen) {
        aPrefLangs[aLen] = aAddLang;
        aLen++;
    }
}

mozilla::FontFamilyType
gfxPlatformFontList::GetDefaultGeneric(eFontPrefLang aLang)
{
    if (aLang == eFontPrefLang_Emoji) {
      return eFamily_moz_emoji;
    }

    // initialize lang group pref font defaults (i.e. serif/sans-serif)
    if (MOZ_UNLIKELY(mDefaultGenericsLangGroup.IsEmpty())) {
        mDefaultGenericsLangGroup.AppendElements(ArrayLength(gPrefLangNames));
        for (uint32_t i = 0; i < ArrayLength(gPrefLangNames); i++) {
            nsAutoCString prefDefaultFontType("font.default.");
            prefDefaultFontType.Append(GetPrefLangName(eFontPrefLang(i)));
            nsAutoCString serifOrSans;
            Preferences::GetCString(prefDefaultFontType.get(), serifOrSans);
            if (serifOrSans.EqualsLiteral("sans-serif")) {
                mDefaultGenericsLangGroup[i] = eFamily_sans_serif;
            } else {
                mDefaultGenericsLangGroup[i] = eFamily_serif;
            }
        }
    }

    if (uint32_t(aLang) < ArrayLength(gPrefLangNames)) {
        return mDefaultGenericsLangGroup[uint32_t(aLang)];
    }
    return eFamily_serif;
}


gfxFontFamily*
gfxPlatformFontList::GetDefaultFont(const gfxFontStyle* aStyle)
{
    gfxFontFamily* family = GetDefaultFontForPlatform(aStyle);
    if (family) {
        return family;
    }
    // Something has gone wrong and we were unable to retrieve a default font
    // from the platform. (Likely the whitelist has blocked all potential
    // default fonts.) As a last resort, we return the first font listed in
    // mFontFamilies.
    return mFontFamilies.Iter().Data();
}

void
gfxPlatformFontList::GetFontFamilyNames(nsTArray<nsString>& aFontFamilyNames)
{
    for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
        RefPtr<gfxFontFamily>& family = iter.Data();
        aFontFamilyNames.AppendElement(family->Name());
    }
}

nsAtom*
gfxPlatformFontList::GetLangGroup(nsAtom* aLanguage)
{
    // map lang ==> langGroup
    nsAtom *langGroup = nullptr;
    if (aLanguage) {
        langGroup = mLangService->GetLanguageGroup(aLanguage);
    }
    if (!langGroup) {
        langGroup = nsGkAtoms::Unicode;
    }
    return langGroup;
}

/* static */ const char*
gfxPlatformFontList::GetGenericName(FontFamilyType aGenericType)
{
    static const char kGeneric_serif[] = "serif";
    static const char kGeneric_sans_serif[] = "sans-serif";
    static const char kGeneric_monospace[] = "monospace";
    static const char kGeneric_cursive[] = "cursive";
    static const char kGeneric_fantasy[] = "fantasy";

    // type should be standard generic type at this point
    NS_ASSERTION(aGenericType >= eFamily_serif &&
                 aGenericType <= eFamily_fantasy,
                 "standard generic font family type required");

    // map generic type to string
    const char *generic = nullptr;
    switch (aGenericType) {
        case eFamily_serif:
            generic = kGeneric_serif;
            break;
        case eFamily_sans_serif:
            generic = kGeneric_sans_serif;
            break;
        case eFamily_monospace:
            generic = kGeneric_monospace;
            break;
        case eFamily_cursive:
            generic = kGeneric_cursive;
            break;
        case eFamily_fantasy:
            generic = kGeneric_fantasy;
            break;
        default:
            break;
    }

    return generic;
}

// mapping of moz lang groups ==> default lang
struct MozLangGroupData {
    nsAtom* const& mozLangGroup;
    const char *defaultLang;
};

const MozLangGroupData MozLangGroups[] = {
    { nsGkAtoms::x_western,      "en" },
    { nsGkAtoms::x_cyrillic,     "ru" },
    { nsGkAtoms::x_devanagari,   "hi" },
    { nsGkAtoms::x_tamil,        "ta" },
    { nsGkAtoms::x_armn,         "hy" },
    { nsGkAtoms::x_beng,         "bn" },
    { nsGkAtoms::x_cans,         "iu" },
    { nsGkAtoms::x_ethi,         "am" },
    { nsGkAtoms::x_geor,         "ka" },
    { nsGkAtoms::x_gujr,         "gu" },
    { nsGkAtoms::x_guru,         "pa" },
    { nsGkAtoms::x_khmr,         "km" },
    { nsGkAtoms::x_knda,         "kn" },
    { nsGkAtoms::x_mlym,         "ml" },
    { nsGkAtoms::x_orya,         "or" },
    { nsGkAtoms::x_sinh,         "si" },
    { nsGkAtoms::x_tamil,        "ta" },
    { nsGkAtoms::x_telu,         "te" },
    { nsGkAtoms::x_tibt,         "bo" },
    { nsGkAtoms::Unicode,        0    }
};

bool
gfxPlatformFontList::TryLangForGroup(const nsACString& aOSLang,
                                       nsAtom* aLangGroup,
                                       nsACString& aFcLang)
{
    // Truncate at '.' or '@' from aOSLang, and convert '_' to '-'.
    // aOSLang is in the form "language[_territory][.codeset][@modifier]".
    // fontconfig takes languages in the form "language-territory".
    // nsLanguageAtomService takes languages in the form language-subtag,
    // where subtag may be a territory.  fontconfig and nsLanguageAtomService
    // handle case-conversion for us.
    const char *pos, *end;
    aOSLang.BeginReading(pos);
    aOSLang.EndReading(end);
    aFcLang.Truncate();
    while (pos < end) {
        switch (*pos) {
            case '.':
            case '@':
                end = pos;
                break;
            case '_':
                aFcLang.Append('-');
                break;
            default:
                aFcLang.Append(*pos);
        }
        ++pos;
    }

    nsAtom *atom = mLangService->LookupLanguage(aFcLang);
    return atom == aLangGroup;
}

void
gfxPlatformFontList::GetSampleLangForGroup(nsAtom* aLanguage,
                                             nsACString& aLangStr,
                                             bool aCheckEnvironment)
{
    aLangStr.Truncate();
    if (!aLanguage) {
        return;
    }

    // set up lang string
    const MozLangGroupData *mozLangGroup = nullptr;

    // -- look it up in the list of moz lang groups
    for (unsigned int i = 0; i < ArrayLength(MozLangGroups); ++i) {
        if (aLanguage == MozLangGroups[i].mozLangGroup) {
            mozLangGroup = &MozLangGroups[i];
            break;
        }
    }

    // -- not a mozilla lang group? Just return the BCP47 string
    //    representation of the lang group
    if (!mozLangGroup) {
        // Not a special mozilla language group.
        // Use aLanguage as a language code.
        aLanguage->ToUTF8String(aLangStr);
        return;
    }

    // -- check the environment for the user's preferred language that
    //    corresponds to this mozilla lang group.
    if (aCheckEnvironment) {
        const char *languages = getenv("LANGUAGE");
        if (languages) {
            const char separator = ':';

            for (const char *pos = languages; true; ++pos) {
                if (*pos == '\0' || *pos == separator) {
                    if (languages < pos &&
                        TryLangForGroup(Substring(languages, pos),
                                        aLanguage, aLangStr))
                        return;

                    if (*pos == '\0')
                        break;

                    languages = pos + 1;
                }
            }
        }
        const char *ctype = setlocale(LC_CTYPE, nullptr);
        if (ctype &&
            TryLangForGroup(nsDependentCString(ctype), aLanguage, aLangStr)) {
            return;
        }
    }

    if (mozLangGroup->defaultLang) {
        aLangStr.Assign(mozLangGroup->defaultLang);
    } else {
        aLangStr.Truncate();
    }
}

void
gfxPlatformFontList::InitLoader()
{
    GetFontFamilyNames(mFontInfo->mFontFamiliesToLoad);
    mStartIndex = 0;
    mNumFamilies = mFontInfo->mFontFamiliesToLoad.Length();
    memset(&(mFontInfo->mLoadStats), 0, sizeof(mFontInfo->mLoadStats));
}

#define FONT_LOADER_MAX_TIMESLICE 100  // max time for one pass through RunLoader = 100ms

bool
gfxPlatformFontList::LoadFontInfo()
{
    TimeStamp start = TimeStamp::Now();
    uint32_t i, endIndex = mNumFamilies;
    bool loadCmaps = !UsesSystemFallback() ||
        gfxPlatform::GetPlatform()->UseCmapsDuringSystemFallback();

    // for each font family, load in various font info
    for (i = mStartIndex; i < endIndex; i++) {
        nsAutoString key;
        gfxFontFamily *familyEntry;
        GenerateFontListKey(mFontInfo->mFontFamiliesToLoad[i], key);

        // lookup in canonical (i.e. English) family name list
        if (!(familyEntry = mFontFamilies.GetWeak(key))) {
            continue;
        }

        // read in face names
        familyEntry->ReadFaceNames(this, NeedFullnamePostscriptNames(), mFontInfo);

        // load the cmaps if needed
        if (loadCmaps) {
            familyEntry->ReadAllCMAPs(mFontInfo);
        }

        // limit the time spent reading fonts in one pass
        TimeDuration elapsed = TimeStamp::Now() - start;
        if (elapsed.ToMilliseconds() > FONT_LOADER_MAX_TIMESLICE &&
                i + 1 != endIndex) {
            endIndex = i + 1;
            break;
        }
    }

    mStartIndex = endIndex;
    bool done = mStartIndex >= mNumFamilies;

    if (LOG_FONTINIT_ENABLED()) {
        TimeDuration elapsed = TimeStamp::Now() - start;
        LOG_FONTINIT(("(fontinit) fontloader load pass %8.2f ms done %s\n",
                      elapsed.ToMilliseconds(), (done ? "true" : "false")));
    }

    if (done) {
        mOtherFamilyNamesInitialized = true;
        CancelInitOtherFamilyNamesTask();
        mFaceNameListsInitialized = true;
    }

    return done;
}

void
gfxPlatformFontList::CleanupLoader()
{
    mFontFamiliesToLoad.Clear();
    mNumFamilies = 0;
    bool rebuilt = false, forceReflow = false;

    // if had missed face names that are now available, force reflow all
    if (mFaceNamesMissed) {
        for (auto it = mFaceNamesMissed->Iter(); !it.Done(); it.Next()) {
            if (FindFaceName(it.Get()->GetKey())) {
                rebuilt = true;
                RebuildLocalFonts();
                break;
            }
        }
        mFaceNamesMissed = nullptr;
    }

    if (mOtherNamesMissed) {
        for (auto it = mOtherNamesMissed->Iter(); !it.Done(); it.Next()) {
            if (FindFamily(it.Get()->GetKey(),
                           (FindFamiliesFlags::eForceOtherFamilyNamesLoading |
                            FindFamiliesFlags::eNoAddToNamesMissedWhenSearching))) {
                forceReflow = true;
                ForceGlobalReflow();
                break;
            }
        }
        mOtherNamesMissed = nullptr;
    }

    if (LOG_FONTINIT_ENABLED() && mFontInfo) {
        LOG_FONTINIT(("(fontinit) fontloader load thread took %8.2f ms "
                      "%d families %d fonts %d cmaps "
                      "%d facenames %d othernames %s %s",
                      mLoadTime.ToMilliseconds(),
                      mFontInfo->mLoadStats.families,
                      mFontInfo->mLoadStats.fonts,
                      mFontInfo->mLoadStats.cmaps,
                      mFontInfo->mLoadStats.facenames,
                      mFontInfo->mLoadStats.othernames,
                      (rebuilt ? "(userfont sets rebuilt)" : ""),
                      (forceReflow ? "(global reflow)" : "")));
    }

    gfxFontInfoLoader::CleanupLoader();
}

void
gfxPlatformFontList::GetPrefsAndStartLoader()
{
    uint32_t delay =
        std::max(1u, Preferences::GetUint(FONT_LOADER_DELAY_PREF));
    uint32_t interval =
        std::max(1u, Preferences::GetUint(FONT_LOADER_INTERVAL_PREF));

    StartLoader(delay, interval);
}

void
gfxPlatformFontList::RebuildLocalFonts()
{
    for (auto it = mUserFontSetList.Iter(); !it.Done(); it.Next()) {
        it.Get()->GetKey()->RebuildLocalRules();
    }
}

void
gfxPlatformFontList::ClearLangGroupPrefFonts()
{
    for (uint32_t i = eFontPrefLang_First;
         i < eFontPrefLang_First + eFontPrefLang_Count; i++) {
        auto& prefFontsLangGroup = mLangGroupPrefFonts[i];
        for (uint32_t j = eFamily_generic_first;
             j < eFamily_generic_first + eFamily_generic_count; j++) {
            prefFontsLangGroup[j] = nullptr;
        }
    }
    mCJKPrefLangs.Clear();
}

// Support for memory reporting

// this is also used by subclasses that hold additional font tables
/*static*/ size_t
gfxPlatformFontList::SizeOfFontFamilyTableExcludingThis(
    const FontFamilyTable& aTable,
    MallocSizeOf aMallocSizeOf)
{
    size_t n = aTable.ShallowSizeOfExcludingThis(aMallocSizeOf);
    for (auto iter = aTable.ConstIter(); !iter.Done(); iter.Next()) {
        // We don't count the size of the family here, because this is an
        // *extra* reference to a family that will have already been counted in
        // the main list.
        n += iter.Key().SizeOfExcludingThisIfUnshared(aMallocSizeOf);
    }
    return n;
}

/*static*/ size_t
gfxPlatformFontList::SizeOfFontEntryTableExcludingThis(
    const FontEntryTable& aTable,
    MallocSizeOf aMallocSizeOf)
{
    size_t n = aTable.ShallowSizeOfExcludingThis(aMallocSizeOf);
    for (auto iter = aTable.ConstIter(); !iter.Done(); iter.Next()) {
        // The font itself is counted by its owning family; here we only care
        // about the names stored in the hashtable keys.
        n += iter.Key().SizeOfExcludingThisIfUnshared(aMallocSizeOf);
    }
    return n;
}

void
gfxPlatformFontList::AddSizeOfExcludingThis(MallocSizeOf aMallocSizeOf,
                                            FontListSizes* aSizes) const
{
    aSizes->mFontListSize +=
        mFontFamilies.ShallowSizeOfExcludingThis(aMallocSizeOf);
    for (auto iter = mFontFamilies.ConstIter(); !iter.Done(); iter.Next()) {
        aSizes->mFontListSize +=
            iter.Key().SizeOfExcludingThisIfUnshared(aMallocSizeOf);
        iter.Data()->AddSizeOfIncludingThis(aMallocSizeOf, aSizes);
    }

    aSizes->mFontListSize +=
        SizeOfFontFamilyTableExcludingThis(mOtherFamilyNames, aMallocSizeOf);

    if (mExtraNames) {
        aSizes->mFontListSize +=
            SizeOfFontEntryTableExcludingThis(mExtraNames->mFullnames,
                                              aMallocSizeOf);
        aSizes->mFontListSize +=
            SizeOfFontEntryTableExcludingThis(mExtraNames->mPostscriptNames,
                                              aMallocSizeOf);
    }

    for (uint32_t i = eFontPrefLang_First;
         i < eFontPrefLang_First + eFontPrefLang_Count; i++) {
        auto& prefFontsLangGroup = mLangGroupPrefFonts[i];
        for (uint32_t j = eFamily_generic_first;
             j < eFamily_generic_first + eFamily_generic_count; j++) {
            PrefFontList* pf = prefFontsLangGroup[j].get();
            if (pf) {
                aSizes->mFontListSize +=
                    pf->ShallowSizeOfExcludingThis(aMallocSizeOf);
            }
        }
    }

    aSizes->mFontListSize +=
        mCodepointsWithNoFonts.SizeOfExcludingThis(aMallocSizeOf);
    aSizes->mFontListSize +=
        mFontFamiliesToLoad.ShallowSizeOfExcludingThis(aMallocSizeOf);

    aSizes->mFontListSize +=
        mBadUnderlineFamilyNames.SizeOfExcludingThis(aMallocSizeOf);

    aSizes->mFontListSize +=
        mSharedCmaps.ShallowSizeOfExcludingThis(aMallocSizeOf);
    for (auto iter = mSharedCmaps.ConstIter(); !iter.Done(); iter.Next()) {
        aSizes->mCharMapsSize +=
            iter.Get()->GetKey()->SizeOfIncludingThis(aMallocSizeOf);
    }
}

void
gfxPlatformFontList::AddSizeOfIncludingThis(MallocSizeOf aMallocSizeOf,
                                            FontListSizes* aSizes) const
{
    aSizes->mFontListSize += aMallocSizeOf(this);
    AddSizeOfExcludingThis(aMallocSizeOf, aSizes);
}

bool
gfxPlatformFontList::IsFontFamilyWhitelistActive()
{
    return mFontFamilyWhitelistActive;
}

void
gfxPlatformFontList::InitOtherFamilyNamesInternal(bool aDeferOtherFamilyNamesLoading)
{
    if (mOtherFamilyNamesInitialized) {
        return;
    }

    if (aDeferOtherFamilyNamesLoading) {
        TimeStamp start = TimeStamp::Now();
        bool timedOut = false;

        for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
            RefPtr<gfxFontFamily>& family = iter.Data();
            family->ReadOtherFamilyNames(this);
            TimeDuration elapsed = TimeStamp::Now() - start;
            if (elapsed.ToMilliseconds() > OTHERNAMES_TIMEOUT) {
                timedOut = true;
                break;
            }
        }

        if (!timedOut) {
            mOtherFamilyNamesInitialized = true;
            CancelInitOtherFamilyNamesTask();
        }
        TimeStamp end = TimeStamp::Now();
        Telemetry::AccumulateTimeDelta(Telemetry::FONTLIST_INITOTHERFAMILYNAMES,
                                       start, end);

        if (LOG_FONTINIT_ENABLED()) {
            TimeDuration elapsed = end - start;
            LOG_FONTINIT(("(fontinit) InitOtherFamilyNames took %8.2f ms %s",
                          elapsed.ToMilliseconds(),
                          (timedOut ? "timeout" : "")));
        }
    } else {
        TimeStamp start = TimeStamp::Now();

        for (auto iter = mFontFamilies.Iter(); !iter.Done(); iter.Next()) {
            RefPtr<gfxFontFamily>& family = iter.Data();
            family->ReadOtherFamilyNames(this);
        }

        mOtherFamilyNamesInitialized = true;
        CancelInitOtherFamilyNamesTask();

        TimeStamp end = TimeStamp::Now();
        Telemetry::AccumulateTimeDelta(Telemetry::FONTLIST_INITOTHERFAMILYNAMES_NO_DEFERRING,
                                       start, end);

        if (LOG_FONTINIT_ENABLED()) {
            TimeDuration elapsed = end - start;
            LOG_FONTINIT(("(fontinit) InitOtherFamilyNames without deferring took %8.2f ms",
                          elapsed.ToMilliseconds()));
        }
    }
}

void
gfxPlatformFontList::CancelInitOtherFamilyNamesTask()
{
    if (mPendingOtherFamilyNameTask) {
        mPendingOtherFamilyNameTask->Cancel();
        mPendingOtherFamilyNameTask = nullptr;
    }
}

#undef LOG
#undef LOG_ENABLED
