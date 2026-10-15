#include <gtest/gtest.h>

#include "Sudoku.h"

using games::Sudoku;

namespace {

bool validSolution(const Sudoku& game) {
  for (int unit = 0; unit < 9; unit++) {
    uint16_t row = 0, col = 0, box = 0;
    for (int k = 0; k < 9; k++) {
      row |= 1u << game.solutionAt(unit * 9 + k);
      col |= 1u << game.solutionAt(k * 9 + unit);
      box |= 1u << game.solutionAt((unit / 3) * 27 + (unit % 3) * 3 + (k / 3) * 9 + k % 3);
    }
    if (row != 0x3FE || col != 0x3FE || box != 0x3FE) return false;
  }
  return true;
}

int givens(const Sudoku& game) {
  int n = 0;
  for (int i = 0; i < 81; i++) n += game.isGiven(i);
  return n;
}

}  // namespace

TEST(Sudoku, GeneratedPuzzlesHaveOneSolution) {
  for (const auto level : {Sudoku::Difficulty::Easy, Sudoku::Difficulty::Medium, Sudoku::Difficulty::Hard}) {
    for (uint32_t seed = 1; seed <= 5; seed++) {
      Sudoku game;
      game.generate(level, seed * 7919);
      ASSERT_TRUE(validSolution(game));
      uint8_t grid[81];
      for (int i = 0; i < 81; i++) {
        grid[i] = game.value(i);
        if (game.isGiven(i)) {
          ASSERT_EQ(game.value(i), game.solutionAt(i));
        }
      }
      EXPECT_EQ(Sudoku::countSolutions(grid, 2), 1);
    }
  }
}

TEST(Sudoku, HarderMeansFewerClues) {
  Sudoku easy, hard;
  easy.generate(Sudoku::Difficulty::Easy, 42);
  hard.generate(Sudoku::Difficulty::Hard, 42);
  EXPECT_EQ(givens(easy), 40);
  EXPECT_LT(givens(hard), 33);
}

TEST(Sudoku, PlaceNotesConflictsAndUndo) {
  Sudoku game;
  game.generate(Sudoku::Difficulty::Easy, 3);
  int a = 0;
  while (game.isGiven(a)) a++;
  int b = a + 1;  // another empty cell in the same row
  while (b % 9 != 0 && game.isGiven(b)) b++;
  ASSERT_LT(b / 9, a / 9 + 1);
  ASSERT_FALSE(game.isGiven(b));

  ASSERT_TRUE(game.toggleNote(b, 5));
  EXPECT_EQ(game.notes(b), 1u << 4);
  ASSERT_TRUE(game.place(a, 5));
  EXPECT_EQ(game.notes(b), 0u);  // the 5 is gone from the peer's notes
  ASSERT_TRUE(game.place(b, 5));
  EXPECT_TRUE(game.conflicts(a));
  EXPECT_TRUE(game.conflicts(b));

  ASSERT_TRUE(game.undo());  // b back to empty
  EXPECT_EQ(game.value(b), 0);
  ASSERT_TRUE(game.undo());  // a back, and b's note returns
  EXPECT_EQ(game.value(a), 0);
  EXPECT_EQ(game.notes(b), 1u << 4);

  int given = 0;
  while (!game.isGiven(given)) given++;
  EXPECT_FALSE(game.place(given, 1));
}

TEST(Sudoku, SolvingAndSaving) {
  Sudoku game;
  game.generate(Sudoku::Difficulty::Medium, 99);
  for (int i = 0; i < 81; i++) {
    if (!game.isGiven(i) && i != 80) game.place(i, game.solutionAt(i));
  }
  uint8_t state[Sudoku::STATE_SIZE];
  game.save(state);
  Sudoku copy;
  ASSERT_TRUE(copy.restore(state));
  EXPECT_EQ(copy.difficulty(), Sudoku::Difficulty::Medium);
  if (!copy.isGiven(80)) {
    EXPECT_FALSE(copy.solved());
    copy.place(80, copy.solutionAt(80));
  }
  EXPECT_TRUE(copy.solved());
  state[81] = 12;
  EXPECT_FALSE(copy.restore(state));
}
