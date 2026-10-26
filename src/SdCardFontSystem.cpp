#include "SdCardFontSystem.h"
#if CROSSINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#include <ScalableBuiltins.h>
#endif

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MemoryBudget.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#if CROSSINK_SCALABLE_FONTS
#include "TtfRenderProfileStore.h"
#endif
#include "fontIds.h"
#include "util/CjkDiag.h"

namespace {
struct UiFontSize {
  int fontId;
  uint8_t pointSize;
};

constexpr UiFontSize kUiFallbackOrder[] = {
    {UI_12_FONT_ID, 12},
    {UI_10_FONT_ID, 10},
    {SMALL_FONT_ID, 8},
};

uint8_t cjkCoverageScore(const EpdFontFamily& family) {
  uint8_t score = 0;
  for (const uint32_t cp : SdFontCjkCoverage::kSample) {
    if (family.hasCodepoint(cp)) ++score;
  }
  return score;
}

bool fontIdHasCjkCoverage(const GfxRenderer& renderer, const int fontId) {
  const auto it = renderer.getFontMap().find(fontId);
  return it != renderer.getFontMap().end() && cjkCoverageScore(it->second) >= SdFontCjkCoverage::kMinScore;
}

uint8_t fontIdCjkCoverageScore(const GfxRenderer& renderer, const int fontId) {
  const auto it = renderer.getFontMap().find(fontId);
  return it == renderer.getFontMap().end() ? 0 : cjkCoverageScore(it->second);
}

enum class FontFileSelection : uint8_t { Closest, Exact };

// This is a cold setup path, not a render loop. The 320-byte stack footprint
// (path and filename) replace a heap-allocated whole-font catalog
// during every dictionary swap, avoiding persistent fragmentation on the C3.
bool findInstalledFontFile(const char* familyName, const uint8_t targetPointSize, const FontFileSelection selection,
                           char* path, const size_t pathSize, uint8_t& selectedPointSize) {
  if (!familyName || familyName[0] == '\0' || !path || pathSize == 0) return false;

  const char* root = SdCardFontRegistry::findFamilyRoot(familyName);
  if (!root) return false;
  const int directoryLength = std::snprintf(path, pathSize, "%s/%s", root, familyName);
  if (directoryLength <= 0 || static_cast<size_t>(directoryLength) >= pathSize) return false;

  HalFile dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;

  uint8_t closestSize = 0;
  uint8_t closestDiff = UINT8_MAX;
  char filename[128] = {};
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0) continue;

    if (selection == FontFileSelection::Closest) {
      const uint8_t diff = pointSize > targetPointSize ? pointSize - targetPointSize : targetPointSize - pointSize;
      if (closestDiff == UINT8_MAX || diff < closestDiff || (diff == closestDiff && pointSize < closestSize)) {
        closestSize = pointSize;
        closestDiff = diff;
      }
    }
  }
  dir.close();

  if (selection == FontFileSelection::Closest) {
    selectedPointSize = closestSize;
  } else if (selection == FontFileSelection::Exact) {
    selectedPointSize = targetPointSize;
  }

  if (selectedPointSize == 0) return false;
  // Scan once more to preserve the exact file name rather than assuming the
  // file base name matches the directory name.
  dir = Storage.open(path);
  if (!dir || !dir.isDirectory()) return false;
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (!isDirectory) entry.getName(filename, sizeof(filename));
    entry.close();
    if (isDirectory) continue;

    uint8_t pointSize = 0;
    uint8_t style = 0;
    if (!SdCardFontRegistry::parseFilename(filename, pointSize, style) || style != 0 ||
        pointSize != selectedPointSize) {
      continue;
    }
    const int pathLength = std::snprintf(path, pathSize, "%s/%s/%s", root, familyName, filename);
    dir.close();
    return pathLength > 0 && static_cast<size_t>(pathLength) < pathSize;
  }
  dir.close();
  return false;
}

}  // namespace

