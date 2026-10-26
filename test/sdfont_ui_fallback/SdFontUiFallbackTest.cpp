// Behavioral regression tests for CJK UI-font fallback lifetime around the
// BookOrbit catalog: entering the catalog releases the SD fonts (TLS heap
// headroom), which also clears the renderer's UI CJK fallback map — catalog
// rows then render CJK titles with the Latin-only built-in UI font (tofu).
//
// These tests drive the real SdCardFontSystem/SdCardFontManager/registry, the
// real settings header, real .cpfont fixtures in an in-memory filesystem, and
// the real GfxRenderer measurement path (getTextWidth/truncatedText route CJK
// through the registered fallback — see GfxRenderer::resolveTextFontId).
//
// Production change each test guards:
//  - EnsureUiFallbacksRestoresRoutingAfterNetworkRelease:
//    SdCardFontSystem::ensureUiFallbacks() re-registers UI fallbacks from the
//    saved family preference after releaseForNetwork() cleared them.
//  - EnsureUiFallbacksUsesNearestInstalledSizeWhenUiSizesMissing:
//    a family without exact 8/10/12 pt files still serves UI fallbacks from
//    its nearest installed size.
//  - ReaderLoadedFamilySeedsFallbackForMissingUiSizes:
//    the reader-path setupUiFallbacks() seeds missing UI sizes with the
//    already-loaded reader font of the same family.
//  - ColdFallbackFontServesGlyphsOnDemand:
//    a freshly loaded SD font has no resident glyph tables (the reader
//    prewarms them); the family-level API must consult the on-demand loader
//    or every character of a routed string renders as a replacement diamond.
//  - EnsureUiFallbacksSkipsSubsetUiSizesForCoveringOnes /
//    EnsureUiFallbacksSkipsProbeOnlyUiSizesForCoveringOnes:
//    sizes within one family can differ in codepoint coverage; a UI slot is
//    only wired to a size that covers a sample of everyday CJK, preferring
//    the nearest covering size (subset UI sizes render replacement diamonds).
//  - TruncatedCjkTextStaysValidUtf8ThroughFallback:
//    ellipsis truncation on the resolved fallback font never splits a UTF-8
//    sequence and measures with the same font it draws with.

#include <GfxRenderer.h>
#include <SdCardFont.h>
#include <Utf8.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "fontIds.h"

// Out-of-line members normally defined in src/CrossPointSettings.cpp and
// lib/Serialization/PersistableStore.cpp. The native suite provides minimal
// bodies; tests set the underlying fields directly.
uint8_t CrossPointSettings::getSdFontTargetPointSize() const { return readerFontPointSize; }
int CrossPointSettings::getBuiltInReaderFontId() const { return 999001; }
int CrossPointSettings::getReaderFontId() const { return getBuiltInReaderFontId(); }
uint8_t CrossPointSettings::defaultUiScale() { return 3; }
bool CrossPointSettings::changeReaderFontSize(bool, FontSizeStepMode) { return false; }
uint8_t CrossPointSettings::getSdFontRangePointSize(uint8_t, uint8_t) { return 12; }
bool CrossPointSettings::saveToFile() const { return true; }
bool PersistableStoreBase::writeDocToFile(const char*, const JsonDocument&) { return true; }
bool PersistableStoreBase::writeDocToFileAtomically(const char*, const JsonDocument&) { return true; }
bool PersistableStoreBase::readDocFromFile(const char*, JsonDocument&) { return false; }

namespace {

// Minimal deterministic .cpfont builder (same layout the production loader
// parses): Latin always, an optional CJK interval 0x4E00-0x4E20. Every glyph
// advances size*16 units, so the width of a two-CJK-char string is 32*size —
// that numeric identity is how routing through a fallback is asserted.
struct CpfontFixture {
  std::vector<EpdUnicodeInterval> intervals;
  std::vector<EpdGlyph> glyphs;
  std::vector<uint8_t> pixels;
  int size;

