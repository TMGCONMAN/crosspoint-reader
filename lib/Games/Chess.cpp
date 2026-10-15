#include "Chess.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace games::chess {

namespace {

// Zobrist keys, made at compile time so they live in flash.
struct Keys {
  uint64_t piece[16][64] = {};
  uint64_t castle[16] = {};
  uint64_t ep[8] = {};
  uint64_t side = 0;
};
constexpr uint64_t splitmix(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}
constexpr Keys makeKeys() {
  Keys k;
  uint64_t state = 0x43484553534B4559ull;
  for (auto& row : k.piece) {
    for (auto& v : row) v = splitmix(state);
  }
  for (auto& v : k.castle) v = splitmix(state);
  for (auto& v : k.ep) v = splitmix(state);
  k.side = splitmix(state);
  return k;
}
constexpr Keys KEYS = makeKeys();

int to64(int square) { return (square + (square & 7)) >> 1; }
int from64(int index) { return (index >> 3) * 16 + (index & 7); }
uint64_t pieceKey(uint8_t piece, int square) { return KEYS.piece[piece][to64(square)]; }

// Castling rights left after a move touches a square.
struct CastleMask {
  uint8_t mask[128] = {};
};
constexpr CastleMask makeCastleMask() {
  CastleMask m;
  for (auto& v : m.mask) v = 15;
  m.mask[0x00] = 15 & ~2;
  m.mask[0x04] = 15 & ~3;
  m.mask[0x07] = 15 & ~1;
  m.mask[0x70] = 15 & ~8;
  m.mask[0x74] = 15 & ~12;
  m.mask[0x77] = 15 & ~4;
  return m;
}
constexpr CastleMask CASTLE_MASK = makeCastleMask();

constexpr int KNIGHT_STEPS[8] = {-33, -31, -18, -14, 14, 18, 31, 33};
constexpr int KING_STEPS[8] = {-17, -16, -15, -1, 1, 15, 16, 17};
constexpr int DIAGONALS[4] = {-17, -15, 15, 17};
constexpr int STRAIGHTS[4] = {-16, -1, 1, 16};

constexpr int VALUE[7] = {0, 100, 320, 330, 500, 900, 0};
constexpr int PHASE[7] = {0, 0, 1, 1, 2, 4, 0};

// Piece-square tables from White's side, a8 first (Michniewski).
// clang-format off
constexpr int8_t PST[7][64] = {
    {},
    {0,  0,  0,  0,  0,  0,  0,  0,
     50, 50, 50, 50, 50, 50, 50, 50,
     10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0},
    {-50,-40,-30,-30,-30,-30,-40,-50,
     -40,-20,  0,  0,  0,  0,-20,-40,
     -30,  0, 10, 15, 15, 10,  0,-30,
     -30,  5, 15, 20, 20, 15,  5,-30,
     -30,  0, 15, 20, 20, 15,  0,-30,
     -30,  5, 10, 15, 15, 10,  5,-30,
     -40,-20,  0,  5,  5,  0,-20,-40,
     -50,-40,-30,-30,-30,-30,-40,-50},
    {-20,-10,-10,-10,-10,-10,-10,-20,
     -10,  0,  0,  0,  0,  0,  0,-10,
     -10,  0,  5, 10, 10,  5,  0,-10,
     -10,  5,  5, 10, 10,  5,  5,-10,
     -10,  0, 10, 10, 10, 10,  0,-10,
     -10, 10, 10, 10, 10, 10, 10,-10,
     -10,  5,  0,  0,  0,  0,  5,-10,
     -20,-10,-10,-10,-10,-10,-10,-20},
    {0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0},
    {-20,-10,-10, -5, -5,-10,-10,-20,
     -10,  0,  0,  0,  0,  0,  0,-10,
     -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
     0,  0,  5,  5,  5,  5,  0, -5,
     -10,  5,  5,  5,  5,  5,  0,-10,
     -10,  0,  5,  0,  0,  0,  0,-10,
     -20,-10,-10, -5, -5,-10,-10,-20},
    {-30,-40,-40,-50,-50,-40,-40,-30,
     -30,-40,-40,-50,-50,-40,-40,-30,
     -30,-40,-40,-50,-50,-40,-40,-30,
     -30,-40,-40,-50,-50,-40,-40,-30,
     -20,-30,-30,-40,-40,-30,-30,-20,
     -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20},
};
constexpr int8_t KING_END[64] = {
     -50,-40,-30,-20,-20,-30,-40,-50,
     -30,-20,-10,  0,  0,-10,-20,-30,
     -30,-10, 20, 30, 30, 20,-10,-30,
     -30,-10, 30, 40, 40, 30,-10,-30,
     -30,-10, 30, 40, 40, 30,-10,-30,
     -30,-10, 20, 30, 30, 20,-10,-30,
     -30,-30,  0,  0,  0,  0,-30,-30,
     -50,-30,-30,-30,-30,-30,-30,-50};
// clang-format on
constexpr int PASSED[8] = {0, 5, 10, 20, 35, 60, 100, 0};

constexpr int INF = 32000;
constexpr int MATE = 30000;
constexpr int MATE_BOUND = MATE - 200;

enum Bound : uint8_t { BOUND_NONE = 0, BOUND_EXACT = 1, BOUND_LOWER = 2, BOUND_UPPER = 3 };

int toTT(int score, int ply) {
  if (score > MATE_BOUND) return score + ply;
  if (score < -MATE_BOUND) return score - ply;
  return score;
}
int fromTT(int score, int ply) {
  if (score > MATE_BOUND) return score - ply;
  if (score < -MATE_BOUND) return score + ply;
  return score;
}

}  // namespace

