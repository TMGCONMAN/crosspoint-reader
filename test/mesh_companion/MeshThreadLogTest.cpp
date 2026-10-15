#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "MeshThreadLog.h"

using namespace meshc;

namespace {

// An in-memory stand-in for a log file on the SD card.
struct MemoryFile {
  std::vector<uint8_t> bytes;
  int reads = 0;

  static bool readAt(void* ctx, uint32_t offset, uint8_t* buf, size_t len) {
    auto* self = static_cast<MemoryFile*>(ctx);
    self->reads++;
    if (static_cast<size_t>(offset) + len > self->bytes.size()) return false;
    memcpy(buf, self->bytes.data() + offset, len);
    return true;
  }

  void writeHeader() {
    uint8_t header[THREAD_LOG_HEADER_SIZE];
    ASSERT_EQ(writeThreadLogHeader(header, sizeof(header)), THREAD_LOG_HEADER_SIZE);
    bytes.assign(header, header + sizeof(header));
  }

  void append(const StoredMessage& message) {
    uint8_t record[MAX_RECORD_SIZE];
    const size_t len = encodeRecord(message, record, sizeof(record));
    ASSERT_GT(len, 0u);
    bytes.insert(bytes.end(), record, record + len);
  }

  uint32_t size() const { return static_cast<uint32_t>(bytes.size()); }
};

StoredMessage incoming(const std::string& sender, const std::string& text, uint32_t timestamp) {
  StoredMessage message;
  message.timestamp = timestamp;
  message.hasSnr = true;
  message.snrQuarterDb = -22;
  message.pathLen = 3;
  strncpy(message.sender, sender.c_str(), sizeof(message.sender) - 1);
  strncpy(message.text, text.c_str(), sizeof(message.text) - 1);
  message.textLen = static_cast<uint16_t>(strlen(message.text));
  return message;
}

std::vector<std::string> readNewestFirst(MemoryFile& file) {
  std::vector<std::string> texts;
  ThreadLogTail tail(&MemoryFile::readAt, &file, file.size());
  StoredMessage message;
  while (tail.previous(message)) texts.emplace_back(message.text);
  return texts;
}

}  // namespace

TEST(MeshThreadLogRecord, RoundTripsEveryField) {
  StoredMessage original = incoming("Ana", "on the ridge", 1790000123);
  uint8_t record[MAX_RECORD_SIZE];
  const size_t len = encodeRecord(original, record, sizeof(record));
  ASSERT_EQ(len, 4u + RECORD_FIXED_BODY_SIZE + 3 + 12);

  StoredMessage decoded;
  ASSERT_TRUE(decodeRecordBody(&record[2], len - 4, decoded));
  EXPECT_EQ(decoded.timestamp, 1790000123u);
  EXPECT_FALSE(decoded.outgoing);
  EXPECT_EQ(decoded.delivery, Delivery::None);
  EXPECT_TRUE(decoded.hasSnr);
  EXPECT_EQ(decoded.snrQuarterDb, -22);
  EXPECT_EQ(decoded.pathLen, 3);
  EXPECT_STREQ(decoded.sender, "Ana");
  EXPECT_STREQ(decoded.text, "on the ridge");
  EXPECT_EQ(decoded.textLen, 12);
}

TEST(MeshThreadLogRecord, KeepsOutgoingAndDeliveryStates) {
  for (const Delivery state : {Delivery::Sending, Delivery::Sent, Delivery::Delivered, Delivery::Failed}) {
    StoredMessage original = incoming("", "hello", 5);
    original.outgoing = true;
    original.hasSnr = false;
    original.delivery = state;
    uint8_t record[MAX_RECORD_SIZE];
    const size_t len = encodeRecord(original, record, sizeof(record));
    StoredMessage decoded;
    ASSERT_TRUE(decodeRecordBody(&record[2], len - 4, decoded));
    EXPECT_TRUE(decoded.outgoing);
    EXPECT_FALSE(decoded.hasSnr);
    EXPECT_EQ(decoded.delivery, state);
    EXPECT_STREQ(decoded.sender, "");
  }
}

