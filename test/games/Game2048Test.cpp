#include <gtest/gtest.h>

#include "Game2048.h"

using games::Game2048;
using Dir = Game2048::Direction;

namespace {
// Rows of exponents, top to bottom.
void load(Game2048& game, const uint8_t (&rows)[4][4]) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) game.set(r, c, rows[r][c]);
  }
}
}  // namespace

TEST(Game2048, ResetPlacesTwoTiles) {
  Game2048 game;
  game.reset(3, 7);
  int tiles = 0;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) tiles += game.at(r, c) != 0;
  }
  EXPECT_EQ(tiles, 2);
  EXPECT_EQ(game.score(), 0u);
}

TEST(Game2048, SlideLeftMergesOncePerPair) {
  Game2048 game;
  load(game, {{1, 1, 1, 1}, {1, 0, 1, 0}, {2, 1, 1, 0}, {0, 0, 0, 3}});
  ASSERT_TRUE(game.move(Dir::Left));
  EXPECT_EQ(game.at(0, 0), 2);
  EXPECT_EQ(game.at(0, 1), 2);
  EXPECT_EQ(game.at(0, 2), 0);
  EXPECT_EQ(game.at(1, 0), 2);
  EXPECT_EQ(game.at(2, 0), 2);  // 4 stays: the 2s merge into a new 4 next to it
  EXPECT_EQ(game.at(2, 1), 2);
  EXPECT_EQ(game.at(3, 0), 3);
  // 4 + 4 + 4 + 4 points.
  EXPECT_EQ(game.score(), 16u);
}

TEST(Game2048, SlideRightAndDown) {
  Game2048 game;
  load(game, {{1, 1, 2, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
  ASSERT_TRUE(game.move(Dir::Right));
  EXPECT_EQ(game.at(0, 3), 2);
  EXPECT_EQ(game.at(0, 2), 2);
  EXPECT_EQ(game.at(0, 1), 0);
  ASSERT_TRUE(game.move(Dir::Down));
  EXPECT_EQ(game.at(3, 3), 2);
  EXPECT_EQ(game.at(3, 2), 2);
}

TEST(Game2048, ThreeEqualMergesTheFrontPair) {
  Game2048 game;
  load(game, {{0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}});
  ASSERT_TRUE(game.move(Dir::Up));
  EXPECT_EQ(game.at(0, 0), 2);
  EXPECT_EQ(game.at(1, 0), 1);
  EXPECT_EQ(game.at(2, 0), 0);
}

TEST(Game2048, NoMoveChangesNothing) {
  Game2048 game;
  load(game, {{1, 2, 3, 4}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
  EXPECT_FALSE(game.move(Dir::Left));
  EXPECT_FALSE(game.move(Dir::Up));
  EXPECT_EQ(game.moves(), 0u);
}

TEST(Game2048, GameOverAndWin) {
  Game2048 game;
  load(game, {{1, 2, 1, 2}, {2, 1, 2, 1}, {1, 2, 1, 2}, {2, 1, 2, 1}});
  EXPECT_FALSE(game.canMove());
  EXPECT_FALSE(game.addRandomTile(5));
  EXPECT_FALSE(game.reachedWin());
  game.set(0, 0, 11);
  EXPECT_TRUE(game.reachedWin());
}

TEST(Game2048, NewTileGoesToAnEmptyCell) {
  Game2048 game;
  load(game, {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 0}, {1, 1, 1, 1}});
  ASSERT_TRUE(game.addRandomTile(123456));
  EXPECT_NE(game.at(2, 3), 0);
}

TEST(Game2048, SaveAndRestore) {
  Game2048 game;
  load(game, {{1, 2, 0, 0}, {0, 11, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 3}});
  game.move(Dir::Left);
  uint8_t state[Game2048::STATE_SIZE];
  game.save(state);
  Game2048 copy;
  ASSERT_TRUE(copy.restore(state));
  EXPECT_EQ(copy.at(1, 0), 11);
  EXPECT_EQ(copy.score(), game.score());
  EXPECT_EQ(copy.moves(), 1u);
  state[0] = 99;
  EXPECT_FALSE(copy.restore(state));
}
