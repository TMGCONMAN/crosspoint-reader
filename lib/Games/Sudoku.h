#pragma once

#include <cstddef>
#include <cstdint>

// Sudoku: puzzle generation with a unique solution, the player's entries and
// pencil marks, conflict checks and undo. Cells are numbered 0..80 row by
// row; a value of 0 is empty. The generator takes a seed, so the same code
// runs on the device and in host tests.
namespace games {

class Sudoku {
 public:
  static constexpr int CELLS = 81;
  enum class Difficulty : uint8_t { Easy, Medium, Hard };

  // A new puzzle. Removing clues stops at the target or when removing more
  // would allow a second solution.
  void generate(Difficulty difficulty, uint32_t seed);

  Difficulty difficulty() const { return level; }
  bool isGiven(int cell) const { return given[cell] != 0; }
  uint8_t value(int cell) const { return values[cell]; }
  uint16_t notes(int cell) const { return marks[cell]; }  // bit d-1 set: d pencilled in
  uint8_t solutionAt(int cell) const { return solution[cell]; }

  // Places or clears (digit 0) a value; also clears the cell's notes and
  // that digit from the notes of its row, column and box. False for a given.
  bool place(int cell, uint8_t digit);
  // Pencils a digit in or out of an empty cell.
  bool toggleNote(int cell, uint8_t digit);
  bool undo();
  bool canUndo() const { return undoCount > 0; }

  // Another cell in the row, column or box holds the same digit.
  bool conflicts(int cell) const;
  // Every cell filled and matching the solution.
  bool solved() const;
  int filledCount() const;

  // Fixed-size snapshot for the SD card; restore() checks it. Undo history
  // is not kept.
  static constexpr size_t STATE_SIZE = CELLS * 5 + 1;
  void save(uint8_t* out) const;
  bool restore(const uint8_t* in);

  // Solutions of grid (0 = empty), counted up to limit. solved, when given,
  // receives the first one. For the generator and tests.
  static int countSolutions(const uint8_t* grid, int limit, uint8_t* solved = nullptr);

 private:
  static constexpr int UNDO_DEPTH = 64;

  // Saves the cell's state and returns the step, to which a placement adds
  // the peers whose notes it clears.
  struct Undo;
  Undo& pushUndo(int cell);

  uint8_t given[CELLS] = {};
  uint8_t values[CELLS] = {};
  uint16_t marks[CELLS] = {};
  uint8_t solution[CELLS] = {};
  Difficulty level = Difficulty::Easy;

  // One undo step: the cell's value and notes before, plus the peers whose
  // notes a placement cleared.
  struct Undo {
    uint8_t cell = 0;
    uint8_t value = 0;
    uint16_t notes = 0;
    uint8_t clearedDigit = 0;       // 0 = none
    uint8_t clearedPeers[20] = {};  // cells whose note for clearedDigit was removed
    uint8_t clearedCount = 0;
  };
  Undo history[UNDO_DEPTH];
  int undoHead = 0;
  int undoCount = 0;
};

}  // namespace games