TEST(MeshThreadLogRecord, BothLengthFieldsMatch) {
  uint8_t record[MAX_RECORD_SIZE];
  const size_t len = encodeRecord(incoming("Ben", "hi", 1), record, sizeof(record));
  const uint16_t leading = static_cast<uint16_t>(record[0] | (record[1] << 8));
  const uint16_t trailing = static_cast<uint16_t>(record[len - 2] | (record[len - 1] << 8));
  EXPECT_EQ(leading, len - 4);
  EXPECT_EQ(trailing, leading);
}

TEST(MeshThreadLogRecord, LargestMessageFitsAndRoundTrips) {
  StoredMessage original = incoming(std::string(MAX_SENDER_BYTES, 'S'), std::string(MAX_STORED_TEXT_BYTES, 't'), 9);
  uint8_t record[MAX_RECORD_SIZE];
  const size_t len = encodeRecord(original, record, sizeof(record));
  ASSERT_EQ(len, MAX_RECORD_SIZE);
  StoredMessage decoded;
  ASSERT_TRUE(decodeRecordBody(&record[2], len - 4, decoded));
  EXPECT_EQ(strlen(decoded.sender), MAX_SENDER_BYTES);
  EXPECT_EQ(decoded.textLen, MAX_STORED_TEXT_BYTES);
  EXPECT_EQ(strlen(decoded.text), MAX_STORED_TEXT_BYTES);
}

TEST(MeshThreadLogRecord, EncodeRejectsShortBufferAndClampsTextLen) {
  uint8_t small[8];
  EXPECT_EQ(encodeRecord(incoming("Ana", "hello", 1), small, sizeof(small)), 0u);

  StoredMessage oversize = incoming("Ana", "x", 1);
  oversize.textLen = 60000;  // a corrupt length must not read past the buffer
  uint8_t record[MAX_RECORD_SIZE];
  EXPECT_LE(encodeRecord(oversize, record, sizeof(record)), MAX_RECORD_SIZE);
}

TEST(MeshThreadLogRecord, DecodeRejectsInconsistentBodies) {
  uint8_t record[MAX_RECORD_SIZE];
  const size_t len = encodeRecord(incoming("Ana", "hello", 1), record, sizeof(record));
  uint8_t* body = &record[2];
  const size_t bodyLen = len - 4;
  StoredMessage decoded;

  EXPECT_FALSE(decodeRecordBody(body, bodyLen - 1, decoded));
  EXPECT_FALSE(decodeRecordBody(body, RECORD_FIXED_BODY_SIZE - 1, decoded));

  uint8_t badSender[MAX_RECORD_SIZE];
  memcpy(badSender, body, bodyLen);
  badSender[7] = MAX_SENDER_BYTES + 1;
  EXPECT_FALSE(decodeRecordBody(badSender, bodyLen, decoded));

  uint8_t badDelivery[MAX_RECORD_SIZE];
  memcpy(badDelivery, body, bodyLen);
  badDelivery[4] = 0xF0;
  EXPECT_FALSE(decodeRecordBody(badDelivery, bodyLen, decoded));
}

TEST(MeshThreadLogRecord, ValidityCheckAgreesWithDecode) {
  uint8_t record[MAX_RECORD_SIZE];
  const size_t len = encodeRecord(incoming("Ana", "hello", 1), record, sizeof(record));
  uint8_t* body = &record[2];
  const size_t bodyLen = len - 4;

  EXPECT_TRUE(isValidRecordBody(body, bodyLen));
  EXPECT_FALSE(isValidRecordBody(body, bodyLen - 1));
  EXPECT_FALSE(isValidRecordBody(body, bodyLen + 1));
  EXPECT_FALSE(isValidRecordBody(body, RECORD_FIXED_BODY_SIZE - 1));

  body[8] = 0xFF;  // text length far past the record
  body[9] = 0xFF;
  EXPECT_FALSE(isValidRecordBody(body, bodyLen));
}

