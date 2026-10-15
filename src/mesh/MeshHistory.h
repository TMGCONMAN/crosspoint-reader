#pragma once

#include "MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshThreadLog.h>

#include <cstddef>
#include <cstdint>

// Message history on the SD card: one append-only log per conversation under
// /.crosspoint/mesh/threads/. The format is meshc's thread log. Call from the
// main loop only; SD access is not safe from the BLE tasks.
namespace meshhistory {

// "g_", "c_" or "s_" plus up to 12 hex digits and a terminator. MeshService
// uses "s_" plus a slot number for a channel the radio did not list.
constexpr size_t THREAD_KEY_SIZE = 16;

// Channel logs are named after a hash of the channel secret, so a channel
// moved to another slot keeps its history and the secret never reaches the card.
void channelThreadKey(const uint8_t* secret, char* out);
// Direct-message logs are named after the contact's 6-byte public key prefix.
void contactThreadKey(const uint8_t* keyPrefix, char* out);

// True when key names a direct-message log; keyPrefix then receives the
// contact's KEY_PREFIX_SIZE bytes.
bool parseContactThreadKey(const char* threadKey, uint8_t* keyPrefix);

// Appends one message and returns the offset of its record, or 0 when it
// could not be stored. A log left with a half-written last record by a power
// loss is cut back to its last whole record first.
uint32_t append(const char* threadKey, const meshc::StoredMessage& message);

// Changes the delivery state of the outgoing message at an offset that
// append() returned. Nothing else in a log is ever rewritten.
bool setDelivery(const char* threadKey, uint32_t recordOffset, meshc::Delivery delivery);

// Loads the messages that end at endOffset, oldest first, into out, and
// returns how many. endOffset 0 means the end of the log. olderEnd receives
// the endOffset of the page before this one.
size_t loadPage(const char* threadKey, uint32_t endOffset, meshc::StoredMessage* out, size_t capacity,
                uint32_t& olderEnd);
// False when the page loadPage() reported as older would be empty.
inline bool hasOlderPage(uint32_t olderEnd) { return olderEnd > meshc::THREAD_LOG_HEADER_SIZE; }
// Loads the newest messages, oldest first, into out. Returns how many.
size_t loadTail(const char* threadKey, meshc::StoredMessage* out, size_t capacity);

// Calls fn with the key of every direct-message log on the card.
// Deletes a conversation's history. True when no file is left.
bool removeThread(const char* threadKey);

void forEachContactThread(void (*fn)(void* ctx, const char* threadKey), void* ctx);

}  // namespace meshhistory

#endif  // CROSSPOINT_MESH