uint16_t Move::pack() const { return static_cast<uint16_t>(to64(from) | (to64(to) << 6) | (promo << 12)); }

Move Move::unpack(const uint16_t packed) {
  Move m;
  m.from = static_cast<uint8_t>(from64(packed & 63));
  m.to = static_cast<uint8_t>(from64((packed >> 6) & 63));
  m.promo = static_cast<uint8_t>((packed >> 12) & 7);
  return m;
}

// ---- Position ----

void Position::clear() {
  memset(board, 0, sizeof(board));
  kings[WHITE] = 0x04;
  kings[BLACK] = 0x74;
  side = WHITE;
  castling = 0;
  ep = NO_SQUARE;
  halfmove = 0;
  fullmove = 1;
  key = 0;
}

void Position::setStart() { setFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); }

bool Position::setFen(const char* fen) {
  clear();
  int rank = 7;
  int file = 0;
  int kingCount[2] = {0, 0};
  const char* p = fen;
  for (; *p && *p != ' '; p++) {
    const char c = *p;
    if (c == '/') {
      rank--;
      file = 0;
      if (rank < 0) return false;
      continue;
    }
    if (c >= '1' && c <= '8') {
      file += c - '0';
      continue;
    }
    int type = 0;
    switch (c | 0x20) {
      case 'p':
        type = PAWN;
        break;
      case 'n':
        type = KNIGHT;
        break;
      case 'b':
        type = BISHOP;
        break;
      case 'r':
        type = ROOK;
        break;
      case 'q':
        type = QUEEN;
        break;
      case 'k':
        type = KING;
        break;
      default:
        return false;
    }
    if (file > 7) return false;
    const int color = (c >= 'a') ? BLACK : WHITE;
    const int sq = squareAt(file, rank);
    board[sq] = makePiece(color, type);
    if (type == KING) {
      kings[color] = static_cast<uint8_t>(sq);
      kingCount[color]++;
    }
    file++;
  }
  if (kingCount[WHITE] != 1 || kingCount[BLACK] != 1) return false;
  while (*p == ' ') p++;
  if (*p == 'b') side = BLACK;
  if (*p) p++;
  while (*p == ' ') p++;
  for (; *p && *p != ' '; p++) {
    if (*p == 'K') castling |= 1;
    if (*p == 'Q') castling |= 2;
    if (*p == 'k') castling |= 4;
    if (*p == 'q') castling |= 8;
  }
  // Rights only with the king and rook at home.
  if (board[0x04] != makePiece(WHITE, KING)) castling &= ~3;
  if (board[0x07] != makePiece(WHITE, ROOK)) castling &= ~1;
  if (board[0x00] != makePiece(WHITE, ROOK)) castling &= ~2;
  if (board[0x74] != makePiece(BLACK, KING)) castling &= ~12;
  if (board[0x77] != makePiece(BLACK, ROOK)) castling &= ~4;
  if (board[0x70] != makePiece(BLACK, ROOK)) castling &= ~8;
  while (*p == ' ') p++;
  if (p[0] >= 'a' && p[0] <= 'h' && p[1] >= '1' && p[1] <= '8') {
    ep = static_cast<uint8_t>(squareAt(p[0] - 'a', p[1] - '1'));
    p += 2;
  } else if (*p) {
    p++;
  }
  while (*p == ' ') p++;
  if (*p) {
    halfmove = static_cast<uint8_t>(atoi(p));
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p) fullmove = static_cast<uint16_t>(atoi(p));
  }
  key = computeHash();
  return true;
}

