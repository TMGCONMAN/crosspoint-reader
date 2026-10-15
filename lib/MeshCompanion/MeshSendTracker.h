#pragma once

#include <cstddef>
#include <cstdint>

#include "MeshFrames.h"

namespace meshc {

// Follows direct messages from "the radio took it" to "the recipient
// acknowledged it" or "gave up". The radio reports an ack code and its
// estimate of the round trip for each attempt; this decides when an attempt
// has failed and what to try next. It sends nothing itself and has no clock:
// the caller acts on its answers and passes the time in.
//
// A message is tried MAX_ATTEMPTS times. The radio does not fall back to
// flooding when a stored route has stopped working, so if the earlier tries
// used a route, the last try asks the caller to clear that route first.
class DirectSendTracker {
 public:
  static constexpr uint8_t CAPACITY = 4;
  static constexpr uint8_t MAX_ATTEMPTS = 3;
  // Added to the radio's estimate before a try counts as lost.
  static constexpr uint32_t ACK_MARGIN_MS = 2000;

  enum class Timeout : uint8_t {
    None,
    Retry,       // send again
    RetryFlood,  // clear the stored route, then send again
    GiveUp,      // every try went unanswered; the message is no longer tracked
  };

  // Start tracking the message with this non-zero tag. False when full.
  bool begin(uint32_t tag);
  void end(uint32_t tag);
  bool tracks(uint32_t tag) const { return find(tag) != nullptr; }
  // Number of the try to send next, or of the one in the air: 0 for the first.
  uint8_t attempt(uint32_t tag) const;

  // The radio took the current try. False when it expects no ack; the
  // message is then no longer tracked.
  bool onAccepted(uint32_t tag, const SentInfo& info, uint32_t nowMs);
  // An ack arrived. Returns the tag it belongs to and stops tracking that
  // message, or 0 when the code matches no tracked try.
  uint32_t onConfirmed(uint32_t ackCode);
  // Reports one message whose wait has run out, if any. For Retry and
  // RetryFlood the caller sends attempt(tag) and reports back through
  // onAccepted(), or calls end() when it cannot send.
  Timeout nextTimeout(uint32_t nowMs, uint32_t& tag);

 private:
  struct Entry {
    uint32_t tag = 0;  // 0 = free
    uint8_t attempt = 0;
    bool waiting = false;
    bool lastWasFlood = false;
    uint32_t deadlineMs = 0;
    // An ack for an earlier try can still arrive after a later one was sent.
    uint32_t acks[MAX_ATTEMPTS] = {0};
  };

  Entry* find(uint32_t tag);
  const Entry* find(uint32_t tag) const;

  Entry entries[CAPACITY];
};

}  // namespace meshc
