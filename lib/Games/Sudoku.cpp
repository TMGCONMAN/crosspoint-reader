#include "Sudoku.h"

#include <cstring>

namespace games {

namespace {

// Small fast generator; the seed fixes the puzzle.
struct Rng {
  uint32_t state;
  explicit Rng(uint32_t seed) : state(seed ? seed : 0x9E3779B9u) {}
  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  int below(int n) { return static_cast<int>(next() % static_cast<uint32_t>(n)); }
};

inline int rowOf(int cell) { return cell / 9; }
inline int colOf(int cell) { return cell % 9; }
inline int boxOf(int cell) { return (cell / 27) * 3 + (cell % 9) / 3; }

// The 20 cells sharing a row, column or box with cell.
int peersOf(int cell, uint8_t* out) {
  int n = 0;
  for (int i = 0; i < 81; i++) {
    if (i == cell) continue;
    if (rowOf(i) == rowOf(cell) || colOf(i) == colOf(cell) || boxOf(i) == boxOf(cell))
      out[n++] = static_cast<uint8_t>(i);
  }
  return n;
}

// Backtracking over bitmasks, the cell with fewest candidates first.
struct Solver {
  uint8_t grid[81];
  uint16_t rows[9], cols[9], boxes[9];
  int limit;
  int found = 0;
  uint8_t* first;
  Rng* rng;  // shuffles candidate order when filling a fresh grid

  bool init(const uint8_t* in) {
    memcpy(grid, in, 81);
    memset(rows, 0, sizeof(rows));
    memset(cols, 0, sizeof(cols));
    memset(boxes, 0, sizeof(boxes));
    for (int i = 0; i < 81; i++) {
      if (grid[i] == 0) continue;
      const uint16_t bit = static_cast<uint16_t>(1u << (grid[i] - 1));
      if ((rows[rowOf(i)] | cols[colOf(i)] | boxes[boxOf(i)]) & bit) return false;
      rows[rowOf(i)] |= bit;
      cols[colOf(i)] |= bit;
      boxes[boxOf(i)] |= bit;
    }
    return true;
  }

