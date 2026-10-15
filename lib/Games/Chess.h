#pragma once

#include <cstddef>
#include <cstdint>

// Chess rules and a small computer player. The board is 0x88: square =
// rank * 16 + file, rank 0 is White's back rank; a square is off the board
// when (square & 0x88) != 0.
namespace games::chess {

enum PieceType : uint8_t { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
constexpr int WHITE = 0;
constexpr int BLACK = 1;
constexpr uint8_t BLACK_BIT = 8;
constexpr uint8_t NO_SQUARE = 0x80;

inline int typeOf(uint8_t piece) { return piece & 7; }
inline int colorOf(uint8_t piece) { return piece >> 3; }
inline uint8_t makePiece(int color, int type) { return static_cast<uint8_t>(type | (color ? BLACK_BIT : 0)); }
inline int squareAt(int file, int rank) { return rank * 16 + file; }
inline int fileOf(int square) { return square & 7; }
inline int rankOf(int square) { return square >> 4; }
inline bool onBoard(int square) { return (square & 0x88) == 0; }

struct Move {
  enum Flag : uint8_t { CAPTURE = 1, EN_PASSANT = 2, CASTLE = 4, DOUBLE_PUSH = 8 };
  uint8_t from = 0;
  uint8_t to = 0;
  uint8_t promo = 0;  // piece type, or 0
  uint8_t flags = 0;

  bool isNull() const { return from == to; }
  bool sameAs(const Move& other) const { return from == other.from && to == other.to && promo == other.promo; }
  // 15 bits: from and to as 0..63, then the promotion type. Flags are not kept.
  uint16_t pack() const;
  static Move unpack(uint16_t packed);
};

struct Undo {
  uint64_t key = 0;
  uint8_t captured = 0;
  uint8_t castling = 0;
  uint8_t ep = NO_SQUARE;
  uint8_t halfmove = 0;
};

class Position {
 public:
  static constexpr int MAX_MOVES = 256;

  void setStart();
  bool setFen(const char* fen);

  uint8_t at(int square) const { return board[square]; }
  int sideToMove() const { return side; }
  int halfmoveClock() const { return halfmove; }
  int kingSquare(int color) const { return kings[color]; }
  uint64_t hash() const { return key; }
  uint64_t computeHash() const;

  // Pseudo-legal moves; capturesOnly also keeps promotions.
  int generate(Move* out, bool capturesOnly) const;
  int legalMoves(Move* out);
  void make(const Move& move, Undo& undo);
  void unmake(const Move& move, const Undo& undo);
  void makeNull(Undo& undo);
  void unmakeNull(const Undo& undo);

  bool attacked(int square, int byColor) const;
  bool inCheck() const { return attacked(kings[side], side ^ 1); }
  // After make(): whether the side that just moved left its king attacked.
  bool moverInCheck() const { return attacked(kings[side ^ 1], side); }
  bool insufficientMaterial() const;
  bool hasPieces(int color) const;  // anything besides pawns and the king

  // From the side to move's point of view, in centipawns.
  int evaluate() const;

 private:
  void clear();

  uint8_t board[128] = {};
  uint8_t kings[2] = {0x04, 0x74};
  uint8_t side = WHITE;
  uint8_t castling = 0;  // 1 White short, 2 White long, 4 Black short, 8 Black long
  uint8_t ep = NO_SQUARE;
  uint8_t halfmove = 0;
  uint16_t fullmove = 1;
  uint64_t key = 0;
};

// A game from the standard start: the moves played, undo, and the result.
class Game {
 public:
  static constexpr int MAX_PLIES = 1024;
  enum class Status : uint8_t { Playing, Checkmate, Stalemate, FiftyMoves, Repetition, Material, TooLong };

  void reset();
  bool setFen(const char* fen);  // tests: a game from another position, not saved
  const Position& position() const { return pos; }
  int legalMoves(Move* out) const;
  // Plays a legal move matching from, to and promotion. False otherwise.
  bool play(const Move& move);
  bool undo();
  int plies() const { return count; }
  Move moveAt(int ply) const { return moves[ply]; }
  Status status() const;
  // Hashes of earlier positions since the last capture or pawn move, oldest
  // first, at most max.
  int recentHashes(uint64_t* out, int max) const;

  static constexpr size_t STATE_SIZE = 2 + 2 * MAX_PLIES;
  void save(uint8_t* out) const;
  bool restore(const uint8_t* in);

 private:
  int repetitions() const;

  Position pos;
  Move moves[MAX_PLIES];
  Undo undos[MAX_PLIES];
  int count = 0;
};

// Alpha-beta search with iterative deepening, quiescence, a transposition
// table and killer moves. About 170 KB; allocate it once per game screen.
class Engine {
 public:
  struct Limits {
    int maxDepth = 4;
    uint32_t maxNodes = 0;  // 0 = no limit
    int noise = 0;          // > 0: a weaker player that adds up to this many centipawns at random
    // Weaker still: false stops following captures past the last ply, so
    // pieces get left hanging; blunderPercent of moves are any legal move.
    bool quiescence = true;
    int blunderPercent = 0;
  };
  // Called every 1024 nodes; returning true stops the search.
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
    Move move;
    int16_t score = 0;
    uint8_t depth = 0;
    uint8_t bound = 0;
  };

  int search(int depth, int alpha, int beta, int ply, bool allowNull);
  int quiesce(int alpha, int beta, int ply);
  bool tick();
  bool isRepetition() const;
  void scoreMoves(int base, int n, const Move& ttMove, int ply);
  void pick(int base, int i, int n);

  Position pos;
  uint64_t rep[REP] = {};
  int repCount = 0;
  Move moveStack[STACK];
  int32_t scoreStack[STACK] = {};
  int stackTop = 0;
  TTEntry tt[TT_SIZE];
  Move killers[MAX_PLY][2];
  Move rootBest;
  uint32_t nodes = 0;
  uint32_t maxNodes = 0;
  bool stopped = false;
  bool useQuiescence = true;
  PollFn poll = nullptr;
  void* pollUser = nullptr;
  int reachedDepth = 0;
  int lastScore = 0;
};

}  // namespace games::chess