void SdCardFontSystem::begin(GfxRenderer& renderer) {
  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t pointSize) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, pointSize);
  };
  SETTINGS.sdFontResolverCtx = this;

  if (SETTINGS.sdFontFamilyName[0] == '\0') {
    LOG_DBG("SDFS", "SD font resolver ready; discovery deferred until requested");
    return;
  }

#if CROSSINK_SCALABLE_FONTS
  LOG_DBG("SDFS", "SD font resolver ready; selected font load deferred until requested");
#else
  // C3 bitmap fonts retain their existing load timing and memory budget.
  ensureLoaded(renderer);
  releaseRegistry();
#endif
}

int SdCardFontSystem::ensureBuiltInReaderFont(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ensureScalableBuiltinFamily(renderer, SETTINGS.fontFamily == CrossPointSettings::BITTER ? 1 : 0);
#else
  (void)renderer;
#endif
  return SETTINGS.getBuiltInReaderFontId();
}

void SdCardFontSystem::persistSettingsChange() const {
  if (settingsPersistenceCallback_) {
    settingsPersistenceCallback_(settingsPersistenceContext_);
  } else {
    SETTINGS.saveToFile();
  }
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  bool registryWasDirty = registryDirty_.load(std::memory_order_acquire) || fontReloadPending_ ||
                          loadedRegistryRevision_ != registry_.revision();

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  uint8_t targetPointSize = SETTINGS.getSdFontTargetPointSize();

  if (wantedFamily[0] == '\0') {
    if (manager_.hasAnyLoadedFont()) {
      manager_.unloadAll(renderer);
      loadedFontPointSize_ = 0;
    }
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
    return;
  }

  if (!registryWasDirty && !manager_.hasTemporaryScalableFamily() && currentFamily == wantedFamily &&
      loadedFontPointSize_ == targetPointSize && SETTINGS.legacySdFontSizeStep == UINT8_MAX) {
    return;
  }

  ensureRegistry();
  if (registry_.lastDiscoveryFailed()) {
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
    return;
  }
  registryWasDirty = registryWasDirty || fontReloadPending_ || loadedRegistryRevision_ != registry_.revision();

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && !family->ensureDetails()) {
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
    return;
  }
  if (family && SETTINGS.legacySdFontSizeStep != UINT8_MAX) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) {
      const uint8_t step = std::min<uint8_t>(SETTINGS.legacySdFontSizeStep, sizes.size() - 1);
      targetPointSize = sizes[step];
      SETTINGS.readerFontPointSize = targetPointSize;
      SETTINGS.legacySdFontSizeStep = UINT8_MAX;
      persistSettingsChange();
      LOG_INF("SDFS", "Migrated SD font size to %u pt", targetPointSize);
    }
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      persistSettingsChange();
#if CROSSINK_SCALABLE_FONTS
      ensureBuiltInReaderFont(renderer);
#endif
      return;
    }
    const auto* wantedFile = family->findClosestFile(targetPointSize);
    uint8_t wantedPt = family->isScalable() ? targetPointSize : (wantedFile ? wantedFile->pointSize : 0);
    if (!registryWasDirty && !manager_.hasTemporaryScalableFamily() && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (target %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            targetPointSize, registryWasDirty ? " [registry dirty]" : "");
  }

  if (manager_.hasAnyLoadedFont() && (!family || !family->isScalable() || !familyMatches || registryWasDirty)) {
    manager_.unloadAll(renderer);
  }

  if (family) {
#if CROSSINK_SCALABLE_FONTS
    const auto options = family->isScalable() ? ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(wantedFamily))
                                              : freeink::font::FtFont::RenderOptions{};
    const bool loaded = family->isScalable() ? manager_.loadFamilyClosest(*family, renderer, targetPointSize, options)
                                             : manager_.loadFamilyClosest(*family, renderer, targetPointSize);
