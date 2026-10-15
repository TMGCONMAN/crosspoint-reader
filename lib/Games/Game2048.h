#pragma once

#include <cstddef>
#include <cstdint>

// The 2048 sliding-tile game: a 4x4 board of tiles that slide together in
// one of four directions, two equal tiles merging into their sum. Tiles are
// stored as exponents (1 = 2, 2 = 4, ... 11 = 2048), 0 for an empty cell.
// Holds no randomness of its own: the caller passes random numbers in, so
// the same code runs on the device and in host tests.
namespace games {

class Game2048 {
 public:
  static constexpr int SIZE = 4;
  static constexpr uint8_t WIN_EXPONENT = 11;  // 2048
  enum class Direction : uint8_t { Up, Down, Left, Right };

  // Clears the board and places two starting tiles.
  void reset(uint32_t random1, uint32_t random2);
  // Slides the tiles; false (and nothing changes) when no tile can move.
  bool move(Direction direction);
  // A new 2 (nine times in ten) or 4 in a random empty cell. False when full.
  bool addRandomTile(uint32_t random);
  bool canMove() const;
  // True once a 2048 tile exists; play may continue past it.
  bool reachedWin() const;

  uint8_t at(int row, int col) const { return cells[row][col]; }
  void set(int row, int col, uint8_t exponent) { cells[row][col] = exponent; }
  uint32_t score() const { return points; }
  // The points the last move earned, for the screen.
  uint32_t lastGain() const { return gain; }
  uint32_t moves() const { return moveCount; }

  // A fixed-size snapshot for the SD card; restore() checks it.
  static constexpr size_t STATE_SIZE = SIZE * SIZE + 8;
  void save(uint8_t* out) const;
  bool restore(const uint8_t* in);

  // The value a tile exponent shows: 2, 4, 8, ...
  static uint32_t value(uint8_t exponent) { return exponent == 0 ? 0 : (1u << exponent); }

 private:
  uint8_t cells[SIZE][SIZE] = {};
  uint32_t points = 0;
  uint32_t gain = 0;
  uint32_t moveCount = 0;
};

}  // namespace games
