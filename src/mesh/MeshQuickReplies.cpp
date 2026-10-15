#include "MeshQuickReplies.h"

#if CROSSPOINT_MESH

#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <cstdio>
#include <cstring>

namespace meshquick {

namespace {

constexpr const char* DEFAULTS[] = {"OK",
                                    "Yes",
                                    "No",
                                    "On my way",
                                    "I'm safe",
                                    "Can't talk now, will reply later",
                                    "Call me when you can",
                                    "Where are you?"};
constexpr char DIR_PATH[] = "/.crosspoint/mesh";

void writeDefaults() {
  if (!Storage.ensureDirectoryExists(DIR_PATH)) return;
  HalFile file;
  if (!Storage.openFileForWrite("MQUICK", FILE_PATH, file)) return;
  for (const char* reply : DEFAULTS) {
    file.write(reinterpret_cast<const uint8_t*>(reply), strlen(reply));
    file.write(reinterpret_cast<const uint8_t*>("\n"), 1);
  }
  file.close();
}

size_t useDefaults(char (*out)[REPLY_SIZE], size_t max) {
  size_t count = 0;
  for (const char* reply : DEFAULTS) {
    if (count >= max) break;
    snprintf(out[count++], REPLY_SIZE, "%s", reply);
  }
  return count;
}

}  // namespace

size_t load(char (*out)[REPLY_SIZE], size_t max) {
  if (max > MAX_REPLIES) max = MAX_REPLIES;
  if (!Storage.exists(FILE_PATH)) {
    writeDefaults();
    return useDefaults(out, max);
  }
  HalFile file;
  if (!Storage.openFileForRead("MQUICK", FILE_PATH, file)) return useDefaults(out, max);

  size_t count = 0;
  size_t len = 0;
  char line[REPLY_SIZE];
  const auto finishLine = [&]() {
    line[len] = 0;
    // Trim spaces and a Windows line end; skip blank lines.
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\t')) line[--len] = 0;
    size_t start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    if (line[start] != 0 && count < max) {
      snprintf(out[count], REPLY_SIZE, "%s", line + start);
      out[count][utf8SafeTruncateBuffer(out[count], static_cast<int>(strlen(out[count])))] = 0;
      count++;
    }
    len = 0;
  };
  uint8_t buf[64];
  while (count < max) {
    const int got = file.read(buf, sizeof(buf));
    if (got <= 0) break;
    for (int i = 0; i < got; i++) {
      if (buf[i] == '\n') {
        finishLine();
      } else if (len + 1 < sizeof(line)) {
        line[len++] = static_cast<char>(buf[i]);
      }
    }
  }
  if (len > 0) finishLine();
  file.close();
  return count > 0 ? count : useDefaults(out, max);
}

}  // namespace meshquick

#endif  // CROSSPOINT_MESH