  explicit CpfontFixture(const int glyphSize, const bool withCjk, const bool includeCatalogMetadata = true)
      : size(glyphSize) {
    intervals.push_back({32u, 127u, static_cast<uint32_t>(glyphs.size())});
    for (unsigned cp = 32; cp <= 127; ++cp) addGlyph(size * 16);
    if (withCjk) {
      // CJK glyphs advance double: a measurement routed through a CJK
      // fallback can then never coincide with the Latin font's own tofu
      // rendering of the same string.
      static constexpr uint32_t kProbeOnlyCodepoints[] = {0x4E00};
      static constexpr uint32_t kCatalogCodepoints[] = {0x2026, 0x3001, 0x3002, 0x4E00, 0x4E09, 0x4F53, 0x5212, 0x5218,
                                                        0x6148, 0x633D, 0x6551, 0x6B23, 0x8BA1, 0xFF01, 0xFF0C, 0xFF1F};
      std::vector<uint32_t> codepoints;
      if (includeCatalogMetadata) {
        // A "full" size must clear the production coverage gate: everyday
        // sample characters plus the catalog punctuation the tests measure.
        // The interval table must stay sorted and duplicate-free for the
        // loader's binary search.
        codepoints.insert(codepoints.end(), std::begin(kCatalogCodepoints), std::end(kCatalogCodepoints));
        codepoints.insert(codepoints.end(), std::begin(SdFontCjkCoverage::kSample),
                          std::end(SdFontCjkCoverage::kSample));
        std::sort(codepoints.begin(), codepoints.end());
        codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());
      } else {
        codepoints.insert(codepoints.end(), std::begin(kProbeOnlyCodepoints), std::end(kProbeOnlyCodepoints));
      }
      for (const uint32_t codepoint : codepoints) {
        intervals.push_back({codepoint, codepoint, static_cast<uint32_t>(glyphs.size())});
        addGlyph(size * 32);
      }
    }
  }

  void addGlyph(const int advanceX) {
    EpdGlyph g{};
    g.width = size;
    g.height = size + 4;
    g.advanceX = advanceX;
    g.left = -3;
    g.top = size + 1;
    g.dataOffset = pixels.size();
    g.dataLength = (g.width * g.height + 3) / 4;
    glyphs.push_back(g);
    for (int b = 0; b < g.dataLength; ++b) pixels.push_back(b % 2 ? 0x1B : 0xE4);
  }

  template <class T>
  static void append(std::vector<uint8_t>& bytes, const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    bytes.insert(bytes.end(), p, p + sizeof(value));
  }

  std::vector<uint8_t> file() const {
    std::vector<uint8_t> bytes(64);
    memcpy(bytes.data(), "CPFONT\0\0", 8);
    auto put16 = [&](int o, uint16_t v) { memcpy(bytes.data() + o, &v, 2); };
    auto put32 = [&](int o, uint32_t v) { memcpy(bytes.data() + o, &v, 4); };
    put16(8, CPFONT_VERSION);
    put16(10, 1);
    bytes[12] = 1;
    put32(36, intervals.size());
    put32(40, glyphs.size());
    bytes[44] = static_cast<uint8_t>(size + 6);
    put16(45, static_cast<uint16_t>(size + 1));
    put16(47, static_cast<uint16_t>(-3));
    put32(56, 64);
    for (const auto& i : intervals) append(bytes, i);
    for (const auto& g : glyphs) append(bytes, g);
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    return bytes;
  }
};

void installFamily(const char* name, const std::vector<uint8_t>& sizes, const bool withCjk,
                   const bool includeCatalogMetadata = true) {
  for (const uint8_t size : sizes) {
    char path[160];
    snprintf(path, sizeof(path), "/fonts/%s/%s_%u.cpfont", name, name, size);
    host_fs::put(path, CpfontFixture(size, withCjk, includeCatalogMetadata).file());
  }
}

void resetSettings(const char* familyName, const uint8_t pointSize) {
  auto& settings = const_cast<CrossPointSettings&>(SETTINGS);
  snprintf(settings.sdFontFamilyName, sizeof(settings.sdFontFamilyName), "%s", familyName);
  settings.sdUiFontFamilyName[0] = '\0';
  settings.readerFontPointSize = pointSize;
  settings.legacySdFontSizeStep = UINT8_MAX;
  settings.dictionarySdFontFamilyName[0] = '\0';
  settings.dictionaryFontPointSize = 0;
}

constexpr int kUiPrimaryIds[] = {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID};

