#include "TextWrap.h"

namespace textwrap {

namespace {
int nextChar(const std::string& s, int i) {
  i++;
  while (i < static_cast<int>(s.size()) && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) i++;
  return i;
}
}  // namespace

void wrap(const std::string& text, const int width, const MeasureFn measure, void* user, std::vector<Line>& out) {
  out.clear();
  const int n = static_cast<int>(text.size());
  int start = 0;
  while (true) {
    const size_t found = text.find('\n', static_cast<size_t>(start));
    const int hard = found == std::string::npos ? n : static_cast<int>(found);
    int i = start;
    int lastSpace = -1;
    int used = 0;
    while (i < hard) {
      const int next = nextChar(text, i);
      const int w = measure(text.data() + i, next - i, user);
      if (used + w > width && i > start) break;
      used += w;
      if (text[i] == ' ') lastSpace = next;
      i = next;
    }
    if (i >= hard) {
      out.push_back(Line{start, hard, hard < n ? hard + 1 : n});
      if (hard >= n) return;
      start = hard + 1;
      if (start >= n) {
        out.push_back(Line{n, n, n});
        return;
      }
      continue;
    }
    // Break after the last space that fit, or mid-word if there was none.
    const int brk = lastSpace > start ? lastSpace : i;
    out.push_back(Line{start, brk, brk});
    start = brk;
  }
}

int lineOfCursor(const std::vector<Line>& lines, const size_t cursor) {
  const int c = static_cast<int>(cursor);
  for (size_t i = 0; i + 1 < lines.size(); i++) {
    if (c < lines[i].next) return static_cast<int>(i);
  }
  return static_cast<int>(lines.size()) - 1;
}

}  // namespace textwrap
