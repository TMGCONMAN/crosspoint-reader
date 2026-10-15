#include "MeshSendTracker.h"

namespace meshc {

namespace {

bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

}  // namespace

DirectSendTracker::Entry* DirectSendTracker::find(uint32_t tag) {
  if (tag == 0) return nullptr;
  for (Entry& entry : entries) {
    if (entry.tag == tag) return &entry;
  }
  return nullptr;
}

const DirectSendTracker::Entry* DirectSendTracker::find(uint32_t tag) const {
  if (tag == 0) return nullptr;
  for (const Entry& entry : entries) {
    if (entry.tag == tag) return &entry;
  }
  return nullptr;
}

bool DirectSendTracker::begin(uint32_t tag) {
  if (tag == 0 || find(tag)) return false;
  for (Entry& entry : entries) {
    if (entry.tag != 0) continue;
    entry = Entry{};
    entry.tag = tag;
    return true;
  }
  return false;
}

void DirectSendTracker::end(uint32_t tag) {
  if (Entry* entry = find(tag)) *entry = Entry{};
}

uint8_t DirectSendTracker::attempt(uint32_t tag) const {
  const Entry* entry = find(tag);
  return entry ? entry->attempt : 0;
}

bool DirectSendTracker::onAccepted(uint32_t tag, const SentInfo& info, uint32_t nowMs) {
  Entry* entry = find(tag);
  if (!entry) return false;
  if (info.expectedAck == 0) {
    *entry = Entry{};
    return false;
  }
  entry->acks[entry->attempt] = info.expectedAck;
  entry->lastWasFlood = info.flood;
  entry->deadlineMs = nowMs + info.timeoutMs + ACK_MARGIN_MS;
  entry->waiting = true;
  return true;
}

uint32_t DirectSendTracker::onConfirmed(uint32_t ackCode) {
  if (ackCode == 0) return 0;
  for (Entry& entry : entries) {
    if (entry.tag == 0) continue;
    for (const uint32_t ack : entry.acks) {
      if (ack != ackCode) continue;
      const uint32_t tag = entry.tag;
      entry = Entry{};
      return tag;
    }
  }
  return 0;
}

DirectSendTracker::Timeout DirectSendTracker::nextTimeout(uint32_t nowMs, uint32_t& tag) {
  for (Entry& entry : entries) {
    if (entry.tag == 0 || !entry.waiting || !reached(nowMs, entry.deadlineMs)) continue;
    tag = entry.tag;
    if (entry.attempt + 1 >= MAX_ATTEMPTS) {
      entry = Entry{};
      return Timeout::GiveUp;
    }
    entry.waiting = false;
    entry.attempt++;
    const bool lastTry = entry.attempt + 1 == MAX_ATTEMPTS;
    return lastTry && !entry.lastWasFlood ? Timeout::RetryFlood : Timeout::Retry;
  }
  return Timeout::None;
}

}  // namespace meshc
