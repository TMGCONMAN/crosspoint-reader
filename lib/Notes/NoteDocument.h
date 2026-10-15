#pragma once

#include <cstddef>
#include <string>
#include <vector>

// A note is a plain text file; each line is one paragraph, so notes can be
// written or read on a computer too. A paragraph starting with "[ ] " or
// "[x] " (optionally after "- ", as in Markdown) is a checklist item.
namespace notes {

class NoteDocument {
 public:
  static constexpr size_t MAX_BYTES = 48 * 1024;  // what the device edits
  static constexpr size_t MAX_ENTRY = 4000;       // what one keyboard entry holds
  static constexpr size_t MAX_EDIT_ALL = 8000;    // a note edited in one go
  enum class Task { None, Open, Done };

  void parse(const char* text, size_t length);
  std::string serialize() const;
  size_t bytes() const;

  size_t count() const { return paragraphs.size(); }
  const std::string& at(size_t index) const { return paragraphs[index]; }
  // The first non-empty paragraph without its checkbox, at most maxBytes.
  std::string title(size_t maxBytes = 60) const;

  // Each returns false if the index is out of range or the note would grow
  // past MAX_BYTES. Setting a paragraph to empty text removes it.
  bool set(size_t index, const std::string& text);
  bool insert(size_t index, const std::string& text);
  bool remove(size_t index);
  // Text with newlines becomes that many paragraphs. replaceWithText swaps
  // one paragraph for them (empty text removes it); insertText adds them
  // before index. Both return how many paragraphs were added, or -1 when the
  // index is out of range or the note would grow past MAX_BYTES.
  int replaceWithText(size_t index, const std::string& text);
  int insertText(size_t index, const std::string& text);
  // A checklist item: unticked <-> ticked. Plain paragraphs are left alone.
  bool toggleDone(size_t index);
  // Adds or removes the checkbox, keeping the text.
  bool setTask(size_t index, bool task);

  static Task taskOf(const std::string& paragraph);
  // The text after any checkbox.
  static std::string bodyOf(const std::string& paragraph);

 private:
  std::vector<std::string> paragraphs;
};

// Cuts text to at most maxBytes without splitting a UTF-8 character.
std::string truncateUtf8(const std::string& text, size_t maxBytes);
// "note-001.txt", one past the highest such number in names.
std::string nextNoteName(const std::vector<std::string>& names);
// The title a note file starts with, from its first bytes.
std::string titleFromStart(const char* text, size_t length, size_t maxBytes = 60);

}  // namespace notes
