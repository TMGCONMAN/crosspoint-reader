#include <gtest/gtest.h>

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>

#include "Checkers.h"

using namespace games::checkers;

namespace {
uint64_t perft(Position& pos, int depth) {
  Move moves[Position::MAX_MOVES];
  const int n = pos.legalMoves(moves);
  if (depth == 1) return static_cast<uint64_t>(n);
  uint64_t total = 0;
  for (int i = 0; i < n; i++) {
    Undo undo;
    pos.make(moves[i], undo);
    EXPECT_EQ(pos.hash(), pos.computeHash());
    total += perft(pos, depth - 1);
    pos.unmake(moves[i], undo);
  }
  return total;
}
// Squares in standard notation (1..32).
Move plain(int from, int to) {
  Move m;
  m.from = static_cast<uint8_t>(from - 1);
  m.steps = 1;
  m.path[0] = static_cast<uint8_t>(to - 1);
  return m;
}
// A board from (square, piece letter) pairs in standard notation.
std::string board(std::initializer_list<std::pair<int, char>> pieces) {
  std::string out(32, '.');
  for (const auto& [square, piece] : pieces) out[square - 1] = piece;
  return out;
}
}  // namespace

// Published perft counts for the starting position.
TEST(Checkers, PerftStart) {
  Position pos;
  pos.setStart();
  EXPECT_EQ(perft(pos, 1), 7u);
  EXPECT_EQ(perft(pos, 2), 49u);
  EXPECT_EQ(perft(pos, 3), 302u);
  EXPECT_EQ(perft(pos, 4), 1469u);
  EXPECT_EQ(perft(pos, 5), 7361u);
  EXPECT_EQ(perft(pos, 6), 36768u);
  EXPECT_EQ(perft(pos, 7), 179740u);
}

TEST(Checkers, JumpsAreCompulsoryAndChain) {
  Position pos;
  // Black man on 10; white men on 14 and 22 can be jumped in a row (10-17-26).
  ASSERT_TRUE(pos.setup(board({{10, 'b'}, {14, 'w'}, {22, 'w'}, {32, 'w'}}).c_str(), BLACK));
  Move moves[Position::MAX_MOVES];
  const int n = pos.legalMoves(moves);
  ASSERT_EQ(n, 1);
  EXPECT_TRUE(moves[0].jump);
  EXPECT_EQ(moves[0].steps, 2);
  EXPECT_EQ(moves[0].to(), 25);  // square 26
  Undo undo;
  pos.make(moves[0], undo);
  EXPECT_EQ(pos.at(13), EMPTY);
  EXPECT_EQ(pos.at(21), EMPTY);
  EXPECT_EQ(pos.pieces(WHITE), 1);
  pos.unmake(moves[0], undo);
  EXPECT_EQ(pos.pieces(WHITE), 3);
}

TEST(Checkers, CrowningEndsTheJumpAndKingsGoBack) {
  Position pos;
  // White man on 9 jumps 6 to land on 2 and is crowned; it may not go on to
  // jump 7 as a king.
  ASSERT_TRUE(pos.setup(board({{9, 'w'}, {6, 'b'}, {7, 'b'}}).c_str(), WHITE));
  Move moves[Position::MAX_MOVES];
  int n = pos.legalMoves(moves);
  ASSERT_EQ(n, 1);
  EXPECT_EQ(moves[0].steps, 1);
  Undo undo;
  pos.make(moves[0], undo);
  EXPECT_EQ(pos.at(1), WHITE_KING);
  EXPECT_EQ(pos.at(6), BLACK_MAN);
  // A king moves backwards too.
  ASSERT_TRUE(pos.setup(board({{15, 'W'}}).c_str(), WHITE));
  n = pos.legalMoves(moves);
  EXPECT_EQ(n, 4);
}

TEST(Checkers, NoMovesLoses) {
  Game game;
  // Black has no pieces left.
  ASSERT_TRUE(game.setup(board({{1, 'w'}}).c_str(), BLACK));
  EXPECT_EQ(game.status(), Game::Status::WhiteWins);
  // A black man on 28 can only step to 32; a white man there, against the
  // edge, can be neither passed nor jumped.
  ASSERT_TRUE(game.setup(board({{28, 'b'}, {32, 'w'}, {1, 'w'}}).c_str(), BLACK));
  EXPECT_EQ(game.status(), Game::Status::WhiteWins);
  ASSERT_TRUE(game.setup(board({{28, 'b'}, {1, 'w'}}).c_str(), BLACK));
  EXPECT_EQ(game.status(), Game::Status::Playing);
}

TEST(Checkers, PlayUndoSaveRestore) {
  Game game;
  game.reset();
  ASSERT_TRUE(game.play(plain(11, 15)));
  ASSERT_TRUE(game.play(plain(22, 18)));
  EXPECT_FALSE(game.play(plain(9, 13)));  // 15x22 is compulsory
  Move moves[Position::MAX_MOVES];
  ASSERT_EQ(game.legalMoves(moves), 1);
  ASSERT_TRUE(game.play(moves[0]));
  auto state = std::make_unique<uint8_t[]>(Game::STATE_SIZE);
  game.save(state.get());
  Game copy;
  ASSERT_TRUE(copy.restore(state.get()));
  EXPECT_EQ(copy.plies(), 3);
  EXPECT_EQ(copy.position().hash(), game.position().hash());
  ASSERT_TRUE(game.undo());
  EXPECT_EQ(game.plies(), 2);
}

TEST(Checkers, EngineTakesTheDoubleJump) {
  auto engine = std::make_unique<Engine>();
  Game game;
  ASSERT_TRUE(game.setup(board({{10, 'b'}, {14, 'w'}, {22, 'w'}, {32, 'w'}}).c_str(), BLACK));
  Engine::Limits limits;
  limits.maxDepth = 6;
  const Move m = engine->think(game, limits, 1, nullptr, nullptr);
  EXPECT_EQ(m.steps, 2);
}

TEST(Checkers, EngineBeatsAWeakPlayer) {
  auto engine = std::make_unique<Engine>();
  int strongWins = 0;
  for (int g = 0; g < 6; g++) {
    Game game;
    game.reset();
    while (game.status() == Game::Status::Playing) {
      Engine::Limits limits;
      const bool strong = (game.position().sideToMove() == BLACK) == (g % 2 == 0);
      limits.maxDepth = strong ? 6 : 2;
      limits.noise = strong ? 0 : 80;
      limits.maxNodes = 50000;
      const Move m = engine->think(game, limits, static_cast<uint32_t>(g * 101 + game.plies()), nullptr, nullptr);
      ASSERT_TRUE(game.play(m));
    }
    const auto s = game.status();
    if ((s == Game::Status::BlackWins && g % 2 == 0) || (s == Game::Status::WhiteWins && g % 2 == 1)) strongWins++;
  }
  EXPECT_GE(strongWins, 5);
}
