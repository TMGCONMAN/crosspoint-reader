#include "CoverReport.h"

#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstdio>

void reportCoverFailure(const std::string& bookPath, const int height, const char* what) {
  constexpr char REPORT_PATH[] = "/.crosspoint/cover-report.txt";
  constexpr size_t MAX_REPORT_BYTES = 48 * 1024;
  constexpr int MAX_REPORTS_PER_BOOT = 6;
  static int reports = 0;
  if (reports >= MAX_REPORTS_PER_BOOT) return;
  reports++;

  if (Storage.exists(REPORT_PATH)) {
    HalFile existing;
    if (Storage.openFileForRead("HOME", REPORT_PATH, existing)) {
      const bool tooLarge = existing.fileSize() > MAX_REPORT_BYTES;
      existing.close();
      if (tooLarge) Storage.remove(REPORT_PATH);
    }
  }
  HalFile file = Storage.open(REPORT_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) return;
  char header[320];
  const int len = snprintf(header, sizeof(header),
                           "=== %s: %s (height %d)\nFirmware %s\nInternal heap free %u, largest %u; PSRAM free %u\n"
                           "--- Last log lines:\n",
                           what, bookPath.c_str(), height, CROSSPOINT_VERSION,
                           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (len > 0) file.write(reinterpret_cast<const uint8_t*>(header), std::min<size_t>(len, sizeof(header) - 1));
  const std::string logs = getLastLogs();
  file.write(reinterpret_cast<const uint8_t*>(logs.data()), logs.size());
  file.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  file.close();
}