#else
    const bool loaded = manager_.loadFamilyClosest(*family, renderer, targetPointSize);
#endif
    if (loaded) {
      loadedFontPointSize_ = targetPointSize;
      fontReloadPending_ = false;
      loadedRegistryRevision_ = registry_.revision();
      setupUiFallbacks(renderer);
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (preserving selection)", wantedFamily);
#if CROSSINK_SCALABLE_FONTS
      ensureBuiltInReaderFont(renderer);
#endif
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    persistSettingsChange();
#if CROSSINK_SCALABLE_FONTS
    ensureBuiltInReaderFont(renderer);
#endif
  }
}

bool SdCardFontSystem::isScalableFamily(const char* familyName) {
#if CROSSINK_SCALABLE_FONTS
  if (!familyName || !familyName[0]) return false;
  ensureRegistry();
  const auto* family = registry_.findFamily(familyName);
  return family && family->ensureDetails() && family->isScalable();
#else
  (void)familyName;
  return false;
#endif
}

bool SdCardFontSystem::fontUsesMonochromeRaster(const GfxRenderer& renderer, const int fontId,
                                                const char* familyName) const {
#if CROSSINK_SCALABLE_FONTS
  return renderer.isSdCardFont(fontId) && hasResidentScalableFamily(familyName) &&
         TTF_RENDER_PROFILES.profileFor(familyName).raster != 0;
#else
  (void)renderer;
  (void)fontId;
  (void)familyName;
  return false;
#endif
}

bool SdCardFontSystem::reloadActiveScalableFamily(GfxRenderer& renderer, const char* familyName) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
  if (!hasResidentScalableFamily(familyName)) return false;
  const auto options = ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(familyName));
  if (!manager_.setScalableRenderOptions(renderer, options)) return false;
  setupUiFallbacks(renderer);
  scalableRenderOptionsGeneration_++;
  return true;
#else
  (void)renderer;
  (void)familyName;
  return false;
#endif
}

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  // Extra UI fallback sizes load without a family name set, so the family
  // name alone cannot decide whether anything is resident.
  if (!manager_.hasAnyLoadedFont()) return;

  const std::string familyName = manager_.currentFamilyName();
  (void)familyName;
  manager_.unloadAll(renderer);
  loadedFontPointSize_ = 0;
  LOG_DBG("SDFS", "Released SD card fonts before low-memory operation: %s",
          familyName.empty() ? "(UI sizes only)" : familyName.c_str());
}

