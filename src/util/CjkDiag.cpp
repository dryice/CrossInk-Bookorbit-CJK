#include "CjkDiag.h"

#if CROSSINK_CJK_DIAG

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>

#include "AppVersion.h"
#include "CrossPointSettings.h"

namespace CjkDiag {
namespace {

constexpr char LOG_PATH[] = "/cjk_debug.log";
constexpr char SHOT_DIR[] = "/cjk_shots";
constexpr char SHOT_PATH_FORMAT[] = "/cjk_shots/shot_%02u.pbm";

// Runaway guards so a forgotten diagnostic build cannot fill the card.
constexpr size_t MAX_LOG_BYTES = 512u * 1024u;
constexpr unsigned MAX_SHOTS = 40;
constexpr unsigned long MIN_SHOT_INTERVAL_MS = 3000;
constexpr unsigned long MUTEX_TIMEOUT_MS = 500;

// Distinct codepoints remembered per table; repeats only bump the counter.
constexpr size_t MAX_NOTES = 64;

struct CpNote {
  int fontId;
  uint32_t codepoint;
  unsigned count;
};

SemaphoreHandle_t mutex = nullptr;
bool initialized = false;
unsigned shotCount = 0;
unsigned long lastShotMs = 0;

CpNote missingNotes[MAX_NOTES];
size_t missingNoteCount = 0;
CpNote unroutableNotes[MAX_NOTES];
size_t unroutableNoteCount = 0;
CpNote inputNotes[MAX_NOTES];
size_t inputNoteCount = 0;
bool countersChanged = false;

void remember(CpNote* table, size_t& count, const int fontId, const uint32_t codepoint) {
  for (size_t i = 0; i < count; ++i) {
    if (table[i].fontId == fontId && table[i].codepoint == codepoint) {
      table[i].count++;
      return;
    }
  }
  if (count < MAX_NOTES) table[count++] = {fontId, codepoint, 1};
  countersChanged = true;
}

// Caller holds `mutex` (or is single-threaded during lazy init).
void appendRaw(const char* line, const size_t length) {
  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("CJKDIAG", "Failed to open %s", LOG_PATH);
    return;
  }
  bool ok = true;
  if (file.size() >= MAX_LOG_BYTES) {
    static bool capReported = false;
    if (!capReported) {
      capReported = true;
      LOG_ERR("CJKDIAG", "%s hit the %u byte cap; no longer appending", LOG_PATH, static_cast<unsigned>(MAX_LOG_BYTES));
    }
    file.close();
    return;
  }
  const bool wrote = file.write(line, length) == length;
  const bool closed = file.close();
  ok = wrote && closed;
  if (!ok) LOG_ERR("CJKDIAG", "Failed to append to %s", LOG_PATH);
}

bool takeMutex() { return mutex != nullptr && xSemaphoreTake(mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) == pdTRUE; }

void ensureInit() {
  if (initialized) return;
  initialized = true;
  mutex = xSemaphoreCreateMutex();
  Storage.mkdir(SHOT_DIR);
  char header[192];
  int length =
      snprintf(header, sizeof(header), "==== cjk diag build version=%s sha=%s dirty=%s built=" __DATE__ " " __TIME__,
               AppVersion::version(), AppVersion::gitSha(), AppVersion::gitDirtyFlag());
  if (length > 0 && static_cast<size_t>(length) < sizeof(header)) {
    header[length++] = '\n';
    appendRaw(header, length);
  }
  char settings[192];
  length = snprintf(settings, sizeof(settings),
                    "settings: readerFamily='%s' uiFamily='%s' effectiveUi='%s' readerPt=%u uiScale=%u",
                    SETTINGS.sdFontFamilyName, SETTINGS.sdUiFontFamilyName, SETTINGS.getEffectiveUiFontFamilyName(),
                    SETTINGS.readerFontPointSize, SETTINGS.uiScale);
  if (length > 0 && static_cast<size_t>(length) < sizeof(settings)) {
    settings[length++] = '\n';
    appendRaw(settings, length);
  }
}

// The framebuffer stores black as a cleared bit; PBM wants black as a set bit,
// so invert each row chunk on the way out.
void writeInverted(HalFile& file, const uint8_t* source, const size_t length) {
  uint8_t chunk[256];
  size_t remaining = length;
  while (remaining > 0) {
    const size_t take = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    for (size_t i = 0; i < take; ++i) chunk[i] = ~source[i];
    if (file.write(chunk, take) != take) return;
    source += take;
    remaining -= take;
  }
}

void flushCountersLocked(const unsigned shotNumber, const int widthPixels, const int height,
                         const std::map<int, int>& fallbackFonts) {
  char line[160];
  int length = snprintf(line, sizeof(line), "--- shot %u: %dx%d px, fallback map:", shotNumber, widthPixels, height);
  if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
    line[length++] = '\n';
    appendRaw(line, length);
  }
  for (const auto& [primary, fallback] : fallbackFonts) {
    length = snprintf(line, sizeof(line), "fallback: %d -> %d", primary, fallback);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
      line[length++] = '\n';
      appendRaw(line, length);
    }
  }
  if (fallbackFonts.empty()) appendRaw("fallback: (none registered)\n", 27);

  for (size_t i = 0; i < missingNoteCount; ++i) {
    length = snprintf(line, sizeof(line), "missing: font=%d cp=U+%04X x%u", missingNotes[i].fontId,
                      static_cast<unsigned>(missingNotes[i].codepoint), missingNotes[i].count);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
      line[length++] = '\n';
      appendRaw(line, length);
    }
  }
  for (size_t i = 0; i < unroutableNoteCount; ++i) {
    length = snprintf(line, sizeof(line), "unroutable: font=%d cp=U+%04X x%u", unroutableNotes[i].fontId,
                      static_cast<unsigned>(unroutableNotes[i].codepoint), unroutableNotes[i].count);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
      line[length++] = '\n';
      appendRaw(line, length);
    }
  }
  for (size_t i = 0; i < inputNoteCount; ++i) {
    length = snprintf(line, sizeof(line), "input_fffd: font=%d x%u", inputNotes[i].fontId, inputNotes[i].count);
    if (length > 0 && static_cast<size_t>(length) < sizeof(line)) {
      line[length++] = '\n';
      appendRaw(line, length);
    }
  }
  missingNoteCount = unroutableNoteCount = inputNoteCount = 0;
}

}  // namespace

