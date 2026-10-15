#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Word wrapping for multi-line text fields: whole words while they fit, a
// word wider than the line split by characters, and a hard break at '\n'.
namespace textwrap {

// text[start, end) is drawn; the next line starts at next (end + 1 after a
// newline, end for a soft wrap). A soft-wrapped line keeps its trailing space.
struct Line {
  int start;
  int end;
  int next;
};

// The width of one character (UTF-8 bytes text[0, length)).
using MeasureFn = int (*)(const char* text, int length, void* user);

// Always at least one line; a newline at the end opens an empty last line.
void wrap(const std::string& text, int width, MeasureFn measure, void* user, std::vector<Line>& out);
// The line a cursor (byte offset) sits on.
int lineOfCursor(const std::vector<Line>& lines, size_t cursor);

}  // namespace textwrap