  void search() {
    if (found >= limit) return;
    int best = -1;
    int bestCount = 10;
    uint16_t bestFree = 0;
    for (int i = 0; i < 81; i++) {
      if (grid[i] != 0) continue;
      const uint16_t free = static_cast<uint16_t>(~(rows[rowOf(i)] | cols[colOf(i)] | boxes[boxOf(i)]) & 0x1FF);
      const int count = __builtin_popcount(free);
      if (count < bestCount) {
        best = i;
        bestCount = count;
        bestFree = free;
        if (count <= 1) break;
      }
    }
    if (best < 0) {
      if (found == 0 && first) memcpy(first, grid, 81);
      found++;
      return;
    }
    uint8_t order[9];
    int n = 0;
    for (int d = 0; d < 9; d++) {
      if (bestFree & (1u << d)) order[n++] = static_cast<uint8_t>(d);
    }
    if (rng) {
      for (int i = n - 1; i > 0; i--) {
        const int j = rng->below(i + 1);
        const uint8_t t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
    for (int k = 0; k < n && found < limit; k++) {
      const int d = order[k];
      const uint16_t bit = static_cast<uint16_t>(1u << d);
      grid[best] = static_cast<uint8_t>(d + 1);
      rows[rowOf(best)] |= bit;
      cols[colOf(best)] |= bit;
      boxes[boxOf(best)] |= bit;
      search();
      rows[rowOf(best)] &= static_cast<uint16_t>(~bit);
      cols[colOf(best)] &= static_cast<uint16_t>(~bit);
      boxes[boxOf(best)] &= static_cast<uint16_t>(~bit);
      grid[best] = 0;
    }
  }
};

int targetClues(Sudoku::Difficulty difficulty) {
  switch (difficulty) {
    case Sudoku::Difficulty::Easy:
      return 40;
    case Sudoku::Difficulty::Medium:
      return 32;
    case Sudoku::Difficulty::Hard:
    default:
      return 25;
  }
}

}  // namespace

int Sudoku::countSolutions(const uint8_t* grid, int limit, uint8_t* solved) {
  Solver solver;
  if (!solver.init(grid)) return 0;
  solver.limit = limit;
  solver.first = solved;
  solver.rng = nullptr;
  solver.search();
  return solver.found;
}

void Sudoku::generate(const Difficulty difficulty, const uint32_t seed) {
  Rng rng(seed);
  // A random full grid.
  Solver filler;
  uint8_t empty[CELLS] = {};
  filler.init(empty);
  filler.limit = 1;
  filler.first = solution;
  filler.rng = &rng;
  filler.search();

  // Take clues away in random order while the solution stays unique.
  uint8_t puzzle[CELLS];
  memcpy(puzzle, solution, CELLS);
  uint8_t order[CELLS];
  for (int i = 0; i < CELLS; i++) order[i] = static_cast<uint8_t>(i);
  for (int i = CELLS - 1; i > 0; i--) {
    const int j = rng.below(i + 1);
    const uint8_t t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  int clues = CELLS;
  const int target = targetClues(difficulty);
  for (int k = 0; k < CELLS && clues > target; k++) {
    const int cell = order[k];
    const uint8_t keep = puzzle[cell];
    puzzle[cell] = 0;
    if (countSolutions(puzzle, 2) != 1) {
      puzzle[cell] = keep;
    } else {
      clues--;
    }
  }

  level = difficulty;
  for (int i = 0; i < CELLS; i++) {
    given[i] = puzzle[i] != 0;
    values[i] = puzzle[i];
    marks[i] = 0;
  }
  undoHead = 0;
  undoCount = 0;
}

Sudoku::Undo& Sudoku::pushUndo(const int cell) {
  Undo& step = history[undoHead];
  undoHead = (undoHead + 1) % UNDO_DEPTH;
  if (undoCount < UNDO_DEPTH) undoCount++;
  step = Undo{};
  step.cell = static_cast<uint8_t>(cell);
  step.value = values[cell];
  step.notes = marks[cell];
  return step;
}

bool Sudoku::place(const int cell, const uint8_t digit) {
  if (cell < 0 || cell >= CELLS || given[cell] || digit > 9) return false;
  if (values[cell] == digit && (digit != 0 || marks[cell] == 0)) return false;
  Undo& step = pushUndo(cell);
  values[cell] = digit;
  marks[cell] = 0;
  if (digit != 0) {
    // The digit is no longer a candidate for the peers.
    const uint16_t bit = static_cast<uint16_t>(1u << (digit - 1));
    uint8_t peers[20];
    const int n = peersOf(cell, peers);
    for (int i = 0; i < n; i++) {
      if (marks[peers[i]] & bit) {
        marks[peers[i]] &= static_cast<uint16_t>(~bit);
        step.clearedPeers[step.clearedCount++] = peers[i];
      }
    }
    if (step.clearedCount > 0) step.clearedDigit = digit;
  }
  return true;
}

bool Sudoku::toggleNote(const int cell, const uint8_t digit) {
  if (cell < 0 || cell >= CELLS || given[cell] || values[cell] != 0 || digit < 1 || digit > 9) return false;
  pushUndo(cell);
  marks[cell] ^= static_cast<uint16_t>(1u << (digit - 1));
  return true;
}

bool Sudoku::undo() {
  if (undoCount == 0) return false;
  undoHead = (undoHead + UNDO_DEPTH - 1) % UNDO_DEPTH;
  undoCount--;
  const Undo& step = history[undoHead];
  values[step.cell] = step.value;
  marks[step.cell] = step.notes;
  if (step.clearedDigit != 0) {
    const uint16_t bit = static_cast<uint16_t>(1u << (step.clearedDigit - 1));
    for (int i = 0; i < step.clearedCount; i++) marks[step.clearedPeers[i]] |= bit;
  }
  return true;
}

bool Sudoku::conflicts(const int cell) const {
  const uint8_t v = values[cell];
  if (v == 0) return false;
  uint8_t peers[20];
  const int n = peersOf(cell, peers);
  for (int i = 0; i < n; i++) {
    if (values[peers[i]] == v) return true;
  }
  return false;
}

bool Sudoku::solved() const {
  for (int i = 0; i < CELLS; i++) {
    if (values[i] != solution[i]) return false;
  }
  return true;
}

int Sudoku::filledCount() const {
  int n = 0;
  for (const uint8_t v : values) n += v != 0;
  return n;
}

void Sudoku::save(uint8_t* out) const {
  memcpy(out, given, CELLS);
  memcpy(out + CELLS, values, CELLS);
  memcpy(out + 2 * CELLS, solution, CELLS);
  for (int i = 0; i < CELLS; i++) {
    out[3 * CELLS + 2 * i] = static_cast<uint8_t>(marks[i]);
    out[3 * CELLS + 2 * i + 1] = static_cast<uint8_t>(marks[i] >> 8);
  }
  out[5 * CELLS] = static_cast<uint8_t>(level);
}

bool Sudoku::restore(const uint8_t* in) {
  for (int i = 0; i < CELLS; i++) {
    if (in[i] > 1 || in[CELLS + i] > 9 || in[2 * CELLS + i] < 1 || in[2 * CELLS + i] > 9) return false;
  }
  if (in[5 * CELLS] > static_cast<uint8_t>(Difficulty::Hard)) return false;
  memcpy(given, in, CELLS);
  memcpy(values, in + CELLS, CELLS);
  memcpy(solution, in + 2 * CELLS, CELLS);
  for (int i = 0; i < CELLS; i++) {
    marks[i] = static_cast<uint16_t>((in[3 * CELLS + 2 * i] | (in[3 * CELLS + 2 * i + 1] << 8)) & 0x1FF);
  }
  level = static_cast<Difficulty>(in[5 * CELLS]);
  undoHead = 0;
  undoCount = 0;
  return true;
}

}  // namespace games