uint64_t Position::computeHash() const {
  uint64_t h = 0;
  for (int sq = 0; sq < 128; sq++) {
    if (onBoard(sq) && board[sq]) h ^= pieceKey(board[sq], sq);
  }
  h ^= KEYS.castle[castling];
  if (ep != NO_SQUARE) h ^= KEYS.ep[fileOf(ep)];
  if (side == BLACK) h ^= KEYS.side;
  return h;
}

bool Position::attacked(const int square, const int byColor) const {
  if (byColor == WHITE) {
    for (const int d : {-15, -17}) {
      const int s = square + d;
      if (onBoard(s) && board[s] == makePiece(WHITE, PAWN)) return true;
    }
  } else {
    for (const int d : {15, 17}) {
      const int s = square + d;
      if (onBoard(s) && board[s] == makePiece(BLACK, PAWN)) return true;
    }
  }
  const uint8_t knight = makePiece(byColor, KNIGHT);
  for (const int d : KNIGHT_STEPS) {
    const int s = square + d;
    if (onBoard(s) && board[s] == knight) return true;
  }
  const uint8_t king = makePiece(byColor, KING);
  for (const int d : KING_STEPS) {
    const int s = square + d;
    if (onBoard(s) && board[s] == king) return true;
  }
  const uint8_t bishop = makePiece(byColor, BISHOP);
  const uint8_t rook = makePiece(byColor, ROOK);
  const uint8_t queen = makePiece(byColor, QUEEN);
  for (const int d : DIAGONALS) {
    for (int s = square + d; onBoard(s); s += d) {
      if (board[s]) {
        if (board[s] == bishop || board[s] == queen) return true;
        break;
      }
    }
  }
  for (const int d : STRAIGHTS) {
    for (int s = square + d; onBoard(s); s += d) {
      if (board[s]) {
        if (board[s] == rook || board[s] == queen) return true;
        break;
      }
    }
  }
  return false;
}

int Position::generate(Move* out, const bool capturesOnly) const {
  int n = 0;
  const int us = side;
  const int them = side ^ 1;
  const auto add = [&](int from, int to, uint8_t flags, uint8_t promo) {
    out[n].from = static_cast<uint8_t>(from);
    out[n].to = static_cast<uint8_t>(to);
    out[n].promo = promo;
    out[n].flags = flags;
    n++;
  };
  const auto addPawn = [&](int from, int to, uint8_t flags) {
    if (rankOf(to) == 0 || rankOf(to) == 7) {
      for (const uint8_t promo : {QUEEN, KNIGHT, ROOK, BISHOP}) add(from, to, flags, promo);
    } else if (flags || !capturesOnly) {
      add(from, to, flags, 0);
    }
  };

  for (int sq = 0; sq < 128; sq++) {
    if (!onBoard(sq)) {
      sq += 7;
      continue;
    }
    const uint8_t piece = board[sq];
    if (!piece || colorOf(piece) != us) continue;
    const int type = typeOf(piece);
    if (type == PAWN) {
      const int dir = us == WHITE ? 16 : -16;
      const int one = sq + dir;
      if (onBoard(one) && !board[one]) {
        addPawn(sq, one, 0);
        const int startRank = us == WHITE ? 1 : 6;
        const int two = one + dir;
        if (!capturesOnly && rankOf(sq) == startRank && !board[two]) add(sq, two, Move::DOUBLE_PUSH, 0);
      }
      for (const int d : {dir - 1, dir + 1}) {
        const int to = sq + d;
        if (!onBoard(to)) continue;
        if (board[to] && colorOf(board[to]) == them) {
          addPawn(sq, to, Move::CAPTURE);
        } else if (to == ep) {
          add(sq, to, Move::CAPTURE | Move::EN_PASSANT, 0);
        }
      }
      continue;
    }
    if (type == KNIGHT || type == KING) {
      for (const int d : (type == KNIGHT ? KNIGHT_STEPS : KING_STEPS)) {
        const int to = sq + d;
        if (!onBoard(to)) continue;
        if (!board[to]) {
          if (!capturesOnly) add(sq, to, 0, 0);
        } else if (colorOf(board[to]) == them) {
          add(sq, to, Move::CAPTURE, 0);
        }
      }
      continue;
    }
    for (int k = 0; k < 8; k++) {
      const bool diagonal = k < 4;
      if (diagonal && type == ROOK) continue;
      if (!diagonal && type == BISHOP) continue;
      const int d = diagonal ? DIAGONALS[k] : STRAIGHTS[k - 4];
      for (int to = sq + d; onBoard(to); to += d) {
        if (!board[to]) {
          if (!capturesOnly) add(sq, to, 0, 0);
          continue;
        }
        if (colorOf(board[to]) == them) add(sq, to, Move::CAPTURE, 0);
        break;
      }
    }
  }

  if (!capturesOnly) {
    const int base = us == WHITE ? 0x00 : 0x70;
    const int shortRight = us == WHITE ? 1 : 4;
    const int longRight = us == WHITE ? 2 : 8;
    const int e = base + 4;
    if ((castling & shortRight) && !board[e + 1] && !board[e + 2] && !attacked(e, them) && !attacked(e + 1, them) &&
        !attacked(e + 2, them)) {
      add(e, e + 2, Move::CASTLE, 0);
    }
    if ((castling & longRight) && !board[e - 1] && !board[e - 2] && !board[e - 3] && !attacked(e, them) &&
        !attacked(e - 1, them) && !attacked(e - 2, them)) {
      add(e, e - 2, Move::CASTLE, 0);
    }
  }
  return n;
}

