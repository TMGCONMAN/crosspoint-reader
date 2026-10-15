#include <gtest/gtest.h>

#include <memory>

#include "Chess.h"

using namespace games::chess;

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
uint64_t perftFen(const char* fen, int depth) {
  Position pos;
  EXPECT_TRUE(pos.setFen(fen));
  return perft(pos, depth);
}
Move parse(const char* text) {
  Move m;
  m.from = static_cast<uint8_t>(squareAt(text[0] - 'a', text[1] - '1'));
  m.to = static_cast<uint8_t>(squareAt(text[2] - 'a', text[3] - '1'));
  if (text[4] == 'q') m.promo = QUEEN;
  if (text[4] == 'n') m.promo = KNIGHT;
  return m;
}
}  // namespace

// Published perft totals (chessprogramming.org "Perft Results").
TEST(Chess, PerftStart) {
  Position pos;
  pos.setStart();
  EXPECT_EQ(perft(pos, 1), 20u);
  EXPECT_EQ(perft(pos, 2), 400u);
  EXPECT_EQ(perft(pos, 3), 8902u);
  EXPECT_EQ(perft(pos, 4), 197281u);
}

TEST(Chess, PerftKiwipete) {
  const char* fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
  EXPECT_EQ(perftFen(fen, 1), 48u);
  EXPECT_EQ(perftFen(fen, 2), 2039u);
  EXPECT_EQ(perftFen(fen, 3), 97862u);
}

TEST(Chess, PerftEndgameAndPromotions) {
  EXPECT_EQ(perftFen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4), 43238u);
  EXPECT_EQ(perftFen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3), 9467u);
  EXPECT_EQ(perftFen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3), 62379u);
}

TEST(Chess, FoolsMateIsCheckmate) {
  Game game;
  game.reset();
  for (const char* m : {"f2f3", "e7e5", "g2g4", "d8h4"}) ASSERT_TRUE(game.play(parse(m))) << m;
  EXPECT_EQ(game.status(), Game::Status::Checkmate);
  EXPECT_FALSE(game.play(parse("e2e4")));
  ASSERT_TRUE(game.undo());
  EXPECT_EQ(game.status(), Game::Status::Playing);
}

TEST(Chess, StalemateAndRepetition) {
  Game game;
  ASSERT_TRUE(game.setFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"));
  EXPECT_EQ(game.status(), Game::Status::Stalemate);
  game.reset();
  for (int i = 0; i < 2; i++) {
    for (const char* m : {"g1f3", "g8f6", "f3g1", "f6g8"}) ASSERT_TRUE(game.play(parse(m)));
  }
  EXPECT_EQ(game.status(), Game::Status::Repetition);
}

TEST(Chess, IllegalMovesRefused) {
  Game game;
  game.reset();
  EXPECT_FALSE(game.play(parse("e2e5")));
  EXPECT_FALSE(game.play(parse("e1e2")));  // own pawn there
  EXPECT_TRUE(game.play(parse("e2e4")));
  EXPECT_FALSE(game.play(parse("e4e5")));  // Black to move
}

TEST(Chess, SaveRestoreReplaysMoves) {
  Game game;
  game.reset();
  for (const char* m : {"e2e4", "d7d5", "e4d5", "g8f6", "f1b5", "c7c6", "g1f3", "c6b5", "e1g1"}) {
    ASSERT_TRUE(game.play(parse(m))) << m;
  }
  auto state = std::make_unique<uint8_t[]>(Game::STATE_SIZE);
  game.save(state.get());
  Game copy;
  ASSERT_TRUE(copy.restore(state.get()));
  EXPECT_EQ(copy.plies(), 9);
  EXPECT_EQ(copy.position().hash(), game.position().hash());
  EXPECT_EQ(copy.position().at(squareAt(6, 0)), makePiece(WHITE, KING));  // castled
  state[2] ^= 0x3F;                                                       // a corrupt first move
  EXPECT_FALSE(copy.restore(state.get()));
}

TEST(Chess, EngineFindsMateInOne) {
  auto engine = std::make_unique<Engine>();
  Game game;
  ASSERT_TRUE(game.setFen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1"));
  Engine::Limits limits;
  limits.maxDepth = 3;
  const Move m = engine->think(game, limits, 1, nullptr, nullptr);
  EXPECT_EQ(m.from, squareAt(3, 0));
  EXPECT_EQ(m.to, squareAt(3, 7));
}

TEST(Chess, EngineTakesHangingQueenAndAvoidsMate) {
  auto engine = std::make_unique<Engine>();
  Game game;
  // The knight on f3 can take the black queen on d4.
  ASSERT_TRUE(game.setFen("rnb1kbnr/pppp1ppp/8/4p3/3q4/5N2/PPPPPPPP/RNBQKB1R w KQkq - 0 1"));
  Engine::Limits limits;
  limits.maxDepth = 4;
  Move m = engine->think(game, limits, 1, nullptr, nullptr);
  EXPECT_EQ(m.to, squareAt(3, 3)) << "should capture on d4";
  // Black to move must stop Qxf7 mate (Scholar's mate threat).
  ASSERT_TRUE(game.setFen("r1bqkbnr/pppp1ppp/2n5/4p2Q/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 3 3"));
  m = engine->think(game, limits, 1, nullptr, nullptr);
  ASSERT_TRUE(game.play(m));
  Move replies[Position::MAX_MOVES];
  const int n = game.legalMoves(replies);
  for (int i = 0; i < n; i++) {
    ASSERT_TRUE(game.play(replies[i]));
    EXPECT_NE(game.status(), Game::Status::Checkmate);
    game.undo();
  }
}

TEST(Chess, EngineStopsAtNodeLimitWithALegalMove) {
  auto engine = std::make_unique<Engine>();
  Game game;
  game.reset();
  Engine::Limits limits;
  limits.maxDepth = 30;
  limits.maxNodes = 20000;
  const Move m = engine->think(game, limits, 7, nullptr, nullptr);
  EXPECT_TRUE(game.play(m));
  EXPECT_LE(engine->nodeCount(), 21000u);
  limits.maxNodes = 0;
  limits.maxDepth = 2;
  limits.noise = 60;
  EXPECT_TRUE(game.play(engine->think(game, limits, 9, nullptr, nullptr)));
}

// The Chess app's Beginner and Easy settings (ChessActivity LEVEL_SPECS).
TEST(Chess, EasyBeatsBeginner) {
  auto engine = std::make_unique<Engine>();
  double easyScore = 0;
  constexpr int GAMES = 8;
  for (int g = 0; g < GAMES; g++) {
    Game game;
    game.reset();
    const bool easyWhite = g % 2 == 0;
    while (game.status() == Game::Status::Playing && game.plies() < 300) {
      const bool easyMoves = (game.position().sideToMove() == WHITE) == easyWhite;
      Engine::Limits limits;
      limits.maxDepth = 1;
      limits.noise = easyMoves ? 120 : 250;
      limits.quiescence = easyMoves;
      limits.blunderPercent = easyMoves ? 8 : 25;
      const Move m =
          engine->think(game, limits, static_cast<uint32_t>(g * 7919 + game.plies() * 31 + 1), nullptr, nullptr);
      ASSERT_TRUE(game.play(m));
    }
    if (game.status() == Game::Status::Checkmate) {
      const bool whiteMated = game.position().sideToMove() == WHITE;
      if (whiteMated != easyWhite) easyScore += 1;
    } else {
      easyScore += 0.5;
    }
  }
  EXPECT_GE(easyScore, GAMES * 0.75);
}