TEST(MeshThreadLogRecord, DeliveryCanBeReplacedInTheFlagsByte) {
  StoredMessage message = incoming("", "on my way", 5);
  message.outgoing = true;
  message.hasSnr = true;
  message.delivery = Delivery::Sent;
  uint8_t record[MAX_RECORD_SIZE];
  const size_t len = encodeRecord(message, record, sizeof(record));
  ASSERT_GT(len, 0u);

  record[RECORD_FLAGS_OFFSET] = flagsWithDelivery(record[RECORD_FLAGS_OFFSET], Delivery::Unconfirmed);
  StoredMessage decoded;
  ASSERT_TRUE(decodeRecordBody(&record[2], len - 4, decoded));
  EXPECT_EQ(decoded.delivery, Delivery::Unconfirmed);
  EXPECT_TRUE(decoded.outgoing);
  EXPECT_TRUE(decoded.hasSnr);
  EXPECT_STREQ(decoded.text, "on my way");

  record[RECORD_FLAGS_OFFSET] = flagsWithDelivery(record[RECORD_FLAGS_OFFSET], Delivery::Delivered);
  ASSERT_TRUE(decodeRecordBody(&record[2], len - 4, decoded));
  EXPECT_EQ(decoded.delivery, Delivery::Delivered);
}

TEST(MeshThreadLogTail, OffsetMarksWhereTheOlderRecordsEnd) {
  MemoryFile file;
  file.writeHeader();
  for (int i = 0; i < 5; i++) file.append(incoming("Ana", "message " + std::to_string(i), 100 + i));

  ThreadLogTail newest(&MemoryFile::readAt, &file, file.size());
  EXPECT_EQ(newest.offset(), file.size());
  StoredMessage message;
  ASSERT_TRUE(newest.previous(message));
  ASSERT_TRUE(newest.previous(message));
  EXPECT_STREQ(message.text, "message 3");

  // A second walk that ends where the first stopped reads the page before it.
  ThreadLogTail older(&MemoryFile::readAt, &file, newest.offset());
  ASSERT_TRUE(older.previous(message));
  EXPECT_STREQ(message.text, "message 2");
  ASSERT_TRUE(older.previous(message));
  ASSERT_TRUE(older.previous(message));
  EXPECT_STREQ(message.text, "message 0");
  EXPECT_EQ(older.offset(), THREAD_LOG_HEADER_SIZE);
  EXPECT_FALSE(older.previous(message));
}

TEST(MeshThreadLogHeader, IdentifiesItsOwnFormatOnly) {
  uint8_t header[THREAD_LOG_HEADER_SIZE];
  ASSERT_EQ(writeThreadLogHeader(header, sizeof(header)), 4u);
  EXPECT_TRUE(isThreadLogHeader(header, sizeof(header)));
  EXPECT_FALSE(isThreadLogHeader(header, 3));
  header[3] = THREAD_LOG_VERSION + 1;
  EXPECT_FALSE(isThreadLogHeader(header, sizeof(header)));
  uint8_t tooSmall[2];
  EXPECT_EQ(writeThreadLogHeader(tooSmall, sizeof(tooSmall)), 0u);
}

TEST(MeshThreadLogTail, ReturnsNewestFirst) {
  MemoryFile file;
  file.writeHeader();
  file.append(incoming("Ana", "first", 1));
  file.append(incoming("Ben", "second, a longer one", 2));
  file.append(incoming("Ana", "third", 3));
  EXPECT_EQ(readNewestFirst(file), (std::vector<std::string>{"third", "second, a longer one", "first"}));
}