int Position::legalMoves(Move* out) {
  Move all[MAX_MOVES];
  const int n = generate(all, false);
  int legal = 0;
  for (int i = 0; i < n; i++) {
    Undo undo;
    make(all[i], undo);
    if (!moverInCheck()) out[legal++] = all[i];
    unmake(all[i], undo);
  }
  return legal;
}

void Position::make(const Move& move, Undo& undo) {
  undo.key = key;
  undo.captured = 0;
  undo.castling = castling;
  undo.ep = ep;
  undo.halfmove = halfmove;
  const int us = side;
  const uint8_t piece = board[move.from];

  if (ep != NO_SQUARE) key ^= KEYS.ep[fileOf(ep)];
  if (halfmove < 255) halfmove++;
  if (move.flags & Move::EN_PASSANT) {
    const int capturedSquare = move.to + (us == WHITE ? -16 : 16);
    undo.captured = board[capturedSquare];
    key ^= pieceKey(undo.captured, capturedSquare);
    board[capturedSquare] = 0;
  } else if (board[move.to]) {
    undo.captured = board[move.to];
    key ^= pieceKey(undo.captured, move.to);
  }
  if (undo.captured || typeOf(piece) == PAWN) halfmove = 0;

  key ^= pieceKey(piece, move.from);
  board[move.from] = 0;
  const uint8_t placed = move.promo ? makePiece(us, move.promo) : piece;
  board[move.to] = placed;
  key ^= pieceKey(placed, move.to);
  if (typeOf(piece) == KING) kings[us] = move.to;

  if (move.flags & Move::CASTLE) {
    const bool kingSide = move.to > move.from;
    const int rookFrom = kingSide ? move.from + 3 : move.from - 4;
    const int rookTo = kingSide ? move.from + 1 : move.from - 1;
    const uint8_t rook = board[rookFrom];
    board[rookFrom] = 0;
    board[rookTo] = rook;
    key ^= pieceKey(rook, rookFrom) ^ pieceKey(rook, rookTo);
  }

  key ^= KEYS.castle[castling];
  castling &= CASTLE_MASK.mask[move.from] & CASTLE_MASK.mask[move.to];
  key ^= KEYS.castle[castling];

  ep = NO_SQUARE;
  if (move.flags & Move::DOUBLE_PUSH) {
    ep = static_cast<uint8_t>((move.from + move.to) / 2);
    key ^= KEYS.ep[fileOf(ep)];
  }
  if (us == BLACK) fullmove++;
  side ^= 1;
  key ^= KEYS.side;
}

