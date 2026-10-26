#pragma once

#include <cstdint>
#include <map>

// Diagnostics for the UI CJK fallback, built for USB-locked devices: appends
// decision logs to /cjk_debug.log and saves framebuffer screenshots (PBM) to
// /cjk_shots/ whenever a frame drew replacement diamonds. Everything compiles
// away unless CROSSINK_CJK_DIAG is set (diagnostic builds only).
namespace CjkDiag {

// Append one line to /cjk_debug.log (millis() prefix is added here).
void log(const char* format, ...) __attribute__((format(printf, 1, 2)));

// A codepoint the resolved font could not render, so it was drawn as the
// replacement diamond. `fontId` is the font the string actually used.
void noteMissingGlyph(int fontId, uint32_t codepoint);

// The input string itself contained U+FFFD: the text was already mangled
// before it reached the renderer (storage/transfer decoding, not a font gap).
void noteInputReplacement(int fontId);

// A CJK codepoint the primary font lacks that the registered fallback also
// lacks, so the string stayed on the primary (Latin) font.
void noteUnroutable(int primaryFontId, int fallbackFontId, uint32_t codepoint);

// Called from GfxRenderer::displayBuffer before the panel refresh. When
// replacement glyphs were drawn since the previous capture, writes the raw
// framebuffer as a PBM and flushes the counters plus the fallback map to the
// log. Rate-limited and capped per boot.
void onDisplayBuffer(const uint8_t* framebuffer, int widthBytes, int height, const std::map<int, int>& fallbackFonts);

}  // namespace CjkDiag

#if !CROSSINK_CJK_DIAG
namespace CjkDiag {
inline void log(const char*, ...) {}
inline void noteMissingGlyph(int, uint32_t) {}
inline void noteInputReplacement(int) {}
inline void noteUnroutable(int, int, uint32_t) {}
inline void onDisplayBuffer(const uint8_t*, int, int, const std::map<int, int>&) {}
}  // namespace CjkDiag
#endif