void SdCardFontSystem::ensureRegistry() {
  const bool dirty = registryDirty_.exchange(false, std::memory_order_acq_rel);
  if (dirty) fontReloadPending_ = true;
  if (registryLoaded_ && !dirty && !registry_.needsRefresh()) return;
  if (dirty) LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
  registry_.loadNames();
  if (registry_.lastDiscoveryFailed()) {
    LOG_ERR("SDFS", "SD font registry scan failed (free=%u maxAlloc=%u)", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    registryDirty_.store(true, std::memory_order_release);
    return;
  }
  registryLoaded_ = true;
}

void SdCardFontSystem::releaseRegistry() {
  if (!registryLoaded_) return;
  LOG_DBG("SDFS", "Releasing SD font catalog (%d families)", registry_.getFamilyCount());
  registry_.clear();
  registryLoaded_ = false;
}

void SdCardFontSystem::releaseForNetwork(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  releaseLoadedFont(renderer);

  releaseRegistry();
  registryDirty_.store(true, std::memory_order_release);
}

void SdCardFontSystem::ensureUiFallbacks(GfxRenderer& renderer) {
  const char* wantedFamily = SETTINGS.getEffectiveUiFontFamilyName();
  if (!wantedFamily || wantedFamily[0] == '\0') {
    if (uiFallbacksFamily_.empty() && !uiFallbacksRegistered_ && !renderer.hasFallbackFont(UI_12_FONT_ID)) return;
    if (manager_.currentFamilyName().empty() && manager_.hasAnyLoadedFont()) {
      manager_.unloadAll(renderer);
      loadedFontPointSize_ = 0;
    } else {
      renderer.clearFallbackFonts();
    }
    uiFallbacksFamily_.clear();
    uiFallbacksRegistered_ = false;
    uiFallbacksRegistryRevision_ = registry_.revision();
    return;
  }

  const bool registryMayHaveChanged = registryDirty_.load(std::memory_order_acquire) || registry_.needsRefresh() ||
                                      uiFallbacksRegistryRevision_ != registry_.revision();
  const bool familyChanged = uiFallbacksFamily_ != wantedFamily;
  const bool registryCurrent = !registryMayHaveChanged;
  if (uiFallbacksFamily_ == wantedFamily && registryCurrent &&
      (!uiFallbacksRegistered_ || renderer.hasFallbackFont(UI_12_FONT_ID))) {
    return;
  }

  CjkDiag::log(
      "ensureUiFallbacks: family='%s' cachedFamily='%s' familyChanged=%d registryDirty=%d needsRefresh=%d "
      "revisionChanged=%d",
      wantedFamily, uiFallbacksFamily_.c_str(), familyChanged, registryDirty_.load(std::memory_order_acquire),
      registry_.needsRefresh(), uiFallbacksRegistryRevision_ != registry_.revision());

  if ((familyChanged || registryMayHaveChanged) && manager_.hasAnyLoadedFont()) {
    manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
  }

  ensureRegistry();
  if (registry_.lastDiscoveryFailed()) {
    CjkDiag::log("ensureUiFallbacks: registry discovery FAILED");
    return;
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family && !family->ensureDetails()) family = nullptr;

  uiFallbacksFamily_ = wantedFamily;
  uiFallbacksRegistryRevision_ = registry_.revision();
  uiFallbacksRegistered_ = false;
  if (!family) {
    CjkDiag::log("ensureUiFallbacks: family not installed: %s", wantedFamily);
    return;
  }
  {
    char sizes[96];
    size_t offset = 0;
    const auto available = family->availableSizes();
    for (size_t i = 0; i < available.size() && offset + 6 < sizeof(sizes); ++i) {
      offset += static_cast<size_t>(
          snprintf(sizes + offset, sizeof(sizes) - offset, "%s%u", i ? "," : "", static_cast<unsigned>(available[i])));
    }
    CjkDiag::log("ensureUiFallbacks: family='%s' scalable=%d sizes=[%s]", wantedFamily, family->isScalable(), sizes);
  }
#if CROSSINK_SCALABLE_FONTS
  if (family->isScalable() && !manager_.hasResidentScalableFamily(wantedFamily)) {
    // Registering TTF UI sizes needs the resident faces; loading a TTF family
    // just for UI text is not worth the heap during network-heavy screens.
    CjkDiag::log("ensureUiFallbacks: TTF family not resident: %s", wantedFamily);
    return;
  }
#endif
  uiFallbacksRegistered_ = registerUiFallbackSizes(renderer, *family, 0);
  CjkDiag::log("ensureUiFallbacks: registered=%d family='%s'", uiFallbacksRegistered_ ? 1 : 0, wantedFamily);
  if (!uiFallbacksRegistered_) {
    LOG_DBG("SDFS", "%s provides no CJK UI fallback sizes", wantedFamily);
  }
}
bool SdCardFontSystem::registerUiFallbackSizes(GfxRenderer& renderer, const SdCardFontFamilyInfo& family,
                                               const int seedFontId) {
  int cjkFontId = seedFontId != 0 && fontIdHasCjkCoverage(renderer, seedFontId) ? seedFontId : 0;
  bool registeredAny = false;
  for (const auto& ui : kUiFallbackOrder) {
    // Only wire a slot to a size that covers everyday CJK: a subset small
    // size routes nothing and every Chinese character becomes a replacement
    // diamond on the Latin UI font.
    int candidate = manager_.loadFamilyExtraSize(family, renderer, ui.pointSize);
    if (candidate != 0 && !fontIdHasCjkCoverage(renderer, candidate)) {
      CjkDiag::log("registerUiFallbacks: slot %u pt exact load id=%d score=%u REJECTED (sparse)",
                   static_cast<unsigned>(ui.pointSize), candidate, fontIdCjkCoverageScore(renderer, candidate));
      candidate = 0;
    }
    if (candidate == 0) {
      candidate = manager_.loadFamilyExtraClosest(family, renderer, ui.pointSize);
      if (candidate != 0 && !fontIdHasCjkCoverage(renderer, candidate)) {
        CjkDiag::log("registerUiFallbacks: slot %u pt nearest load id=%d score=%u REJECTED (sparse)",
                     static_cast<unsigned>(ui.pointSize), candidate, fontIdCjkCoverageScore(renderer, candidate));
        candidate = 0;
      }
    }
    if (candidate == 0) candidate = loadCjkCoveringExtraSize(family, renderer, ui.pointSize);
    if (candidate == 0) candidate = cjkFontId;
    if (candidate == 0) {
      CjkDiag::log("registerUiFallbacks: slot %u pt has no covering size", static_cast<unsigned>(ui.pointSize));
      continue;
    }
    if (cjkFontId == 0) cjkFontId = candidate;
    renderer.setFallbackFont(ui.fontId, candidate);
    CjkDiag::log("registerUiFallbacks: slot fontId=%d <- %u pt file id=%d score=%u", ui.fontId,
                 static_cast<unsigned>(ui.pointSize), candidate, fontIdCjkCoverageScore(renderer, candidate));
    registeredAny = true;
  }
  if (!registeredAny) {
    LOG_DBG("SDFS", "%s provides no CJK UI fallback sizes", family.name.c_str());
    manager_.unloadAll(renderer);
    return false;
  }
  return true;
}

int SdCardFontSystem::loadCjkCoveringExtraSize(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                               const uint8_t targetPointSize) {
#if CROSSINK_SCALABLE_FONTS
  if (family.isScalable()) return 0;  // every TTF size rasterizes from the same resident faces
#endif
  std::vector<uint8_t> sizes = family.availableSizes();
  while (!sizes.empty()) {
    const auto distance = [targetPointSize](const uint8_t size) {
      return size > targetPointSize ? size - targetPointSize : targetPointSize - size;
    };
    size_t nearest = 0;
    for (size_t i = 1; i < sizes.size(); ++i) {
      if (distance(sizes[i]) < distance(sizes[nearest])) nearest = i;
    }
    const uint8_t size = sizes[nearest];
    sizes.erase(sizes.begin() + static_cast<long>(nearest));
    const int fontId = manager_.loadFamilyExtraSize(family, renderer, size);
    if (fontId != 0 && fontIdHasCjkCoverage(renderer, fontId)) {
      CjkDiag::log("loadCjkCoveringExtraSize: %u pt id=%d score=%u ACCEPTED for %u pt slot",
                   static_cast<unsigned>(size), fontId, fontIdCjkCoverageScore(renderer, fontId),
                   static_cast<unsigned>(targetPointSize));
      return fontId;
    }
    CjkDiag::log("loadCjkCoveringExtraSize: %u pt id=%d score=%u rejected for %u pt slot", static_cast<unsigned>(size),
                 fontId, fontIdCjkCoverageScore(renderer, fontId), static_cast<unsigned>(targetPointSize));
  }
  return 0;
}

void SdCardFontSystem::setupUiFallbacks(GfxRenderer& renderer) {
  const std::string& familyName = manager_.currentFamilyName();
  if (familyName.empty()) return;

  const auto* family = registry_.findFamily(familyName);
  if (!family) return;

  const int seedFontId = manager_.getFontId(familyName);
  const auto readerIt = renderer.getFontMap().find(seedFontId);
  if (readerIt == renderer.getFontMap().end()) return;
  if (!fontIdHasCjkCoverage(renderer, seedFontId)) {
    LOG_DBG("SDFS", "%s has no CJK coverage - skipping UI fallback sizes", familyName.c_str());
    return;
  }

  CjkDiag::log("setupUiFallbacks: reader family='%s' seed id=%d score=%u", familyName.c_str(), seedFontId,
               fontIdCjkCoverageScore(renderer, seedFontId));
  uiFallbacksRegistered_ = registerUiFallbackSizes(renderer, *family, seedFontId);
  uiFallbacksFamily_ = familyName;
  uiFallbacksRegistryRevision_ = registry_.revision();
}

void SdCardFontSystem::setupUiFallbacksDirect(GfxRenderer& renderer, const char* familyName) {
  if (!familyName || familyName[0] == '\0') return;

  const int seedFontId = manager_.getFontId(manager_.currentFamilyName());
  const auto readerIt = renderer.getFontMap().find(seedFontId);
  if (readerIt == renderer.getFontMap().end()) return;
  if (!fontIdHasCjkCoverage(renderer, seedFontId)) return;

  int cjkFontId = seedFontId;
  for (const auto& ui : kUiFallbackOrder) {
    char path[160] = {};
    uint8_t pointSize = 0;
    int sdFontId =
        findInstalledFontFile(familyName, ui.pointSize, FontFileSelection::Closest, path, sizeof(path), pointSize)
            ? manager_.loadFamilyExtraFile(path, familyName, pointSize, renderer)
            : 0;
    if (sdFontId != 0 && !fontIdHasCjkCoverage(renderer, sdFontId)) sdFontId = 0;
    const int candidate = sdFontId != 0 ? sdFontId : cjkFontId;
    if (candidate == 0) continue;
    cjkFontId = candidate;
    renderer.setFallbackFont(ui.fontId, candidate);
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*pointSize*/) const {
  // The manager loads exactly one size (closest to the selected point size), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

bool SdCardFontSystem::changeReaderFontSize(const bool larger, const FontSizeStepMode mode) {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    refreshIfDirty();
    if (registry_.lastDiscoveryFailed()) return false;
    const auto* family = registry_.findFamily(SETTINGS.sdFontFamilyName);
    if (family) {
      if (!family->ensureDetails()) return false;
      const auto sizes = family->availableSizes();
      if (changeReaderFontSizeStep(sizes.data(), sizes.size(), SETTINGS.readerFontPointSize, larger, mode)) return true;
      if (sizes.size() > 0) return false;
    }
  }

  return SETTINGS.changeReaderFontSize(larger, mode);
}

uint8_t SdCardFontSystem::resolveLegacySizeStep(const char* familyName, const uint8_t sizeStep) {
  ensureRegistry();
  const auto* family = familyName ? registry_.findFamily(familyName) : nullptr;
  if (family) {
    const auto sizes = family->availableSizes();
    if (!sizes.empty()) return sizes[std::min<uint8_t>(sizeStep, sizes.size() - 1)];
  }
  return CrossPointSettings::getSdFontRangePointSize(SETTINGS.sdFontSizeRange, sizeStep);
}

DictionaryFontActivation SdCardFontSystem::activateDictionaryFont(GfxRenderer& renderer, const char* familyName,
                                                                  uint8_t targetPointSize) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  // A non-zero size with no dedicated family means "use the reader's installed
  // family at this size". This keeps the setting useful when the same custom
  // family is wanted for reading and definitions without keeping two families
  // resident.
  if ((!familyName || familyName[0] == '\0') && targetPointSize != 0 && SETTINGS.sdFontFamilyName[0] != '\0') {
    familyName = SETTINGS.sdFontFamilyName;
  }
  if (!familyName || familyName[0] == '\0') {
    return {restoreReaderFont(renderer), false};
  }

#if CROSSINK_SCALABLE_FONTS
  ensureRegistry();
  if (const auto* family = registry_.findFamily(familyName); family && family->isScalable()) {
    const uint8_t size = targetPointSize ? targetPointSize : SETTINGS.getSdFontTargetPointSize();
    const auto options = ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(familyName));
    if (manager_.loadDictionaryFamily(*family, renderer, size, options)) return {manager_.getFontId(familyName), true};
    return {restoreReaderFont(renderer), false};
  }
#endif
  MemoryBudget::logHeapShape("dict.font_before_activate");
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  // Prefer the actual loaded reader-file size. Built-in readers have no SD
  // file, so use their effective physical point size instead.
  if (targetPointSize == 0) {
    targetPointSize = manager_.currentPointSize() != 0 ? manager_.currentPointSize()
                                                       : closestBuiltinReaderPointSize(SETTINGS.readerFontPointSize);
  }
  if (!findInstalledFontFile(familyName, targetPointSize, FontFileSelection::Closest, path, sizeof(path),
                             selectedPointSize)) {
    LOG_DBG("SDFS", "Dictionary font not found on card: %s", familyName);
    const char* globalFamilyName = SETTINGS.dictionarySdFontFamilyName;
    if (globalFamilyName[0] != '\0' && std::strcmp(familyName, globalFamilyName) != 0) {
      LOG_DBG("SDFS", "Using global dictionary font while per-book font is unavailable: %s", globalFamilyName);
      return activateDictionaryFont(renderer, globalFamilyName, SETTINGS.dictionaryFontPointSize);
    }
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_reader_fallback");
    return {readerFontId, false};
  }

  if (manager_.currentFamilyName() == familyName && manager_.currentPointSize() == selectedPointSize) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    MemoryBudget::logHeapShape("dict.font_reused_reader");
    return {fontId, true};
  }

  // A reader SD font can retain page glyphs, kerning, and advance tables after
  // a long reading session. They are disposable at this handoff: keeping them
  // through the headroom check makes a dictionary font appear unavailable until
  // its book cache is deleted or the heap happens to be less fragmented.
  const int activeReaderFontId = SETTINGS.getReaderFontId();
  const auto beforeCacheRelease = MemoryBudget::snapshot();
  if (renderer.releaseSdCardFontForLowMemory(activeReaderFontId)) {
    const auto afterCacheRelease = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader SD-font caches before dictionary swap: free=%u->%u maxAlloc=%u->%u",
            beforeCacheRelease.freeHeap, afterCacheRelease.freeHeap, beforeCacheRelease.maxAllocHeap,
            afterCacheRelease.maxAllocHeap);
  }

  auto heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    // The reader family itself is also disposable for a dictionary swap. Retry
    // after releasing it so its font data does not cause a false low-memory
    // fallback.
    const auto beforeReaderUnload = heap;
    if (manager_.hasAnyLoadedFont()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    heap = MemoryBudget::snapshot();
    LOG_DBG("SDFS", "Released reader font before dictionary swap retry: free=%u->%u maxAlloc=%u->%u",
            beforeReaderUnload.freeHeap, heap.freeHeap, beforeReaderUnload.maxAllocHeap, heap.maxAllocHeap);
  }
  if (!MemoryBudget::hasHeapForDictionarySdFont(heap)) {
    LOG_ERR("SDFS", "Low heap for dictionary font swap (%u free, %u max alloc, need %u/%u); using reader font",
            heap.freeHeap, heap.maxAllocHeap, MemoryBudget::DICTIONARY_SD_FONT_MIN_FREE,
            MemoryBudget::DICTIONARY_SD_FONT_MIN_MAX_ALLOC);
    const int readerFontId = restoreReaderFont(renderer);
    MemoryBudget::logHeapShape("dict.font_heap_fallback");
    return {readerFontId, false};
  }

  // unloadAll() also drops optional CJK UI sizes before the dictionary file is
  // allocated, so both families are never resident at once.
  if (manager_.hasAnyLoadedFont()) {
    manager_.unloadAll(renderer);
  }
  loadedFontPointSize_ = 0;

  if (manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
    const int fontId = manager_.getFontId(manager_.currentFamilyName());
    LOG_DBG("SDFS", "Activated dictionary font %s at %u pt", familyName, manager_.currentPointSize());
    MemoryBudget::logHeapShape("dict.font_after_activate");
    return {fontId, true};
  }

  LOG_ERR("SDFS", "Failed to load dictionary font %s; restoring reader font", familyName);
  const int readerFontId = restoreReaderFont(renderer);
  MemoryBudget::logHeapShape("dict.font_reader_fallback");
  return {readerFontId, false};
}

