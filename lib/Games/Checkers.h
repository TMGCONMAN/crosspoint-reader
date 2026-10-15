#pragma once

#include <cstddef>
#include <cstdint>

// American checkers (English draughts) and a small computer player. The 32
// dark squares are numbered row by row from the top left, as in standard
// notation (square n is index n - 1). Black starts at the top (0..11) and
// moves first; White starts at the bottom (20..31).
namespace games::checkers {

constexpr int BLACK = 0;
constexpr int WHITE = 1;
constexpr int SQUARES = 32;
enum Piece : uint8_t { EMPTY = 0, BLACK_MAN = 1, BLACK_KING = 2, WHITE_MAN = 3, WHITE_KING = 4 };

inline int colorOf(uint8_t piece) { return piece >= WHITE_MAN ? WHITE : BLACK; }
inline bool isKing(uint8_t piece) { return piece == BLACK_KING || piece == WHITE_KING; }
inline int rowOf(int square) { return square / 4; }
inline int columnOf(int square) { return 2 * (square % 4) + ((square / 4) % 2 == 0 ? 1 : 0); }
// The square at a row and column, or -1 for a light square.
inline int squareAt(int row, int column) {
  if (row < 0 || row > 7 || column < 0 || column > 7 || (row + column) % 2 == 0) return -1;
  return row * 4 + column / 2;
}

struct Move {
  static constexpr int MAX_STEPS = 12;
  uint8_t from = 0;
  uint8_t steps = 0;  // squares landed on; one for a plain move
  bool jump = false;
  uint8_t path[MAX_STEPS] = {};

  uint8_t to() const { return steps ? path[steps - 1] : from; }
  bool sameAs(const Move& other) const;
  // The squares of the pieces this move jumps, in order. Returns the count.
  int captured(uint8_t* out) const;
};

struct Undo {
  uint64_t key = 0;
  uint8_t moved = 0;
  uint8_t quiet = 0;
  uint8_t captured[Move::MAX_STEPS] = {};
};

class Position {
 public:
  static constexpr int MAX_MOVES = 128;

  void setStart();
  // Tests: 32 characters (b B w W .) for the squares, then the side to move.
  bool setup(const char* squares, int sideToMove);

  uint8_t at(int square) const { return board[square]; }
  int sideToMove() const { return side; }
  int quietPlies() const { return quiet; }  // since a capture or a man moved
  uint64_t hash() const { return key; }
  uint64_t computeHash() const;
  int pieces(int color) const;

  // Jumps are compulsory: when any exists, only jumps are returned.
  int legalMoves(Move* out) const;
  void make(const Move& move, Undo& undo);
  void unmake(const Move& move, const Undo& undo);

  // From the side to move's point of view.
  int evaluate() const;

 private:
  void addJumps(int from, uint8_t piece, int at, Move& current, uint32_t taken, Move* out, int& n) const;

  uint8_t board[SQUARES] = {};
  uint8_t side = BLACK;
  uint8_t quiet = 0;
  uint64_t key = 0;
};

class Game {
 public:
  static constexpr int MAX_PLIES = 600;
  // 40 moves each without a capture or a man moving is a draw.
  static constexpr int QUIET_DRAW_PLIES = 80;
  enum class Status : uint8_t { Playing, BlackWins, WhiteWins, QuietDraw, Repetition, TooLong };

  void reset();
  bool setup(const char* squares, int sideToMove);  // tests; not saved
  const Position& position() const { return pos; }
  int legalMoves(Move* out) const { return pos.legalMoves(out); }
  bool play(const Move& move);
  bool undo();
  int plies() const { return count; }
  Move moveAt(int ply) const { return moves[ply]; }
  Status status() const;
  int recentHashes(uint64_t* out, int max) const;

  // Each move is kept as its index in the legal move list.
  static constexpr size_t STATE_SIZE = 2 + MAX_PLIES;
  void save(uint8_t* out) const;
  bool restore(const uint8_t* in);

 private:
  Position pos;
  Move moves[MAX_PLIES];
  Undo undos[MAX_PLIES];
  uint8_t indices[MAX_PLIES] = {};
  int count = 0;
};

// Alpha-beta search with iterative deepening, a transposition table and
// killer moves; jumps are always followed to the end. About 170 KB.
class Engine {
 public:
  struct Limits {
    int maxDepth = 6;
    uint32_t maxNodes = 0;  // 0 = no limit
    int noise = 0;          // > 0: a weaker player
  };
  using PollFn = bool (*)(void* user);

  Move think(const Game& game, const Limits& limits, uint32_t seed, PollFn poll, void* user);
  int depthReached() const { return reachedDepth; }
  int score() const { return lastScore; }
  uint32_t nodeCount() const { return nodes; }

 private:
  static constexpr int MAX_PLY = 64;
  static constexpr int STACK = 4096;
  static constexpr int TT_SIZE = 1 << 13;
  static constexpr int REP = 256;
  struct TTEntry {
    uint64_t key = 0;
    int16_t score = 0;
    uint8_t depth = 0;
    uint8_t bound = 0;
    uint8_t best = 0xFF;  // index in the move list
  };

  int search(int depth, int alpha, int beta, int ply);
  bool tick();
  bool isRepetition() const;

  Position pos;
  uint64_t rep[REP] = {};
  int repCount = 0;
  Move moveStack[STACK];
  int32_t scoreStack[STACK] = {};
  uint8_t indexStack[STACK] = {};
  int stackTop = 0;
  TTEntry tt[TT_SIZE];
  Move killers[MAX_PLY];
  Move rootBest;
  bool rootFound = false;
  uint32_t nodes = 0;
  uint32_t maxNodes = 0;
  bool stopped = false;
  PollFn poll = nullptr;
  void* pollUser = nullptr;
  int reachedDepth = 0;
  int lastScore = 0;
};

}  // namespace games::checkers
