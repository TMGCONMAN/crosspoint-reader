#include "Game2048.h"

#include <cstring>

namespace games {

void Game2048::reset(uint32_t random1, uint32_t random2) {
  memset(cells, 0, sizeof(cells));
  points = 0;
  gain = 0;
  moveCount = 0;
  addRandomTile(random1);
  addRandomTile(random2);
}

bool Game2048::addRandomTile(uint32_t random) {
  int empty = 0;
  for (const auto& row : cells) {
    for (const uint8_t cell : row) empty += cell == 0;
  }
  if (empty == 0) return false;
  int target = static_cast<int>(random % static_cast<uint32_t>(empty));
  // Tile kind from other bits, so it is independent of the position.
  const uint8_t exponent = ((random >> 16) % 10) == 0 ? 2 : 1;
  for (auto& row : cells) {
    for (uint8_t& cell : row) {
      if (cell != 0) continue;
      if (target-- == 0) {
        cell = exponent;
        return true;
      }
    }
  }
  return false;
}

bool Game2048::move(const Direction direction) {
  bool moved = false;
  gain = 0;
  for (int line = 0; line < SIZE; line++) {
    // Cell i of this line, counted from the side the tiles slide towards.
    const auto cell = [&](int i) -> uint8_t& {
      switch (direction) {
        case Direction::Left:
          return cells[line][i];
        case Direction::Right:
          return cells[line][SIZE - 1 - i];
        case Direction::Up:
          return cells[i][line];
        case Direction::Down:
        default:
          return cells[SIZE - 1 - i][line];
      }
    };
    uint8_t packed[SIZE] = {};
    int count = 0;
    for (int i = 0; i < SIZE; i++) {
      if (cell(i) != 0) packed[count++] = cell(i);
    }
    // Merge neighbours once each, front first.
    uint8_t merged[SIZE] = {};
    int out = 0;
    for (int i = 0; i < count; i++) {
      if (i + 1 < count && packed[i] == packed[i + 1] && packed[i] < 31) {
        merged[out] = static_cast<uint8_t>(packed[i] + 1);
        gain += value(merged[out]);
        out++;
        i++;
      } else {
        merged[out++] = packed[i];
      }
    }
    for (int i = 0; i < SIZE; i++) {
      if (cell(i) != merged[i]) {
        cell(i) = merged[i];
        moved = true;
      }
    }
  }
  if (moved) {
    points += gain;
    moveCount++;
  } else {
    gain = 0;
  }
  return moved;
}

bool Game2048::canMove() const {
  for (int r = 0; r < SIZE; r++) {
    for (int c = 0; c < SIZE; c++) {
      if (cells[r][c] == 0) return true;
      if (c + 1 < SIZE && cells[r][c] == cells[r][c + 1]) return true;
      if (r + 1 < SIZE && cells[r][c] == cells[r + 1][c]) return true;
    }
  }
  return false;
}

bool Game2048::reachedWin() const {
  for (const auto& row : cells) {
    for (const uint8_t cell : row) {
      if (cell >= WIN_EXPONENT) return true;
    }
  }
  return false;
}

namespace {
void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
}  // namespace

void Game2048::save(uint8_t* out) const {
  memcpy(out, cells, SIZE * SIZE);
  putU32(out + SIZE * SIZE, points);
  putU32(out + SIZE * SIZE + 4, moveCount);
}

bool Game2048::restore(const uint8_t* in) {
  for (int i = 0; i < SIZE * SIZE; i++) {
    if (in[i] > 20) return false;  // not a board this game wrote
  }
  memcpy(cells, in, SIZE * SIZE);
  points = getU32(in + SIZE * SIZE);
  moveCount = getU32(in + SIZE * SIZE + 4);
  gain = 0;
  return true;
}

}  // namespace games
