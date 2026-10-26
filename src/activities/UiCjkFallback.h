#pragma once

#include <GfxRenderer.h>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "activities/RenderLock.h"
#include "fontIds.h"

// UI CJK fallback glue for non-reader screens. While a book is open the reader
// registers these fallbacks itself (SdCardFontSystem::setupUiFallbacks); every
// other screen that shows user text — Library, home, file browser, bookmarks,
// the BookOrbit catalog — must ask for them, because any network-bearing screen
// releases the SD fonts (SdCardFontSystem::releaseForNetwork/releaseLoadedFont)
// and that release also clears the renderer's fallback map.
namespace UiCjk {

// True when a UI family preference exists but the renderer currently has no
// fallback registered for it. Cheap enough to call every loop tick; it does no
// I/O and takes no lock.
inline bool fallbackMissing(GfxRenderer& renderer) {
  const char* family = SETTINGS.getEffectiveUiFontFamilyName();
  return family != nullptr && family[0] != '\0' && !renderer.hasFallbackFont(UI_12_FONT_ID);
}

// Re-register the UI CJK fallbacks when they went missing. Call from the
// main/loop task only — never from render() and never while already holding
// the render lock (callers that hold one use fallbackMissing() plus a direct
// sdFontSystem.ensureUiFallbacks() instead). Returns true when fallbacks are
// in place after the call, so the caller can requestUpdate() after healing a
// frame that was drawn without them.
inline bool ensureFallbacks(GfxRenderer& renderer) {
  if (!fallbackMissing(renderer)) return false;
  RenderLock lock;
  sdFontSystem.ensureUiFallbacks(renderer);
  return !fallbackMissing(renderer);
}

}  // namespace UiCjk
