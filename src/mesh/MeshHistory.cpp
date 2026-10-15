#include "MeshHistory.h"

#if CROSSPOINT_MESH

#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

namespace meshhistory {

namespace {

constexpr char THREADS_DIR[] = "/.crosspoint/mesh/threads";
constexpr size_t PATH_SIZE = 64;
constexpr size_t SHA256_SIZE = 32;

// Shared by every call so the main-loop stack stays small. Main loop only.
meshc::StoredMessage scratch;
uint8_t recordBuf[meshc::MAX_RECORD_SIZE];
// Set once the directory is known to exist; cleared when an open fails.
bool threadsDirReady = false;

void threadPath(const char* threadKey, char* out) { snprintf(out, PATH_SIZE, "%s/%s.log", THREADS_DIR, threadKey); }

// Read context for the log codec. The codec cannot tell a failed read from a
// bad record, and only a bad record may be repaired by cutting the file.
struct LogReader {
  HalFile& file;
  bool ioError = false;
};

bool readAt(void* ctx, uint32_t offset, uint8_t* buf, size_t len) {
  auto* reader = static_cast<LogReader*>(ctx);
  if (reader->file.seek(offset) && reader->file.read(buf, len) == static_cast<int>(len)) return true;
  reader->ioError = true;
  return false;
}

bool startNewLog(HalFile& file) {
  uint8_t header[meshc::THREAD_LOG_HEADER_SIZE];
  const size_t len = meshc::writeThreadLogHeader(header, sizeof(header));
  return file.seek(0) && file.write(header, len) == len;
}

// Length of the log up to its last whole record, or 0 when the file cannot
// be read or is not a thread log.
uint32_t usableLength(LogReader& reader, uint32_t size) {
  // Checking the last record is cheap; the full scan runs only when it fails.
  meshc::ThreadLogTail tail(&readAt, &reader, size);
  if (tail.valid() && (size == meshc::THREAD_LOG_HEADER_SIZE || tail.previous(scratch))) return size;
  if (reader.ioError) return 0;
  const uint32_t validLen = meshc::validThreadLogLength(&readAt, &reader, size);
  return reader.ioError ? 0 : validLen;
}

// Returns the offset to append at, or 0 when the file must not be written.
uint32_t prepareForAppend(HalFile& file, const char* threadKey) {
  const uint32_t size = static_cast<uint32_t>(file.fileSize());
  // Shorter than a header: new, or cut while the header was being written.
  if (size < meshc::THREAD_LOG_HEADER_SIZE) return startNewLog(file) ? meshc::THREAD_LOG_HEADER_SIZE : 0;

  LogReader reader{file};
  const uint32_t validLen = usableLength(reader, size);
  if (validLen == size) return size;
  if (validLen == 0) {
    // A read error, or a file this version did not write. Leave it alone.
    LOG_ERR("MHIST", "Log %s is unreadable; message not stored", threadKey);
    return 0;
  }
  // A power loss cut the last record short.
  LOG_ERR("MHIST", "Log %s damaged: keeping %u of %u bytes", threadKey, static_cast<unsigned>(validLen),
          static_cast<unsigned>(size));
  return file.truncate(validLen) ? validLen : 0;
}

}  // namespace

void channelThreadKey(const uint8_t* secret, char* out) {
  uint8_t digest[SHA256_SIZE];
  mbedtls_sha256(secret, meshc::CHANNEL_SECRET_SIZE, digest, 0);
  snprintf(out, THREAD_KEY_SIZE, "g_%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
}

void contactThreadKey(const uint8_t* keyPrefix, char* out) {
  snprintf(out, THREAD_KEY_SIZE, "c_%02x%02x%02x%02x%02x%02x", keyPrefix[0], keyPrefix[1], keyPrefix[2], keyPrefix[3],
           keyPrefix[4], keyPrefix[5]);
}

bool parseContactThreadKey(const char* threadKey, uint8_t* keyPrefix) {
  constexpr size_t KEY_LEN = 2 + 2 * meshc::KEY_PREFIX_SIZE;
  if (threadKey == nullptr || strncmp(threadKey, "c_", 2) != 0 || strlen(threadKey) != KEY_LEN) return false;
  for (size_t i = 0; i < meshc::KEY_PREFIX_SIZE; i++) {
    uint8_t value = 0;
    for (const char c : {threadKey[2 + 2 * i], threadKey[3 + 2 * i]}) {
      uint8_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<uint8_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<uint8_t>(c - 'a' + 10);
      } else {
        return false;
      }
      value = static_cast<uint8_t>((value << 4) | digit);
    }
    keyPrefix[i] = value;
  }
  return true;
}

uint32_t append(const char* threadKey, const meshc::StoredMessage& message) {
  if (!threadsDirReady && !Storage.ensureDirectoryExists(THREADS_DIR)) {
    LOG_ERR("MHIST", "Cannot create %s", THREADS_DIR);
    return 0;
  }
  threadsDirReady = true;
  char path[PATH_SIZE];
  threadPath(threadKey, path);
  HalFile file = Storage.open(path, O_RDWR | O_CREAT);
  if (!file) {
    LOG_ERR("MHIST", "Cannot open %s", path);
    threadsDirReady = false;
    return 0;
  }

  const uint32_t offset = prepareForAppend(file, threadKey);
  const size_t len = meshc::encodeRecord(message, recordBuf, sizeof(recordBuf));
  const bool written = offset != 0 && len != 0 && file.seek(offset) && file.write(recordBuf, len) == len;
  // close() flushes; a failure there means the record may not be on the card.
  if (!file.close() || !written) {
    LOG_ERR("MHIST", "Write to %s failed", path);
    return 0;
  }
  return offset;
}

bool setDelivery(const char* threadKey, uint32_t recordOffset, meshc::Delivery delivery) {
  char path[PATH_SIZE];
  threadPath(threadKey, path);
  HalFile file = Storage.open(path, O_RDWR);
  if (!file) return false;

  // Check that a whole record starts here before changing a byte of it.
  const uint32_t size = static_cast<uint32_t>(file.fileSize());
  uint8_t head[2];
  if (recordOffset < meshc::THREAD_LOG_HEADER_SIZE || size < recordOffset ||
      size - recordOffset < meshc::RECORD_FIXED_BODY_SIZE + 4 || !file.seek(recordOffset) ||
      file.read(head, sizeof(head)) != sizeof(head)) {
    return false;
  }
  const size_t bodyLen = static_cast<size_t>(head[0] | (head[1] << 8));
  if (bodyLen > meshc::MAX_RECORD_BODY_SIZE || size - recordOffset < bodyLen + 4 ||
      file.read(recordBuf, bodyLen) != static_cast<int>(bodyLen) || !meshc::isValidRecordBody(recordBuf, bodyLen)) {
    LOG_ERR("MHIST", "No record at %u in %s", static_cast<unsigned>(recordOffset), threadKey);
    return false;
  }

  const size_t flagsInBody = meshc::RECORD_FLAGS_OFFSET - 2;
  const uint8_t flags = meshc::flagsWithDelivery(recordBuf[flagsInBody], delivery);
  const bool written = file.seek(recordOffset + meshc::RECORD_FLAGS_OFFSET) && file.write(&flags, 1) == 1;
  return file.close() && written;
}

size_t loadPage(const char* threadKey, uint32_t endOffset, meshc::StoredMessage* out, size_t capacity,
                uint32_t& olderEnd) {
  olderEnd = meshc::THREAD_LOG_HEADER_SIZE;
  if (out == nullptr || capacity == 0) return 0;
  char path[PATH_SIZE];
  threadPath(threadKey, path);
  if (!Storage.exists(path)) return 0;
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) return 0;