TEST(MeshThreadLogTail, EmptyLogAndMissingHeader) {
  MemoryFile empty;
  empty.writeHeader();
  EXPECT_TRUE(readNewestFirst(empty).empty());
  ThreadLogTail emptyTail(&MemoryFile::readAt, &empty, empty.size());
  EXPECT_TRUE(emptyTail.valid());

  MemoryFile none;
  ThreadLogTail noneTail(&MemoryFile::readAt, &none, 0);
  EXPECT_FALSE(noneTail.valid());

  MemoryFile other;
  other.bytes = {'E', 'P', 'U', 'B', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  ThreadLogTail otherTail(&MemoryFile::readAt, &other, other.size());
  StoredMessage message;
  EXPECT_FALSE(otherTail.valid());
  EXPECT_FALSE(otherTail.previous(message));
}

TEST(MeshThreadLogTail, ReadsOnlyTheRecordsAskedFor) {
  MemoryFile file;
  file.writeHeader();
  for (int i = 0; i < 500; i++) file.append(incoming("Ana", "message " + std::to_string(i), i));
  ThreadLogTail tail(&MemoryFile::readAt, &file, file.size());
  file.reads = 0;
  StoredMessage message;
  for (int i = 0; i < 3; i++) ASSERT_TRUE(tail.previous(message));
  EXPECT_STREQ(message.text, "message 497");
  EXPECT_LE(file.reads, 6);  // two reads per record, however long the log is
}

TEST(MeshThreadLogTail, StopsAtATruncatedLastRecord) {
  MemoryFile file;
  file.writeHeader();
  file.append(incoming("Ana", "kept", 1));
  file.append(incoming("Ana", "cut short by a power loss", 2));
  file.bytes.resize(file.bytes.size() - 5);
  // The tail cannot be trusted, so nothing is returned until the log is repaired.
  EXPECT_TRUE(readNewestFirst(file).empty());
}

TEST(MeshThreadLogTail, StopsAtACorruptRecordInTheMiddle) {
  MemoryFile file;
  file.writeHeader();
  file.append(incoming("Ana", "oldest", 1));
  const size_t middleStart = file.bytes.size();
  file.append(incoming("Ana", "middle", 2));
  file.append(incoming("Ana", "newest", 3));
  file.bytes[middleStart] ^= 0xFF;  // leading length no longer matches the trailing one
  EXPECT_EQ(readNewestFirst(file), (std::vector<std::string>{"newest"}));
}

TEST(MeshThreadLogRepair, ValidLengthOfAHealthyLogIsItsSize) {
  MemoryFile file;
  file.writeHeader();
  EXPECT_EQ(validThreadLogLength(&MemoryFile::readAt, &file, file.size()), THREAD_LOG_HEADER_SIZE);
  file.append(incoming("Ana", "one", 1));
  file.append(incoming("Ben", "two", 2));
  EXPECT_EQ(validThreadLogLength(&MemoryFile::readAt, &file, file.size()), file.size());
}

TEST(MeshThreadLogRepair, TruncatingToValidLengthRecoversTheLog) {
  MemoryFile file;
  file.writeHeader();
  file.append(incoming("Ana", "kept one", 1));
  file.append(incoming("Ana", "kept two", 2));
  const uint32_t goodSize = file.size();
  file.append(incoming("Ana", "half written", 3));

  for (const size_t cut : {size_t{1}, size_t{2}, size_t{7}, size_t{20}}) {
    MemoryFile damaged = file;
    damaged.bytes.resize(damaged.bytes.size() - cut);
    const uint32_t valid = validThreadLogLength(&MemoryFile::readAt, &damaged, damaged.size());
    EXPECT_EQ(valid, goodSize) << "cut " << cut;
    damaged.bytes.resize(valid);
    EXPECT_EQ(readNewestFirst(damaged), (std::vector<std::string>{"kept two", "kept one"}));
    // The repaired log accepts new records.
    damaged.append(incoming("Ana", "after repair", 4));
    EXPECT_EQ(readNewestFirst(damaged).front(), "after repair");
  }
}

TEST(MeshThreadLogRepair, GarbageAfterTheLastRecordIsCutOff) {
  MemoryFile file;
  file.writeHeader();
  file.append(incoming("Ana", "good", 1));
  const uint32_t goodSize = file.size();
  file.bytes.insert(file.bytes.end(), 40, 0xFF);
  EXPECT_EQ(validThreadLogLength(&MemoryFile::readAt, &file, file.size()), goodSize);
}

TEST(MeshThreadLogRepair, WrongHeaderMeansNoValidPrefix) {
  MemoryFile file;
  file.bytes = {'X', 'Y', 'Z', 1, 0, 0};
  EXPECT_EQ(validThreadLogLength(&MemoryFile::readAt, &file, file.size()), 0u);
  MemoryFile tiny;
  tiny.bytes = {'M', 'C'};
  EXPECT_EQ(validThreadLogLength(&MemoryFile::readAt, &tiny, tiny.size()), 0u);
}