// Registers Latin-only built-in UI stand-ins under the real UI font IDs, the
// way main.cpp registers the built-in UI fonts. Their glyphs also advance
// size*16, so routed-vs-unrouted CJK differs whenever the fallback size
// differs from the UI font's own size.
struct UiPrimaryFonts {
  SdCardFont font12, font10, font8;
  bool installed = false;

  void install(GfxRenderer& renderer) {
    if (!installed) {
      host_fs::put("/builtin/ui12.cpfont", CpfontFixture(12, false).file());
      host_fs::put("/builtin/ui10.cpfont", CpfontFixture(10, false).file());
      host_fs::put("/builtin/ui8.cpfont", CpfontFixture(8, false).file());
      if (!font12.load("/builtin/ui12.cpfont") || !font10.load("/builtin/ui10.cpfont") ||
          !font8.load("/builtin/ui8.cpfont")) {
        ADD_FAILURE() << "built-in UI fixture fonts failed to load";
        return;
      }
      installed = true;
    }
    renderer.insertFont(UI_12_FONT_ID, EpdFontFamily(font12.getEpdFont(0), font12.getEpdFont(1), font12.getEpdFont(2),
                                                     font12.getEpdFont(3)));
    renderer.insertFont(UI_10_FONT_ID, EpdFontFamily(font10.getEpdFont(0), font10.getEpdFont(1), font10.getEpdFont(2),
                                                     font10.getEpdFont(3)));
    renderer.insertFont(SMALL_FONT_ID, EpdFontFamily(font8.getEpdFont(0), font8.getEpdFont(1), font8.getEpdFont(2),
                                                     font8.getEpdFont(3)));
  }
};

struct SdFontUiFallbackTest : testing::Test {
  HalDisplay display{800, 480};
  GfxRenderer renderer{display};
  UiPrimaryFonts uiPrimary;
  SdCardFontSystem sdFontSystem;  // fresh per test: no cross-test font state

  void SetUp() override {
    host_fs::reset();
    sdFontSystem.markRegistryDirty();  // also drops the on-card catalog index
    resetSettings("", 12);
    renderer.begin();
    uiPrimary.install(renderer);
  }

  void TearDown() override { sdFontSystem.releaseLoadedFont(renderer); }
};

// 三 (0x4E09) is inside the fixture's CJK interval. Fixture sizes are
// identified by their header advanceY, which is point size + 6.
constexpr const char* kCjkTitle = "\xe4\xb8\x89\xe4\xbd\x93";  // 三体

int cjkFontIdForSize(GfxRenderer& renderer, const int pointSize) {
  for (const auto& [id, family] : renderer.getFontMap()) {
    if (!family.hasCodepoint(0x4E09, EpdFontFamily::REGULAR)) continue;
    const EpdFontData* data = family.getData(EpdFontFamily::REGULAR);
    if (data && data->advanceY == pointSize + 6) return id;
  }
  return 0;
}

bool cjkRoutedThroughSize(GfxRenderer& renderer, const int uiId, const int pointSize, const char* text = kCjkTitle) {
  const int cjkId = cjkFontIdForSize(renderer, pointSize);
  if (cjkId == 0) return false;
  return renderer.getTextWidth(uiId, text) == renderer.getTextWidth(cjkId, text);
}

uint64_t renderedTextFingerprint(GfxRenderer& renderer, const int fontId, const char* text) {
  renderer.clearScreen();
  renderer.drawText(fontId, 20, 80, text);
  uint64_t fingerprint = 14695981039346656037ULL;
  for (int y = 0; y < renderer.getScreenHeight(); ++y) {
    for (int x = 0; x < renderer.getScreenWidth(); ++x) {
      fingerprint ^= renderer.isPixelBlack(x, y) ? 1U : 0U;
      fingerprint *= 1099511628211ULL;
    }
  }
  return fingerprint;
}

}  // namespace

// Defect A: the catalog releases fonts for network heap headroom; UI fallback
// registration must be restorable from the saved family preference alone,
// without the reader font being loaded.
TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksRestoresRoutingAfterNetworkRelease) {
  installFamily("WenKaiCJK", {8, 10, 12}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.ensureLoaded(renderer);  // reader had the family loaded
  sdFontSystem.releaseForNetwork(renderer);

  // The documented defect: after the release the UI fonts render CJK with
  // their own Latin glyphs (tofu), not through any fallback.
  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
  EXPECT_FALSE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));

  // The fix: re-register UI fallbacks from the preference, reader not loaded.
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_10_FONT_ID, 10));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, SMALL_FONT_ID, 8));
  // Latin-only strings keep the built-in UI font: appearance unchanged.
  const int latinWidth = renderer.getTextWidth(UI_12_FONT_ID, "ABC 123");
  EXPECT_NE(latinWidth, 0);
}

