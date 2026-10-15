#pragma once

#include <cstddef>
#include <cstdint>

#include "MeshFrames.h"

// On-disk format of one conversation's message history: a 4-byte header
// followed by append-only records. Each record carries its body length at
// both ends, so the newest messages can be read from the end of the file
// without scanning it, and a record cut short by a power loss is detectable.
//
//   file   = 'M' 'C' 'T' version, record*
//   record = u16 bodyLen, body, u16 bodyLen
//   body   = u32 timestamp, u8 flags, i8 snr*4, u8 pathLen, u8 senderLen,
//            u16 textLen, sender bytes, text bytes
//   flags  = bit0 outgoing, bit1 hasSnr, bits 4-7 delivery
//
// A record never changes once written, with one exception: the delivery
// state of an outgoing message, which is updated in place in its flags byte.
//
// This code touches no file system: callers supply a read function, so it
// runs unchanged in host tests.
namespace meshc {

constexpr uint8_t THREAD_LOG_VERSION = 1;
constexpr size_t THREAD_LOG_HEADER_SIZE = 4;
constexpr size_t MAX_SENDER_BYTES = 32;
constexpr size_t MAX_STORED_TEXT_BYTES = MAX_FRAME_SIZE;
constexpr size_t RECORD_FIXED_BODY_SIZE = 10;
constexpr size_t MAX_RECORD_BODY_SIZE = RECORD_FIXED_BODY_SIZE + MAX_SENDER_BYTES + MAX_STORED_TEXT_BYTES;
constexpr size_t MAX_RECORD_SIZE = MAX_RECORD_BODY_SIZE + 4;

// Offset of the flags byte from the start of a record.
constexpr size_t RECORD_FLAGS_OFFSET = 6;

enum class Delivery : uint8_t {
  None = 0,
  Sending = 1,      // not yet taken by the radio
  Sent = 2,         // taken by the radio; for a direct message, no ack yet
  Delivered = 3,    // the recipient acknowledged it
  Failed = 4,       // the radio did not take it
  Unconfirmed = 5,  // sent, but every try went unacknowledged
};

struct StoredMessage {
  uint32_t timestamp = 0;  // sender's clock, epoch seconds
  bool outgoing = false;
  Delivery delivery = Delivery::None;  // outgoing messages only
  bool hasSnr = false;
  int8_t snrQuarterDb = 0;
  uint8_t pathLen = 0;
  uint16_t textLen = 0;
  char sender[MAX_SENDER_BYTES + 1] = {0};     // empty when unknown
  char text[MAX_STORED_TEXT_BYTES + 1] = {0};  // UTF-8, null terminated
};

// Writes the file header into out and returns its size, or 0 if capacity is too small.
size_t writeThreadLogHeader(uint8_t* out, size_t capacity);
bool isThreadLogHeader(const uint8_t* data, size_t len);

// Encodes one whole record (both length fields included). Returns its size,
// or 0 if capacity is too small. Sender and text are cut to their limits.
size_t encodeRecord(const StoredMessage& message, uint8_t* out, size_t capacity);
// Checks a record body (the bytes between the two length fields) without decoding it.
bool isValidRecordBody(const uint8_t* body, size_t bodyLen);
// The flags byte of a record with its delivery state replaced.
uint8_t flagsWithDelivery(uint8_t flags, Delivery delivery);
bool decodeRecordBody(const uint8_t* body, size_t bodyLen, StoredMessage& out);

// Reads len bytes at offset; false on any short read or error.
using ReadAtFn = bool (*)(void* ctx, uint32_t offset, uint8_t* buf, size_t len);

// Walks a thread log from its newest record back to its oldest.
class ThreadLogTail {
 public:
  ThreadLogTail(ReadAtFn readAt, void* ctx, uint32_t fileSize);
  // False when the file is too short or its header is not a thread log.
  bool valid() const { return headerOk; }
  // Reads the next older record. False at the start of the file, or at a
  // record that fails its checks; nothing older is returned after that.
  bool previous(StoredMessage& out);
  // Offset of the oldest record returned so far: where a walk over the
  // records before it would start. The file size before any was returned.
  uint32_t offset() const { return cursor; }

 private:
  ReadAtFn readAt;
  void* ctx;
  uint32_t cursor;  // offset just past the next record to return
  bool headerOk = false;
};

// Length of the valid prefix of a log: the header plus every record that
// passes its checks, scanning from the start. A file cut short while a record
// was being written is repaired by truncating it to this length. Returns 0
// when the header itself is missing or wrong.
uint32_t validThreadLogLength(ReadAtFn readAt, void* ctx, uint32_t fileSize);

}  // namespace meshc