  // A log with a half-written last record still shows what came before it.
  LogReader reader{file};
  const uint32_t size = static_cast<uint32_t>(file.fileSize());
  const uint32_t validLen = size < meshc::THREAD_LOG_HEADER_SIZE ? 0 : usableLength(reader, size);
  if (validLen == 0) return 0;
  if (endOffset == 0 || endOffset > validLen) endOffset = validLen;

  // Fill from the back so the result ends up oldest first.
  meshc::ThreadLogTail tail(&readAt, &reader, endOffset);
  size_t count = 0;
  while (count < capacity && tail.previous(out[capacity - 1 - count])) count++;
  if (count > 0 && count < capacity) {
    memmove(out, out + (capacity - count), count * sizeof(meshc::StoredMessage));
  }
  if (count > 0) olderEnd = tail.offset();
  return count;
}

size_t loadTail(const char* threadKey, meshc::StoredMessage* out, size_t capacity) {
  uint32_t olderEnd = 0;
  return loadPage(threadKey, 0, out, capacity, olderEnd);
}

bool removeThread(const char* threadKey) {
  char path[PATH_SIZE];
  threadPath(threadKey, path);
  if (!Storage.exists(path)) return true;
  if (Storage.remove(path)) return true;
  LOG_ERR("MHIST", "Could not delete %s", path);
  return false;
}

void forEachContactThread(void (*fn)(void* ctx, const char* threadKey), void* ctx) {
  if (!Storage.exists(THREADS_DIR)) return;
  HalFile dir = Storage.open(THREADS_DIR);
  if (!dir || !dir.isDirectory()) return;
  char name[PATH_SIZE];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (entry.isDirectory()) continue;
    entry.getName(name, sizeof(name));
    // "c_" + 12 hex digits + ".log"
    char* extension = strrchr(name, '.');
    if (extension == nullptr || strcmp(extension, ".log") != 0) continue;
    *extension = 0;
    uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
    if (parseContactThreadKey(name, keyPrefix)) fn(ctx, name);
  }
}

}  // namespace meshhistory

#endif  // CROSSPOINT_MESH