TEST_F(SdFontUiFallbackTest, CatalogTitlesAuthorsAndMixedPunctuationUseCjkGlyphs) {
  installFamily("WenKaiCJK", {12}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12, "挽救计划"));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12, "三体"));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12, "刘慈欣"));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12, "Book 2 - 三体，刘慈欣"));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12, "三体，刘慈欣。…"));
  constexpr const char* mixedMetadata = "Book 2 - 三体，刘慈欣。…";
  const int cjkFontId = cjkFontIdForSize(renderer, 12);
  ASSERT_NE(cjkFontId, 0);
  EXPECT_EQ(renderedTextFingerprint(renderer, UI_12_FONT_ID, mixedMetadata),
            renderedTextFingerprint(renderer, cjkFontId, mixedMetadata));
}

// A freshly loaded SD font serves stub data: glyph tables only become resident
// through the reader's prewarm. UI screens draw cold, so the family-level API
// must consult the on-demand loader — otherwise every character of a routed
// string (Chinese and Latin alike) renders as the replacement diamond.
TEST_F(SdFontUiFallbackTest, ColdFallbackFontServesGlyphsOnDemand) {
  installFamily("WenKaiCJK", {12}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  const int cjkFontId = cjkFontIdForSize(renderer, 12);
  ASSERT_NE(cjkFontId, 0);
  const auto it = renderer.getFontMap().find(cjkFontId);
  ASSERT_NE(it, renderer.getFontMap().end());

  EXPECT_EQ(it->second.getFallbackCodepoint(0x4E09, EpdFontFamily::REGULAR), 0x4E09);
  const EpdGlyph* glyph = it->second.getGlyph(0x4E09, EpdFontFamily::REGULAR);
  ASSERT_NE(glyph, nullptr);
  EXPECT_GT(glyph->width, 0);

  // Real pixels, not a row of replacement diamonds.
  EXPECT_NE(renderedTextFingerprint(renderer, UI_12_FONT_ID, kCjkTitle),
            renderedTextFingerprint(renderer, UI_12_FONT_ID, "\xEF\xBF\xBD\xEF\xBF\xBD"));
}

TEST_F(SdFontUiFallbackTest, MissingCodepointDoesNotMasqueradeAsOnDemandReplacementGlyph) {
  CpfontFixture fixture(12, /*withCjk=*/true);
  fixture.intervals.push_back({0xFFFDu, 0xFFFDu, static_cast<uint32_t>(fixture.glyphs.size())});
  fixture.addGlyph(12 * 32);
  host_fs::put("/fonts/WenKaiCJK/WenKaiCJK_12.cpfont", fixture.file());
  resetSettings("WenKaiCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  const int cjkFontId = cjkFontIdForSize(renderer, 12);
  ASSERT_NE(cjkFontId, 0);
  const auto it = renderer.getFontMap().find(cjkFontId);
  ASSERT_NE(it, renderer.getFontMap().end());
  EXPECT_EQ(it->second.getFallbackCodepoint(0x20AC, EpdFontFamily::REGULAR), 0xFFFD);
}

TEST_F(SdFontUiFallbackTest, ExplicitUiFamilyWorksWithBuiltinReaderFont) {
  installFamily("UiCJK", {12}, /*withCjk=*/true);
  resetSettings("", 12);
  sdFontSystem.begin(renderer);
  const int readerFontId = SETTINGS.getReaderFontId();
  snprintf(SETTINGS.sdUiFontFamilyName, sizeof(SETTINGS.sdUiFontFamilyName), "UiCJK");

  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
  EXPECT_EQ(SETTINGS.sdFontFamilyName[0], '\0');
  EXPECT_EQ(SETTINGS.getReaderFontId(), readerFontId);
}

TEST_F(SdFontUiFallbackTest, ExplicitUiFamilySurvivesPerBookReaderFontChanges) {
  installFamily("UiCJK", {12}, /*withCjk=*/true);
  resetSettings("", 12);
  snprintf(SETTINGS.sdUiFontFamilyName, sizeof(SETTINGS.sdUiFontFamilyName), "UiCJK");
  sdFontSystem.begin(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);
  ASSERT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));

  snprintf(SETTINGS.sdFontFamilyName, sizeof(SETTINGS.sdFontFamilyName), "PerBookReaderFamily");
  sdFontSystem.ensureUiFallbacks(renderer);
  SETTINGS.sdFontFamilyName[0] = '\0';
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
  EXPECT_STREQ(SETTINGS.sdFontFamilyName, "");
  EXPECT_STREQ(SETTINGS.sdUiFontFamilyName, "UiCJK");
}

