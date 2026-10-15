#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "TextWrap.h"

using textwrap::Line;

namespace {
// Every character is 10 px wide.
int tenEach(const char*, int, void*) { return 10; }
std::vector<std::string> shown(const std::string& text, int width) {
  std::vector<Line> lines;
  textwrap::wrap(text, width, &tenEach, nullptr, lines);
  std::vector<std::string> out;
  for (const auto& l : lines) out.push_back(text.substr(l.start, l.end - l.start));
  return out;
}
}  // namespace

TEST(TextWrap, WholeWordsWrap) {
  // 100 px = 10 characters a line.
  EXPECT_EQ(shown("the quick brown fox jumps", 100), (std::vector<std::string>{"the quick ", "brown fox ", "jumps"}));
}

TEST(TextWrap, NewlinesBreakAndTrailingNewlineOpensALine) {
  EXPECT_EQ(shown("milk\neggs\n", 100), (std::vector<std::string>{"milk", "eggs", ""}));
  EXPECT_EQ(shown("", 100), (std::vector<std::string>{""}));
  EXPECT_EQ(shown("\n\n", 100), (std::vector<std::string>{"", "", ""}));
}

TEST(TextWrap, LongWordsSplit) {
  EXPECT_EQ(shown("abcdefghijklmnopqrstuvwxyz", 100), (std::vector<std::string>{"abcdefghij", "klmnopqrst", "uvwxyz"}));
}

TEST(TextWrap, CursorLines) {
  const std::string text = "milk\nthe quick brown";
  std::vector<Line> lines;
  textwrap::wrap(text, 100, &tenEach, nullptr, lines);
  ASSERT_EQ(lines.size(), 3u);  // "milk", "the quick ", "brown"
  EXPECT_EQ(textwrap::lineOfCursor(lines, 0), 0);
  EXPECT_EQ(textwrap::lineOfCursor(lines, 4), 0);   // at the newline: end of "milk"
  EXPECT_EQ(textwrap::lineOfCursor(lines, 5), 1);   // start of "the"
  EXPECT_EQ(textwrap::lineOfCursor(lines, 15), 2);  // after the soft wrap: start of "brown"
  EXPECT_EQ(textwrap::lineOfCursor(lines, text.size()), 2);
}