int SdCardFontSystem::restoreReaderFont(GfxRenderer& renderer) {
#if CROSSINK_SCALABLE_FONTS
  ScalableFontAccess access;
#endif
  const char* familyName = SETTINGS.sdFontFamilyName;
  if (!familyName || familyName[0] == '\0') {
    if (manager_.hasAnyLoadedFont()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return ensureBuiltInReaderFont(renderer);
  }

#if CROSSINK_SCALABLE_FONTS
  ensureRegistry();
  if (const auto* family = registry_.findFamily(familyName); family && family->isScalable()) {
    const auto options = ttfRenderOptions(TTF_RENDER_PROFILES.profileFor(familyName));
    if (manager_.loadFamilyClosest(*family, renderer, SETTINGS.getSdFontTargetPointSize(), options)) {
      loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
      setupUiFallbacks(renderer);
      return manager_.getFontId(familyName);
    }
    return ensureBuiltInReaderFont(renderer);
  }
#endif
  char path[160] = {};
  uint8_t selectedPointSize = 0;
  if (!findInstalledFontFile(familyName, SETTINGS.getSdFontTargetPointSize(), FontFileSelection::Closest, path,
                             sizeof(path), selectedPointSize)) {
    LOG_ERR("SDFS", "Reader font unavailable while restoring: %s", familyName);
    if (manager_.hasAnyLoadedFont()) manager_.unloadAll(renderer);
    loadedFontPointSize_ = 0;
    MemoryBudget::logHeapShape("dict.font_after_restore");
    return ensureBuiltInReaderFont(renderer);
  }

  if (manager_.currentFamilyName() != familyName || manager_.currentPointSize() != selectedPointSize) {
    if (manager_.hasAnyLoadedFont()) manager_.unloadAll(renderer);
    if (!manager_.loadFamilyFile(path, familyName, selectedPointSize, renderer)) {
      LOG_ERR("SDFS", "Failed to restore reader font: %s", familyName);
      MemoryBudget::logHeapShape("dict.font_after_restore");
      return ensureBuiltInReaderFont(renderer);
    }
    loadedFontPointSize_ = SETTINGS.getSdFontTargetPointSize();
    setupUiFallbacksDirect(renderer, familyName);
  }

  const int fontId = manager_.getFontId(manager_.currentFamilyName());
  MemoryBudget::logHeapShape("dict.font_after_restore");
  return fontId != 0 ? fontId : ensureBuiltInReaderFont(renderer);
}

void SdCardFontSystem::markRegistryDirtyForPath(const char* path) {
  if (!path) return;
  for (const char* root : {SdCardFontRegistry::FONTS_DIR_HIDDEN, SdCardFontRegistry::FONTS_DIR_VISIBLE}) {
    const size_t length = std::strlen(root);
    if (strncasecmp(path, root, length) == 0 && (path[length] == '/' || path[length] == '\0')) {
      markRegistryDirty();
      return;
    }
  }
}
