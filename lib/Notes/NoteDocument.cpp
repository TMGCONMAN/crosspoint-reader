#include "NoteDocument.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace notes {

namespace {

// The checkbox prefix of a paragraph: its length, and whether it is ticked.
size_t boxPrefix(const std::string& p, bool& done) {
  size_t i = 0;
  if (p.compare(0, 2, "- ") == 0 || p.compare(0, 2, "* ") == 0) i = 2;
  if (p.size() >= i + 3 && p[i] == '[' && p[i + 2] == ']' && (p[i + 1] == ' ' || p[i + 1] == 'x' || p[i + 1] == 'X')) {
    done = p[i + 1] != ' ';
    size_t end = i + 3;
    if (end < p.size() && p[end] == ' ') end++;
    return end;
  }
  return 0;
}

std::string trimmed(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
  return s.substr(a, b - a);
}

}  // namespace

std::string truncateUtf8(const std::string& text, const size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  size_t end = maxBytes;
  // Back up over continuation bytes to the start of a character.
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) end--;
  return text.substr(0, end);
}

void NoteDocument::parse(const char* text, const size_t length) {
  paragraphs.clear();
  size_t start = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i < length && text[i] != '\n') continue;
    size_t end = i;
    if (end > start && text[end - 1] == '\r') end--;
    // A final newline does not start another paragraph.
    if (i < length || end > start) paragraphs.emplace_back(text + start, end - start);
    start = i + 1;
  }
}

std::string NoteDocument::serialize() const {
  std::string out;
  out.reserve(bytes());
  for (const auto& p : paragraphs) {
    out += p;
    out += '\n';
  }
  return out;
}

size_t NoteDocument::bytes() const {
  size_t n = 0;
  for (const auto& p : paragraphs) n += p.size() + 1;
  return n;
}

std::string NoteDocument::title(const size_t maxBytes) const {
  for (const auto& p : paragraphs) {
    const std::string body = trimmed(bodyOf(p));
    if (!body.empty()) return truncateUtf8(body, maxBytes);
  }
  return "";
}

bool NoteDocument::set(const size_t index, const std::string& text) {
  if (index >= paragraphs.size()) return false;
  if (text.empty()) return remove(index);
  if (bytes() - paragraphs[index].size() + text.size() > MAX_BYTES) return false;
  paragraphs[index] = text;
  return true;
}

bool NoteDocument::insert(const size_t index, const std::string& text) {
  if (index > paragraphs.size() || bytes() + text.size() + 1 > MAX_BYTES) return false;
  paragraphs.insert(paragraphs.begin() + static_cast<std::ptrdiff_t>(index), text);
  return true;
}

bool NoteDocument::remove(const size_t index) {
  if (index >= paragraphs.size()) return false;
  paragraphs.erase(paragraphs.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

namespace {
std::vector<std::string> splitLines(const std::string& text) {
  NoteDocument parts;
  parts.parse(text.data(), text.size());
  std::vector<std::string> out;
  for (size_t i = 0; i < parts.count(); i++) out.push_back(parts.at(i));
  return out;
}
}  // namespace

int NoteDocument::replaceWithText(const size_t index, const std::string& text) {
  if (index >= paragraphs.size()) return -1;
  if (text.empty()) return remove(index) ? 0 : -1;
  const auto lines = splitLines(text);
  size_t added = 0;
  for (const auto& l : lines) added += l.size() + 1;
  if (bytes() - (paragraphs[index].size() + 1) + added > MAX_BYTES) return -1;
  paragraphs.erase(paragraphs.begin() + static_cast<std::ptrdiff_t>(index));
  paragraphs.insert(paragraphs.begin() + static_cast<std::ptrdiff_t>(index), lines.begin(), lines.end());
  return static_cast<int>(lines.size());
}

int NoteDocument::insertText(const size_t index, const std::string& text) {
  if (index > paragraphs.size()) return -1;
  const auto lines = splitLines(text);
  size_t added = 0;
  for (const auto& l : lines) added += l.size() + 1;
  if (bytes() + added > MAX_BYTES) return -1;
  paragraphs.insert(paragraphs.begin() + static_cast<std::ptrdiff_t>(index), lines.begin(), lines.end());
  return static_cast<int>(lines.size());
}

NoteDocument::Task NoteDocument::taskOf(const std::string& paragraph) {
  bool done = false;
  if (boxPrefix(paragraph, done) == 0) return Task::None;
  return done ? Task::Done : Task::Open;
}

std::string NoteDocument::bodyOf(const std::string& paragraph) {
  bool done = false;
  return paragraph.substr(boxPrefix(paragraph, done));
}

bool NoteDocument::toggleDone(const size_t index) {
  if (index >= paragraphs.size()) return false;
  std::string& p = paragraphs[index];
  bool done = false;
  if (boxPrefix(p, done) == 0) return false;
  const size_t mark = p.find('[') + 1;
  p[mark] = done ? ' ' : 'x';
  return true;
}

bool NoteDocument::setTask(const size_t index, const bool task) {
  if (index >= paragraphs.size()) return false;
  std::string& p = paragraphs[index];
  bool done = false;
  const size_t prefix = boxPrefix(p, done);
  if (task == (prefix > 0)) return true;
  if (task) {
    if (bytes() + 4 > MAX_BYTES) return false;
    p.insert(0, "[ ] ");
  } else {
    p.erase(0, prefix);
  }
  return true;
}

std::string nextNoteName(const std::vector<std::string>& names) {
  int highest = 0;
  for (const auto& name : names) {
    if (name.compare(0, 5, "note-") != 0) continue;
    const int n = atoi(name.c_str() + 5);
    if (n > highest) highest = n;
  }
  char out[24];
  snprintf(out, sizeof(out), "note-%03d.txt", highest + 1);
  return out;
}

std::string titleFromStart(const char* text, const size_t length, const size_t maxBytes) {
  size_t start = 0;
  for (size_t i = 0; i <= length; i++) {
    if (i < length && text[i] != '\n') continue;
    std::string line(text + start, i - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string body = trimmed(NoteDocument::bodyOf(line));
    if (!body.empty()) return truncateUtf8(body, maxBytes);
    start = i + 1;
  }
  return "";
}

}  // namespace notes