void Position::unmake(const Move& move, const Undo& undo) {
  side ^= 1;
  const int us = side;
  if (us == BLACK) fullmove--;
  uint8_t piece = board[move.to];
  if (move.promo) piece = makePiece(us, PAWN);
  board[move.from] = piece;
  board[move.to] = 0;
  if (move.flags & Move::EN_PASSANT) {
    board[move.to + (us == WHITE ? -16 : 16)] = undo.captured;
  } else {
    board[move.to] = undo.captured;
  }
  if (typeOf(piece) == KING) kings[us] = move.from;
  if (move.flags & Move::CASTLE) {
    const bool kingSide = move.to > move.from;
    const int rookFrom = kingSide ? move.from + 3 : move.from - 4;
    const int rookTo = kingSide ? move.from + 1 : move.from - 1;
    board[rookFrom] = board[rookTo];
    board[rookTo] = 0;
  }
  castling = undo.castling;
  ep = undo.ep;
  halfmove = undo.halfmove;
  key = undo.key;
}

void Position::makeNull(Undo& undo) {
  undo.key = key;
  undo.ep = ep;
  undo.halfmove = halfmove;
  if (ep != NO_SQUARE) key ^= KEYS.ep[fileOf(ep)];
  ep = NO_SQUARE;
  side ^= 1;
  key ^= KEYS.side;
}

void Position::unmakeNull(const Undo& undo) {
  side ^= 1;
  ep = undo.ep;
  halfmove = undo.halfmove;
  key = undo.key;
}

bool Position::insufficientMaterial() const {
  int minors = 0;
  for (int sq = 0; sq < 128; sq++) {
    if (!onBoard(sq) || !board[sq]) continue;
    const int type = typeOf(board[sq]);
    if (type == PAWN || type == ROOK || type == QUEEN) return false;
    if (type == KNIGHT || type == BISHOP) minors++;
  }
  return minors <= 1;
}

bool Position::hasPieces(const int color) const {
  for (int sq = 0; sq < 128; sq++) {
    if (!onBoard(sq) || !board[sq] || colorOf(board[sq]) != color) continue;
    const int type = typeOf(board[sq]);
    if (type != PAWN && type != KING) return true;
  }
  return false;
}

int Position::evaluate() const {
  int score[2] = {0, 0};
  int kingMid[2] = {0, 0};
  int kingEnd[2] = {0, 0};
  int material[2] = {0, 0};
  int bishops[2] = {0, 0};
  int phase = 0;
  // For passed pawns, per file: the lowest White pawn and highest Black pawn.
  int whiteLowest[8];
  int blackHighest[8];
  for (int f = 0; f < 8; f++) {
    whiteLowest[f] = 8;
    blackHighest[f] = -1;
  }
  for (int sq = 0; sq < 128; sq++) {
    if (!onBoard(sq)) {
      sq += 7;
      continue;
    }
    const uint8_t piece = board[sq];
    if (!piece) continue;
    const int c = colorOf(piece);
    const int type = typeOf(piece);
    const int f = fileOf(sq);
    const int r = rankOf(sq);
    const int index = c == WHITE ? (7 - r) * 8 + f : r * 8 + f;
    if (type == KING) {
      kingMid[c] = PST[KING][index];
      kingEnd[c] = KING_END[index];
      continue;
    }
    material[c] += VALUE[type];
    score[c] += VALUE[type] + PST[type][index];
    phase += PHASE[type];
    if (type == BISHOP) bishops[c]++;
    if (type == PAWN) {
      if (c == WHITE && r < whiteLowest[f]) whiteLowest[f] = r;
      if (c == BLACK && r > blackHighest[f]) blackHighest[f] = r;
    }
  }
  if (phase > 24) phase = 24;
  // Passed pawns: no enemy pawn ahead on the same or a neighbouring file.
  for (int sq = 0; sq < 128; sq++) {
    if (!onBoard(sq)) {
      sq += 7;
      continue;
    }
    const uint8_t piece = board[sq];
    if (typeOf(piece) != PAWN) continue;
    const int c = colorOf(piece);
    const int f = fileOf(sq);
    const int r = rankOf(sq);
    bool passed = true;
    for (int g = f - 1; g <= f + 1 && passed; g++) {
      if (g < 0 || g > 7) continue;
      if (c == WHITE && blackHighest[g] > r) passed = false;
      if (c == BLACK && whiteLowest[g] < r) passed = false;
    }
    if (passed) score[c] += PASSED[c == WHITE ? r : 7 - r] * (24 - phase / 2) / 24;
  }
  for (int c = 0; c < 2; c++) {
    if (bishops[c] >= 2) score[c] += 30;
    score[c] += (kingMid[c] * phase + kingEnd[c] * (24 - phase)) / 24;
  }
  int total = score[WHITE] - score[BLACK];
  // In a won ending, drive the lone king to the edge and bring ours closer.
  if (phase <= 6) {
    const int diff = material[WHITE] - material[BLACK];
    if (diff >= 300 || diff <= -300) {
      const int winner = diff > 0 ? WHITE : BLACK;
      const int loser = winner ^ 1;
      const int lf = fileOf(kings[loser]);
      const int lr = rankOf(kings[loser]);
      const int edge = abs(2 * lf - 7) + abs(2 * lr - 7);  // 2..14, larger at the edge
      const int dist = abs(lf - fileOf(kings[winner])) + abs(lr - rankOf(kings[winner]));
      const int bonus = 5 * edge + 4 * (14 - dist);
      total += winner == WHITE ? bonus : -bonus;
    }
  }
  return side == WHITE ? total : -total;
}