void log(const char* format, ...) {
  ensureInit();
  char line[224];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line) - 1, format, args);
  va_end(args);
  if (length <= 0) return;
  if (static_cast<size_t>(length) >= sizeof(line) - 1) length = sizeof(line) - 2;

  char stamped[256];
  length = snprintf(stamped, sizeof(stamped), "%lu %s\n", static_cast<unsigned long>(millis()), line);
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(stamped)) return;
  if (!takeMutex()) return;
  appendRaw(stamped, static_cast<size_t>(length));
  xSemaphoreGive(mutex);
}

void noteMissingGlyph(const int fontId, const uint32_t codepoint) {
  if (!takeMutex()) return;
  remember(missingNotes, missingNoteCount, fontId, codepoint);
  xSemaphoreGive(mutex);
}

void noteInputReplacement(const int fontId) {
  if (!takeMutex()) return;
  remember(inputNotes, inputNoteCount, fontId, 0);
  xSemaphoreGive(mutex);
}

void noteUnroutable(const int primaryFontId, const int fallbackFontId, const uint32_t codepoint) {
  (void)fallbackFontId;  // the consulted fallback is identified by the fallback map logged with each shot
  if (!takeMutex()) return;
  remember(unroutableNotes, unroutableNoteCount, primaryFontId, codepoint);
  xSemaphoreGive(mutex);
}

void onDisplayBuffer(const uint8_t* framebuffer, const int widthBytes, const int height,
                     const std::map<int, int>& fallbackFonts) {
  ensureInit();
  if (!takeMutex()) return;
  if (!countersChanged || shotCount >= MAX_SHOTS) {
    xSemaphoreGive(mutex);
    return;
  }
  const unsigned long now = millis();
  if (lastShotMs != 0 && now - lastShotMs < MIN_SHOT_INTERVAL_MS) {
    xSemaphoreGive(mutex);
    return;
  }
  countersChanged = false;
  lastShotMs = now;
  shotCount++;

  char path[40];
  if (snprintf(path, sizeof(path), SHOT_PATH_FORMAT, shotCount) <= 0) {
    xSemaphoreGive(mutex);
    return;
  }
  HalFile file = Storage.open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (file) {
    char header[32];
    const int headerLength = snprintf(header, sizeof(header), "P4\n%d %d\n", widthBytes * 8, height);
    if (headerLength > 0 && file.write(header, headerLength) == static_cast<size_t>(headerLength)) {
      writeInverted(file, framebuffer, static_cast<size_t>(widthBytes) * height);
    }
    if (!file.close()) LOG_ERR("CJKDIAG", "Failed to close %s", path);
  } else {
    LOG_ERR("CJKDIAG", "Failed to open %s for write", path);
  }
  flushCountersLocked(shotCount, widthBytes * 8, height, fallbackFonts);
  xSemaphoreGive(mutex);
}

}  // namespace CjkDiag

#endif  // CROSSINK_CJK_DIAG
