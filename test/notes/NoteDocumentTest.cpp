#include <gtest/gtest.h>

#include <string>

#include "NoteDocument.h"

using notes::NoteDocument;

namespace {
NoteDocument parsed(const std::string& text) {
  NoteDocument doc;
  doc.parse(text.data(), text.size());
  return doc;
}
}  // namespace

TEST(NoteDocument, ParagraphsAreLinesAndRoundTrip) {
  const NoteDocument doc = parsed("Groceries\r\n[ ] milk\n\n- [x] eggs\n");
  ASSERT_EQ(doc.count(), 4u);
  EXPECT_EQ(doc.at(0), "Groceries");
  EXPECT_EQ(doc.at(2), "");
  EXPECT_EQ(doc.serialize(), "Groceries\n[ ] milk\n\n- [x] eggs\n");
  EXPECT_EQ(parsed("no newline").count(), 1u);
  EXPECT_EQ(parsed("").count(), 0u);
}

TEST(NoteDocument, ChecklistItems) {
  NoteDocument doc = parsed("[ ] milk\n- [x] eggs\nplain\n[]not a box\n");
  EXPECT_EQ(NoteDocument::taskOf(doc.at(0)), NoteDocument::Task::Open);
  EXPECT_EQ(NoteDocument::taskOf(doc.at(1)), NoteDocument::Task::Done);
  EXPECT_EQ(NoteDocument::taskOf(doc.at(2)), NoteDocument::Task::None);
  EXPECT_EQ(NoteDocument::taskOf(doc.at(3)), NoteDocument::Task::None);
  EXPECT_EQ(NoteDocument::bodyOf(doc.at(1)), "eggs");
  ASSERT_TRUE(doc.toggleDone(0));
  EXPECT_EQ(doc.at(0), "[x] milk");
  ASSERT_TRUE(doc.toggleDone(1));
  EXPECT_EQ(doc.at(1), "- [ ] eggs");
  EXPECT_FALSE(doc.toggleDone(2));
  ASSERT_TRUE(doc.setTask(2, true));
  EXPECT_EQ(doc.at(2), "[ ] plain");
  ASSERT_TRUE(doc.setTask(1, false));
  EXPECT_EQ(doc.at(1), "eggs");
}

TEST(NoteDocument, EditingAndLimits) {
  NoteDocument doc = parsed("one\ntwo\n");
  ASSERT_TRUE(doc.insert(1, "middle"));
  ASSERT_TRUE(doc.set(0, "first"));
  EXPECT_EQ(doc.serialize(), "first\nmiddle\ntwo\n");
  ASSERT_TRUE(doc.set(1, ""));  // empty removes
  EXPECT_EQ(doc.count(), 2u);
  EXPECT_FALSE(doc.set(5, "x"));
  EXPECT_FALSE(doc.insert(9, "x"));
  const std::string big(NoteDocument::MAX_BYTES, 'a');
  EXPECT_FALSE(doc.insert(0, big));
}

TEST(NoteDocument, TitlesAndNames) {
  EXPECT_EQ(parsed("\n  \n[ ]  Shopping list  \nmore\n").title(), "Shopping list");
  EXPECT_EQ(notes::titleFromStart("\r\n- [x] Done thing\r\n", 18), "Done thing");
  // UTF-8 is never cut inside a character: "é" is two bytes.
  EXPECT_EQ(notes::truncateUtf8("caf\xC3\xA9", 4), "caf");
  EXPECT_EQ(notes::nextNoteName({}), "note-001.txt");
  EXPECT_EQ(notes::nextNoteName({"note-007.txt", "shopping.txt", "note-012.txt"}), "note-013.txt");
}

TEST(NoteDocument, TypedTextBecomesParagraphs) {
  NoteDocument doc = parsed("Shopping\n[ ] milk\n");
  // Edit "[ ] milk" into three items.
  EXPECT_EQ(doc.replaceWithText(1, "[ ] milk\n[ ] eggs\n[ ] bread"), 3);
  EXPECT_EQ(doc.serialize(), "Shopping\n[ ] milk\n[ ] eggs\n[ ] bread\n");
  EXPECT_EQ(doc.insertText(1, "Saturday\n"), 1);
  EXPECT_EQ(doc.at(1), "Saturday");
  EXPECT_EQ(doc.replaceWithText(1, ""), 0);  // removed
  EXPECT_EQ(doc.count(), 4u);
  EXPECT_EQ(doc.insertText(9, "x"), -1);
  EXPECT_EQ(doc.insertText(0, std::string(NoteDocument::MAX_BYTES, 'a')), -1);
}