// ---- Game ----

void Game::reset() {
  pos.setStart();
  count = 0;
}

bool Game::setFen(const char* fen) {
  count = 0;
  return pos.setFen(fen);
}

int Game::legalMoves(Move* out) const {
  Position copy = pos;
  return copy.legalMoves(out);
}

bool Game::play(const Move& move) {
  if (count >= MAX_PLIES) return false;
  Move legal[Position::MAX_MOVES];
  const int n = legalMoves(legal);
  for (int i = 0; i < n; i++) {
    if (!legal[i].sameAs(move)) continue;
    pos.make(legal[i], undos[count]);
    moves[count] = legal[i];
    count++;
    return true;
  }
  return false;
}

bool Game::undo() {
  if (count == 0) return false;
  count--;
  pos.unmake(moves[count], undos[count]);
  return true;
}

int Game::repetitions() const {
  // undos[i].key is the position before move i.
  int seen = 0;
  const int reach = pos.halfmoveClock();
  for (int k = 2; k <= reach && k <= count; k += 2) {
    if (undos[count - k].key == pos.hash()) seen++;
  }
  return seen;
}

int Game::recentHashes(uint64_t* out, const int max) const {
  int n = pos.halfmoveClock();
  if (n > count) n = count;
  if (n > max) n = max;
  for (int i = 0; i < n; i++) out[i] = undos[count - n + i].key;
  return n;
}

Game::Status Game::status() const {
  Move legal[Position::MAX_MOVES];
  if (legalMoves(legal) == 0) return pos.inCheck() ? Status::Checkmate : Status::Stalemate;
  if (pos.halfmoveClock() >= 100) return Status::FiftyMoves;
  if (repetitions() >= 2) return Status::Repetition;
  if (pos.insufficientMaterial()) return Status::Material;
  if (count >= MAX_PLIES) return Status::TooLong;
  return Status::Playing;
}

void Game::save(uint8_t* out) const {
  memset(out, 0, STATE_SIZE);
  out[0] = static_cast<uint8_t>(count);
  out[1] = static_cast<uint8_t>(count >> 8);
  for (int i = 0; i < count; i++) {
    const uint16_t packed = moves[i].pack();
    out[2 + 2 * i] = static_cast<uint8_t>(packed);
    out[3 + 2 * i] = static_cast<uint8_t>(packed >> 8);
  }
}

bool Game::restore(const uint8_t* in) {
  const int n = in[0] | (in[1] << 8);
  if (n > MAX_PLIES) return false;
  reset();
  for (int i = 0; i < n; i++) {
    const uint16_t packed = static_cast<uint16_t>(in[2 + 2 * i] | (in[3 + 2 * i] << 8));
    if (!play(Move::unpack(packed))) {
      reset();
      return false;
    }
  }
  return true;
}

// ---- Engine ----

bool Engine::tick() {
  if (stopped) return true;
  nodes++;
  if ((nodes & 1023) == 0) {
    if (poll && poll(pollUser)) stopped = true;
    if (maxNodes && nodes >= maxNodes) stopped = true;
  }
  return stopped;
}