// Defect B (catalog path): a family installed only at reading sizes has no
// exact 8/10/12 pt files; UI fallbacks must come from the nearest size.
TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksUsesNearestInstalledSizeWhenUiSizesMissing) {
  installFamily("WenKaiCJK", {16, 18, 20}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 18);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  for (const int uiId : kUiPrimaryIds) {
    EXPECT_TRUE(cjkRoutedThroughSize(renderer, uiId, 16))
        << "UI font " << uiId << " did not route through the nearest installed size";
  }
}

TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksUsesSmallOnlyFontForEveryUiSlot) {
  installFamily("WenKaiCJK", {8}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 8);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  for (const int uiId : kUiPrimaryIds) {
    EXPECT_TRUE(cjkRoutedThroughSize(renderer, uiId, 8))
        << "UI font " << uiId << " did not reuse the only installed CJK size";
  }
}

TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksChoosesNearestSizePerUiSlot) {
  installFamily("WenKaiCJK", {8, 14}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 14);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 14));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_10_FONT_ID, 8));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, SMALL_FONT_ID, 8));
}

// Sizes within one family can differ in codepoint coverage (converted packs
// sometimes subset their small sizes). A slot wired to a Latin-only size never
// routes CJK, and the text falls back to the Latin UI font, which draws the
// replacement diamond for every Chinese character. The slot must instead use a
// size of the family that covers everyday CJK.
TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksSkipsSubsetUiSizesForCoveringOnes) {
  host_fs::put("/fonts/MixedCjk/MixedCjk_12.cpfont", CpfontFixture(12, /*withCjk=*/false).file());
  host_fs::put("/fonts/MixedCjk/MixedCjk_20.cpfont", CpfontFixture(20, /*withCjk=*/true).file());
  resetSettings("MixedCjk", 20);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  for (const int uiId : kUiPrimaryIds) {
    EXPECT_TRUE(cjkRoutedThroughSize(renderer, uiId, 20)) << "UI font " << uiId << " kept a subset size";
  }
}

// Same defect with a subtler trigger: the small size passes the one-glyph CJK
// probe but carries almost no real text. Everyday characters must still come
// from a size that actually covers them.
TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksSkipsProbeOnlyUiSizesForCoveringOnes) {
  host_fs::put("/fonts/ProbeCjk/ProbeCjk_12.cpfont",
               CpfontFixture(12, /*withCjk=*/true, /*includeCatalogMetadata=*/false).file());
  host_fs::put("/fonts/ProbeCjk/ProbeCjk_20.cpfont", CpfontFixture(20, /*withCjk=*/true).file());
  resetSettings("ProbeCjk", 20);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  for (const int uiId : kUiPrimaryIds) {
    EXPECT_TRUE(cjkRoutedThroughSize(renderer, uiId, 20)) << "UI font " << uiId << " kept a probe-only size";
  }
}

// Defect B (reader path): while the reader font is resident, a family without
// exact UI sizes must seed the UI fallbacks from the loaded reader size.
TEST_F(SdFontUiFallbackTest, ReaderLoadedFamilySeedsFallbackForMissingUiSizes) {
  installFamily("WenKaiCJK", {18}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 18);

  sdFontSystem.begin(renderer);
  sdFontSystem.ensureLoaded(renderer);

  for (const int uiId : kUiPrimaryIds) {
    EXPECT_TRUE(cjkRoutedThroughSize(renderer, uiId, 18))
        << "UI font " << uiId << " did not route through the resident reader size";
  }
}

