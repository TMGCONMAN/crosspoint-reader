#include <gtest/gtest.h>

#include "MeshSendTracker.h"

using namespace meshc;
using Timeout = DirectSendTracker::Timeout;

namespace {

SentInfo sent(uint32_t ack, uint32_t timeoutMs, bool flood = false) {
  SentInfo info;
  info.expectedAck = ack;
  info.timeoutMs = timeoutMs;
  info.flood = flood;
  return info;
}

constexpr uint32_t MARGIN = DirectSendTracker::ACK_MARGIN_MS;

}  // namespace

TEST(MeshSendTracker, AckWithinTheWaitConfirmsTheMessage) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  EXPECT_TRUE(tracker.onAccepted(7, sent(0xAAAA, 4000), 1000));

  uint32_t tag = 0;
  EXPECT_EQ(tracker.nextTimeout(1000 + 4000 + MARGIN - 1, tag), Timeout::None);
  EXPECT_EQ(tracker.onConfirmed(0xAAAA), 7u);
  EXPECT_FALSE(tracker.tracks(7));
  EXPECT_EQ(tracker.onConfirmed(0xAAAA), 0u);  // a repeated ack matches nothing
}

TEST(MeshSendTracker, UnknownOrZeroAckMatchesNothing) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  tracker.onAccepted(7, sent(0xAAAA, 4000), 0);
  EXPECT_EQ(tracker.onConfirmed(0xBBBB), 0u);
  EXPECT_EQ(tracker.onConfirmed(0), 0u);
  EXPECT_TRUE(tracker.tracks(7));
}

TEST(MeshSendTracker, NoAckExpectedEndsTracking) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  EXPECT_FALSE(tracker.onAccepted(7, sent(0, 4000), 0));
  EXPECT_FALSE(tracker.tracks(7));
}

TEST(MeshSendTracker, RouteThatStoppedWorkingEndsWithAFloodTry) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  uint32_t now = 0;
  uint32_t tag = 0;

  tracker.onAccepted(7, sent(0xA0, 3000), now);
  EXPECT_EQ(tracker.attempt(7), 0);
  now += 3000 + MARGIN;
  EXPECT_EQ(tracker.nextTimeout(now, tag), Timeout::Retry);
  EXPECT_EQ(tag, 7u);
  EXPECT_EQ(tracker.attempt(7), 1);
  // Not waiting until the caller reports the retry as accepted.
  EXPECT_EQ(tracker.nextTimeout(now + 60000, tag), Timeout::None);

  tracker.onAccepted(7, sent(0xA1, 3000), now);
  now += 3000 + MARGIN;
  EXPECT_EQ(tracker.nextTimeout(now, tag), Timeout::RetryFlood);
  EXPECT_EQ(tracker.attempt(7), 2);

  tracker.onAccepted(7, sent(0xA2, 20000, true), now);
  now += 20000 + MARGIN;
  EXPECT_EQ(tracker.nextTimeout(now, tag), Timeout::GiveUp);
  EXPECT_EQ(tag, 7u);
  EXPECT_FALSE(tracker.tracks(7));
  EXPECT_EQ(tracker.onConfirmed(0xA2), 0u);
}

TEST(MeshSendTracker, FloodTriesNeverAskToClearTheRoute) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  uint32_t tag = 0;
  tracker.onAccepted(7, sent(0xA0, 1000, true), 0);
  EXPECT_EQ(tracker.nextTimeout(1000 + MARGIN, tag), Timeout::Retry);
  tracker.onAccepted(7, sent(0xA1, 1000, true), 10000);
  EXPECT_EQ(tracker.nextTimeout(11000 + MARGIN, tag), Timeout::Retry);
  tracker.onAccepted(7, sent(0xA2, 1000, true), 20000);
  EXPECT_EQ(tracker.nextTimeout(21000 + MARGIN, tag), Timeout::GiveUp);
}

TEST(MeshSendTracker, LateAckForAnEarlierTryStillConfirms) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  uint32_t tag = 0;
  tracker.onAccepted(7, sent(0xA0, 1000), 0);
  ASSERT_EQ(tracker.nextTimeout(1000 + MARGIN, tag), Timeout::Retry);
  // The first try's ack arrives before the retry was even accepted.
  EXPECT_EQ(tracker.onConfirmed(0xA0), 7u);
  EXPECT_FALSE(tracker.tracks(7));
}

TEST(MeshSendTracker, TracksSeveralMessagesAndRefusesWhenFull) {
  DirectSendTracker tracker;
  for (uint32_t tag = 1; tag <= DirectSendTracker::CAPACITY; tag++) ASSERT_TRUE(tracker.begin(tag));
  EXPECT_FALSE(tracker.begin(99));
  EXPECT_FALSE(tracker.begin(1));  // already tracked
  EXPECT_FALSE(tracker.begin(0));

  tracker.onAccepted(2, sent(0xB2, 1000), 0);
  tracker.onAccepted(3, sent(0xB3, 5000), 0);
  uint32_t tag = 0;
  EXPECT_EQ(tracker.nextTimeout(1000 + MARGIN, tag), Timeout::Retry);
  EXPECT_EQ(tag, 2u);
  EXPECT_EQ(tracker.onConfirmed(0xB3), 3u);

  tracker.end(1);
  EXPECT_TRUE(tracker.begin(99));
}

TEST(MeshSendTracker, WaitSurvivesMillisecondClockWrap) {
  DirectSendTracker tracker;
  ASSERT_TRUE(tracker.begin(7));
  const uint32_t start = 0xFFFFF000u;
  tracker.onAccepted(7, sent(0xA0, 3000), start);
  uint32_t tag = 0;
  EXPECT_EQ(tracker.nextTimeout(start + 3000 + MARGIN - 1, tag), Timeout::None);
  EXPECT_EQ(tracker.nextTimeout(start + 3000 + MARGIN, tag), Timeout::Retry);
}