bool Engine::isRepetition() const {
  const int reach = pos.halfmoveClock();
  for (int k = 2; k <= reach && k <= repCount; k += 2) {
    if (rep[repCount - k] == pos.hash()) return true;
  }
  return false;
}

void Engine::scoreMoves(const int base, const int n, const Move& ttMove, const int ply) {
  for (int i = 0; i < n; i++) {
    const Move& m = moveStack[base + i];
    int32_t s = 0;
    if (!ttMove.isNull() && m.sameAs(ttMove)) {
      s = 1000000;
    } else if (m.flags & Move::CAPTURE) {
      const int victim = (m.flags & Move::EN_PASSANT) ? PAWN : typeOf(pos.at(m.to));
      s = 100000 + VALUE[victim] * 10 - typeOf(pos.at(m.from));
    } else if (m.promo) {
      s = 90000 + m.promo;
    } else if (ply < MAX_PLY && m.sameAs(killers[ply][0])) {
      s = 80000;
    } else if (ply < MAX_PLY && m.sameAs(killers[ply][1])) {
      s = 79000;
    }
    scoreStack[base + i] = s;
  }
}

void Engine::pick(const int base, const int i, const int n) {
  int best = i;
  for (int j = i + 1; j < n; j++) {
    if (scoreStack[base + j] > scoreStack[base + best]) best = j;
  }
  if (best != i) {
    const Move m = moveStack[base + i];
    moveStack[base + i] = moveStack[base + best];
    moveStack[base + best] = m;
    const int32_t s = scoreStack[base + i];
    scoreStack[base + i] = scoreStack[base + best];
    scoreStack[base + best] = s;
  }
}

int Engine::quiesce(int alpha, const int beta, const int ply) {
  if (tick()) return 0;
  const int standPat = pos.evaluate();
  if (ply >= MAX_PLY - 1 || standPat >= beta) return standPat;
  if (standPat > alpha) alpha = standPat;
  if (stackTop + Position::MAX_MOVES > STACK) return standPat;

  const int base = stackTop;
  const int n = pos.generate(moveStack + base, true);
  stackTop += n;
  scoreMoves(base, n, Move{}, MAX_PLY);
  int best = standPat;
  for (int i = 0; i < n; i++) {
    pick(base, i, n);
    const Move m = moveStack[base + i];
    Undo undo;
    pos.make(m, undo);
    if (pos.moverInCheck()) {
      pos.unmake(m, undo);
      continue;
    }
    const int s = -quiesce(-beta, -alpha, ply + 1);
    pos.unmake(m, undo);
    if (stopped) break;
    if (s > best) {
      best = s;
      if (s > alpha) alpha = s;
      if (alpha >= beta) break;
    }
  }
  stackTop = base;
  return stopped ? 0 : best;
}