TEST_F(SdFontUiFallbackTest, ReaderLoadAfterUiOnlyFallbackUsesRequestedReaderSize) {
  installFamily("WenKaiCJK", {8, 10, 12, 18}, /*withCjk=*/true);
  sdFontSystem.begin(renderer);
  resetSettings("WenKaiCJK", 18);
  sdFontSystem.ensureUiFallbacks(renderer);
  sdFontSystem.ensureLoaded(renderer);

  const int readerFontId = sdFontSystem.resolveFontId("WenKaiCJK", 18);
  const auto readerFont = renderer.getFontMap().find(readerFontId);
  ASSERT_NE(readerFont, renderer.getFontMap().end());
  int width = 0;
  int height = 0;
  readerFont->second.getTextDimensions(kCjkTitle, &width, &height);
  EXPECT_EQ(height, 18 + 4);
}

TEST_F(SdFontUiFallbackTest, DictionaryRestoreUsesNearestUiSizes) {
  installFamily("ReaderCJK", {8, 14}, /*withCjk=*/true);
  installFamily("DictionaryCJK", {12}, /*withCjk=*/true);
  resetSettings("ReaderCJK", 14);
  sdFontSystem.begin(renderer);

  const DictionaryFontActivation dictionary = sdFontSystem.activateDictionaryFont(renderer, "DictionaryCJK", 12);
  ASSERT_TRUE(dictionary.usingDictionaryFont);
  ASSERT_NE(sdFontSystem.restoreReaderFont(renderer), 0);

  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 14));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_10_FONT_ID, 8));
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, SMALL_FONT_ID, 8));
}

TEST_F(SdFontUiFallbackTest, ChangingUiFamilyDoesNotReusePreviousFamilyAtSameSize) {
  installFamily("FullCjk", {12}, /*withCjk=*/true);
  installFamily("PartialCjk", {12}, /*withCjk=*/true, /*includeCatalogMetadata=*/false);
  sdFontSystem.begin(renderer);
  resetSettings("FullCjk", 12);
  sdFontSystem.ensureUiFallbacks(renderer);
  ASSERT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));

  resetSettings("PartialCjk", 12);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
}

TEST_F(SdFontUiFallbackTest, DirtyRegistryReloadDoesNotReuseReplacedFontSize) {
  installFamily("WenKaiCJK", {12}, /*withCjk=*/true);
  sdFontSystem.begin(renderer);
  resetSettings("WenKaiCJK", 12);
  sdFontSystem.ensureUiFallbacks(renderer);
  ASSERT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));

  host_fs::put("/fonts/WenKaiCJK/WenKaiCJK_12.cpfont", CpfontFixture(12, true, false).file());
  sdFontSystem.markRegistryDirty();
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
}

// A family without CJK coverage must not be registered (no false coverage
// claims), and the negative result is cached: no SD re-reads on re-entry.
TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksSkipsLatinFamilyWithoutRepeatScans) {
  installFamily("LatinOnly", {8, 10, 12}, /*withCjk=*/false);
  resetSettings("LatinOnly", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  const size_t uiFontCount = renderer.getFontMap().size();
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
  EXPECT_FALSE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
  EXPECT_EQ(renderer.getFontMap().size(), uiFontCount);

  const size_t readsAfterFirst = host_fs::totalReadCalls();
  sdFontSystem.ensureUiFallbacks(renderer);
  EXPECT_EQ(host_fs::totalReadCalls(), readsAfterFirst) << "negative result must not rescan";
}

// A size that passes the one-glyph CJK probe but carries almost no real text
// must not be wired to any slot: unsupported characters would stay on the
// Latin UI font and render as replacement diamonds.
TEST_F(SdFontUiFallbackTest, PartialCjkCoverageDoesNotClaimUnsupportedCatalogGlyphs) {
  installFamily("PartialCJK", {12}, /*withCjk=*/true, /*includeCatalogMetadata=*/false);
  resetSettings("PartialCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  const int tofuWidth = renderer.getTextWidth(UI_12_FONT_ID, kCjkTitle);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
  EXPECT_FALSE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
  EXPECT_EQ(renderer.getTextWidth(UI_12_FONT_ID, kCjkTitle), tofuWidth);
}

TEST_F(SdFontUiFallbackTest, MissingFamilyLeavesBuiltInUiFontUsable) {
  sdFontSystem.begin(renderer);
  resetSettings("NotInstalled", 12);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
  EXPECT_NE(renderer.getTextWidth(UI_12_FONT_ID, "ABC 123"), 0);
}

TEST_F(SdFontUiFallbackTest, CorruptFamilyFileDegradesWithoutRegisteringFallback) {
  host_fs::put("/fonts/Broken/Broken_12.cpfont", {0x00, 0x01, 0x02, 0x03});
  resetSettings("Broken", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
  EXPECT_NE(renderer.getTextWidth(UI_12_FONT_ID, "ABC 123"), 0);
}

// Idempotence and re-registration across release cycles (catalog exit/re-entry).
TEST_F(SdFontUiFallbackTest, EnsureUiFallbacksIsIdempotentAndReRegistersAfterRelease) {
  installFamily("WenKaiCJK", {10, 12}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);
  ASSERT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));

  const size_t readsAfterFirst = host_fs::totalReadCalls();
  sdFontSystem.ensureUiFallbacks(renderer);
  EXPECT_EQ(host_fs::totalReadCalls(), readsAfterFirst) << "second call must not reload fonts";
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));

  // A release (e.g. another network phase) drops the fonts again; recovery
  // must work without reloading the reader font.
  sdFontSystem.releaseLoadedFont(renderer);
  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
  sdFontSystem.ensureUiFallbacks(renderer);
  EXPECT_TRUE(cjkRoutedThroughSize(renderer, UI_12_FONT_ID, 12));
}

