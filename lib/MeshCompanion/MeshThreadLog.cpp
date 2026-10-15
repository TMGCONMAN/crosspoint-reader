#include "MeshThreadLog.h"

#include <cstring>

namespace meshc {

namespace {

constexpr uint8_t FLAG_OUTGOING = 0x01;
constexpr uint8_t FLAG_HAS_SNR = 0x02;
constexpr uint8_t DELIVERY_SHIFT = 4;
constexpr uint8_t DELIVERY_MASK = 0xF0;
constexpr uint8_t MAX_DELIVERY = static_cast<uint8_t>(Delivery::Unconfirmed);

uint16_t readU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

void writeU16(uint8_t* p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value);
  p[1] = static_cast<uint8_t>(value >> 8);
}

bool plausibleBodyLen(uint16_t bodyLen) { return bodyLen >= RECORD_FIXED_BODY_SIZE && bodyLen <= MAX_RECORD_BODY_SIZE; }

}  // namespace

size_t writeThreadLogHeader(uint8_t* out, size_t capacity) {
  if (capacity < THREAD_LOG_HEADER_SIZE) return 0;
  out[0] = 'M';
  out[1] = 'C';
  out[2] = 'T';
  out[3] = THREAD_LOG_VERSION;
  return THREAD_LOG_HEADER_SIZE;
}

bool isThreadLogHeader(const uint8_t* data, size_t len) {
  return len >= THREAD_LOG_HEADER_SIZE && data[0] == 'M' && data[1] == 'C' && data[2] == 'T' &&
         data[3] == THREAD_LOG_VERSION;
}

size_t encodeRecord(const StoredMessage& message, uint8_t* out, size_t capacity) {
  const size_t senderLen = strnlen(message.sender, MAX_SENDER_BYTES);
  const size_t textLen = message.textLen > MAX_STORED_TEXT_BYTES ? MAX_STORED_TEXT_BYTES : message.textLen;
  const size_t bodyLen = RECORD_FIXED_BODY_SIZE + senderLen + textLen;
  if (capacity < bodyLen + 4) return 0;

  uint8_t flags = static_cast<uint8_t>(static_cast<uint8_t>(message.delivery) << DELIVERY_SHIFT);
  if (message.outgoing) flags |= FLAG_OUTGOING;
  if (message.hasSnr) flags |= FLAG_HAS_SNR;

  uint8_t* p = out;
  writeU16(p, static_cast<uint16_t>(bodyLen));
  p += 2;
  writeU32(p, message.timestamp);
  p[4] = flags;
  p[5] = static_cast<uint8_t>(message.snrQuarterDb);
  p[6] = message.pathLen;
  p[7] = static_cast<uint8_t>(senderLen);
  writeU16(&p[8], static_cast<uint16_t>(textLen));
  p += RECORD_FIXED_BODY_SIZE;
  memcpy(p, message.sender, senderLen);
  p += senderLen;
  memcpy(p, message.text, textLen);
  p += textLen;
  writeU16(p, static_cast<uint16_t>(bodyLen));
  return bodyLen + 4;
}

bool isValidRecordBody(const uint8_t* body, size_t bodyLen) {
  if (bodyLen < RECORD_FIXED_BODY_SIZE) return false;
  const uint8_t delivery = body[4] >> DELIVERY_SHIFT;
  const size_t senderLen = body[7];
  const size_t textLen = readU16(&body[8]);
  if (delivery > MAX_DELIVERY || senderLen > MAX_SENDER_BYTES || textLen > MAX_STORED_TEXT_BYTES) return false;
  return bodyLen == RECORD_FIXED_BODY_SIZE + senderLen + textLen;
}

uint8_t flagsWithDelivery(uint8_t flags, Delivery delivery) {
  return static_cast<uint8_t>((flags & ~DELIVERY_MASK) | (static_cast<uint8_t>(delivery) << DELIVERY_SHIFT));
}

bool decodeRecordBody(const uint8_t* body, size_t bodyLen, StoredMessage& out) {
  if (!isValidRecordBody(body, bodyLen)) return false;
  const uint8_t flags = body[4];
  const size_t senderLen = body[7];
  const size_t textLen = readU16(&body[8]);

  out = StoredMessage{};
  out.timestamp = readU32(body);
  out.outgoing = (flags & FLAG_OUTGOING) != 0;
  out.hasSnr = (flags & FLAG_HAS_SNR) != 0;
  out.delivery = static_cast<Delivery>(flags >> DELIVERY_SHIFT);
  out.snrQuarterDb = static_cast<int8_t>(body[5]);
  out.pathLen = body[6];
  memcpy(out.sender, &body[RECORD_FIXED_BODY_SIZE], senderLen);
  out.sender[senderLen] = 0;
  memcpy(out.text, &body[RECORD_FIXED_BODY_SIZE + senderLen], textLen);
  out.text[textLen] = 0;
  out.textLen = static_cast<uint16_t>(textLen);
  return true;
}

ThreadLogTail::ThreadLogTail(ReadAtFn readAt, void* ctx, uint32_t fileSize)
    : readAt(readAt), ctx(ctx), cursor(fileSize) {
  uint8_t header[THREAD_LOG_HEADER_SIZE];
  headerOk = fileSize >= THREAD_LOG_HEADER_SIZE && readAt(ctx, 0, header, sizeof(header)) &&
             isThreadLogHeader(header, sizeof(header));
}

bool ThreadLogTail::previous(StoredMessage& out) {
  if (!headerOk || cursor < THREAD_LOG_HEADER_SIZE + RECORD_FIXED_BODY_SIZE + 4) return false;

  uint8_t lenBytes[2];
  if (!readAt(ctx, cursor - 2, lenBytes, 2)) return false;
  const uint16_t bodyLen = readU16(lenBytes);
  if (!plausibleBodyLen(bodyLen) || cursor < THREAD_LOG_HEADER_SIZE + bodyLen + 4) {
    headerOk = false;
    return false;
  }

  const uint32_t start = cursor - bodyLen - 4;
  uint8_t record[MAX_RECORD_BODY_SIZE + 2];
  if (!readAt(ctx, start, record, bodyLen + 2u) || readU16(record) != bodyLen ||
      !decodeRecordBody(&record[2], bodyLen, out)) {
    headerOk = false;
    return false;
  }
  cursor = start;
  return true;
}

uint32_t validThreadLogLength(ReadAtFn readAt, void* ctx, uint32_t fileSize) {
  uint8_t header[THREAD_LOG_HEADER_SIZE];
  if (fileSize < THREAD_LOG_HEADER_SIZE || !readAt(ctx, 0, header, sizeof(header)) ||
      !isThreadLogHeader(header, sizeof(header))) {
    return 0;
  }

  uint32_t offset = THREAD_LOG_HEADER_SIZE;
  uint8_t record[MAX_RECORD_SIZE];
  for (;;) {
    uint8_t lenBytes[2];
    if (fileSize - offset < RECORD_FIXED_BODY_SIZE + 4 || !readAt(ctx, offset, lenBytes, 2)) break;
    const uint16_t bodyLen = readU16(lenBytes);
    if (!plausibleBodyLen(bodyLen) || fileSize - offset < bodyLen + 4u) break;
    if (!readAt(ctx, offset + 2, record, bodyLen + 2u)) break;
    if (readU16(&record[bodyLen]) != bodyLen || !isValidRecordBody(record, bodyLen)) break;
    offset += bodyLen + 4u;
  }
  return offset;
}

}  // namespace meshc