int Engine::search(int depth, int alpha, int beta, const int ply, const bool allowNull) {
  if (tick()) return 0;
  if (ply > 0) {
    if (pos.halfmoveClock() >= 100 || isRepetition() || pos.insufficientMaterial()) return 0;
    // No mate found deeper can beat one already found nearer.
    if (alpha < -MATE + ply) alpha = -MATE + ply;
    if (beta > MATE - ply - 1) beta = MATE - ply - 1;
    if (alpha >= beta) return alpha;
  }
  const bool inCheck = pos.inCheck();
  if (inCheck) depth++;
  if (depth <= 0) return useQuiescence ? quiesce(alpha, beta, ply) : pos.evaluate();
  if (ply >= MAX_PLY - 1 || stackTop + Position::MAX_MOVES > STACK || repCount >= REP) return pos.evaluate();

  TTEntry& entry = tt[pos.hash() & (TT_SIZE - 1)];
  Move ttMove;
  if (entry.key == pos.hash()) {
    ttMove = entry.move;
    if (ply > 0 && entry.depth >= depth) {
      const int s = fromTT(entry.score, ply);
      if (entry.bound == BOUND_EXACT || (entry.bound == BOUND_LOWER && s >= beta) ||
          (entry.bound == BOUND_UPPER && s <= alpha)) {
        return s;
      }
    }
  }

  // Null move: if passing still beats beta, this line is good enough.
  if (allowNull && !inCheck && ply > 0 && depth >= 3 && beta < MATE_BOUND && pos.hasPieces(pos.sideToMove()) &&
      pos.evaluate() >= beta) {
    Undo undo;
    pos.makeNull(undo);
    rep[repCount++] = undo.key;
    const int s = -search(depth - 3, -beta, -beta + 1, ply + 1, false);
    repCount--;
    pos.unmakeNull(undo);
    if (stopped) return 0;
    if (s >= beta) return beta;
  }

  const int base = stackTop;
  const int n = pos.generate(moveStack + base, false);
  stackTop += n;
  scoreMoves(base, n, ttMove, ply);
  const int startAlpha = alpha;
  int best = -INF;
  Move bestMove;
  int legal = 0;
  for (int i = 0; i < n; i++) {
    pick(base, i, n);
    const Move m = moveStack[base + i];
    Undo undo;
    pos.make(m, undo);
    if (pos.moverInCheck()) {
      pos.unmake(m, undo);
      continue;
    }
    legal++;
    rep[repCount++] = undo.key;
    const int s = -search(depth - 1, -beta, -alpha, ply + 1, true);
    repCount--;
    pos.unmake(m, undo);
    if (stopped) {
      stackTop = base;
      return 0;
    }
    if (s <= best) continue;
    best = s;
    bestMove = m;
    if (s <= alpha) continue;
    alpha = s;
    if (ply == 0) rootBest = m;
    if (alpha >= beta) {
      if (!(m.flags & Move::CAPTURE) && !m.promo && !m.sameAs(killers[ply][0])) {
        killers[ply][1] = killers[ply][0];
        killers[ply][0] = m;
      }
      break;
    }
  }
  stackTop = base;
  if (legal == 0) return inCheck ? -MATE + ply : 0;

  entry.key = pos.hash();
  entry.move = bestMove;
  entry.score = static_cast<int16_t>(toTT(best, ply));
  entry.depth = static_cast<uint8_t>(depth);
  entry.bound = best <= startAlpha ? BOUND_UPPER : (best >= beta ? BOUND_LOWER : BOUND_EXACT);
  return best;
}

Move Engine::think(const Game& game, const Limits& limits, uint32_t seed, const PollFn pollFn, void* user) {
  pos = game.position();
  repCount = game.recentHashes(rep, 128);
  stackTop = 0;
  nodes = 0;
  maxNodes = limits.maxNodes;
  stopped = false;
  poll = pollFn;
  pollUser = user;
  reachedDepth = 0;
  lastScore = 0;
  for (auto& k : killers) k[0] = k[1] = Move{};

  Move legal[Position::MAX_MOVES];
  const int n = pos.legalMoves(legal);
  if (n == 0) return Move{};
  if (n == 1) return legal[0];
  const int maxDepth = limits.maxDepth < 1 ? 1 : (limits.maxDepth > MAX_PLY - 8 ? MAX_PLY - 8 : limits.maxDepth);

  useQuiescence = limits.quiescence;
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  if (limits.blunderPercent > 0 && static_cast<int>(seed % 100) < limits.blunderPercent) {
    return legal[(seed / 100) % static_cast<uint32_t>(n)];
  }

  if (limits.noise > 0) {
    // A weaker player: a shallow look at every move, plus a random nudge.
    Move best = legal[0];
    int bestScore = -INF;
    for (int i = 0; i < n; i++) {
      Undo undo;
      pos.make(legal[i], undo);
      rep[repCount++] = undo.key;
      int s = -search(maxDepth - 1, -INF, INF, 1, true);
      repCount--;
      pos.unmake(legal[i], undo);
      if (stopped) break;
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      if (s > -MATE_BOUND && s < MATE_BOUND) s += static_cast<int>(seed % static_cast<uint32_t>(limits.noise + 1));
      if (s > bestScore) {
        bestScore = s;
        best = legal[i];
      }
    }
    reachedDepth = maxDepth;
    lastScore = bestScore;
    return best;
  }

  Move best = legal[0];
  for (int depth = 1; depth <= maxDepth; depth++) {
    rootBest = Move{};
    const int s = search(depth, -INF, INF, 0, false);
    // A cut-short pass still knows a move at least as good as the last best.
    if (!rootBest.isNull()) best = rootBest;
    if (stopped) break;
    reachedDepth = depth;
    lastScore = s;
    if (s > MATE_BOUND || s < -MATE_BOUND) break;
  }
  return best;
}

}  // namespace games::chess