// With no SD family preference (built-in reader font), the catalog must not
// touch the SD card and routing stays on the built-in UI fonts.
TEST_F(SdFontUiFallbackTest, NoFamilyPreferenceLeavesRoutingUntouched) {
  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  const size_t readsBefore = host_fs::totalReadCalls();
  sdFontSystem.ensureUiFallbacks(renderer);
  EXPECT_EQ(host_fs::totalReadCalls(), readsBefore);
  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
}

TEST_F(SdFontUiFallbackTest, ClearingUiFamilyPreferenceClearsPreviousFallbacks) {
  installFamily("WenKaiCJK", {12}, /*withCjk=*/true);
  sdFontSystem.begin(renderer);
  resetSettings("WenKaiCJK", 12);
  sdFontSystem.ensureUiFallbacks(renderer);
  ASSERT_TRUE(renderer.hasFallbackFont(UI_12_FONT_ID));

  resetSettings("", 12);
  sdFontSystem.ensureUiFallbacks(renderer);

  EXPECT_FALSE(renderer.hasFallbackFont(UI_12_FONT_ID));
}

// Ellipsis truncation of a long CJK title must measure with the resolved
// fallback font and never split a UTF-8 sequence.
TEST_F(SdFontUiFallbackTest, TruncatedCjkTextStaysValidUtf8ThroughFallback) {
  installFamily("WenKaiCJK", {10, 12}, /*withCjk=*/true);
  resetSettings("WenKaiCJK", 12);

  sdFontSystem.begin(renderer);
  sdFontSystem.releaseForNetwork(renderer);
  sdFontSystem.ensureUiFallbacks(renderer);

  std::string longTitle;
  for (int i = 0; i < 40; ++i) longTitle += "\xe6\x8c\xbd\xe6\x95\x91\xe8\xae\xa1\xe5\x88\x92";  // 挽救计划
  const std::string truncated = renderer.truncatedText(UI_12_FONT_ID, longTitle.c_str(), 120);
  const std::string ellipsis = "\xe2\x80\xa6";  // …
  ASSERT_GE(truncated.size(), ellipsis.size());
  EXPECT_EQ(truncated.substr(truncated.size() - ellipsis.size()), ellipsis);
  EXPECT_TRUE(longTitle.starts_with(truncated.substr(0, truncated.size() - ellipsis.size())));
  // Walk the result with the production decoder: it must consume the whole
  // string without a dangling partial sequence.
  const char* cursor = truncated.c_str();
  const char* const start = cursor;
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&cursor))) != 0) {
  }
  EXPECT_EQ(cursor - start, static_cast<std::ptrdiff_t>(truncated.size())) << "truncation split a UTF-8 sequence";
  // And the truncated text fits the requested width in the resolved font.
  EXPECT_LE(renderer.getTextWidth(UI_12_FONT_ID, truncated.c_str()), 120);
}
