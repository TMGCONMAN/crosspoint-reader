#include "Checkers.h"

#include <cstdlib>
#include <cstring>

namespace games::checkers {

namespace {

// Neighbours by direction: 0 up-left, 1 up-right, 2 down-left, 3 down-right.
struct Geometry {
  int8_t step[SQUARES][4] = {};
  int8_t jump[SQUARES][4] = {};
};
constexpr Geometry makeGeometry() {
  Geometry g;
  constexpr int DR[4] = {-1, -1, 1, 1};
  constexpr int DC[4] = {-1, 1, -1, 1};
  for (int s = 0; s < SQUARES; s++) {
    const int r = s / 4;
    const int c = 2 * (s % 4) + (r % 2 == 0 ? 1 : 0);
    for (int d = 0; d < 4; d++) {
      const int r1 = r + DR[d];
      const int c1 = c + DC[d];
      const int r2 = r + 2 * DR[d];
      const int c2 = c + 2 * DC[d];
      g.step[s][d] = static_cast<int8_t>((r1 >= 0 && r1 < 8 && c1 >= 0 && c1 < 8) ? r1 * 4 + c1 / 2 : -1);
      g.jump[s][d] = static_cast<int8_t>((r2 >= 0 && r2 < 8 && c2 >= 0 && c2 < 8) ? r2 * 4 + c2 / 2 : -1);
    }
  }
  return g;
}
constexpr Geometry GEO = makeGeometry();

bool forward(int color, int direction) { return color == BLACK ? direction >= 2 : direction < 2; }
int crownRow(int color) { return color == BLACK ? 7 : 0; }
uint8_t crowned(uint8_t piece) {
  if (piece == BLACK_MAN) return BLACK_KING;
  if (piece == WHITE_MAN) return WHITE_KING;
  return piece;
}

int middleOf(int a, int b) {
  for (int d = 0; d < 4; d++) {
    if (GEO.jump[a][d] == b) return GEO.step[a][d];
  }
  return -1;
}

struct Keys {
  uint64_t piece[5][SQUARES] = {};
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
  uint64_t state = 0x434845434B455253ull;
  for (auto& row : k.piece) {
    for (auto& v : row) v = splitmix(state);
  }
  k.side = splitmix(state);
  return k;
}
constexpr Keys KEYS = makeKeys();

constexpr int INF = 32000;
constexpr int WIN = 30000;
constexpr int WIN_BOUND = WIN - 200;
enum Bound : uint8_t { BOUND_NONE = 0, BOUND_EXACT = 1, BOUND_LOWER = 2, BOUND_UPPER = 3 };

int toTT(int score, int ply) {
  if (score > WIN_BOUND) return score + ply;
  if (score < -WIN_BOUND) return score - ply;
  return score;
}
int fromTT(int score, int ply) {
  if (score > WIN_BOUND) return score - ply;
  if (score < -WIN_BOUND) return score + ply;
  return score;
}

}  // namespace

bool Move::sameAs(const Move& other) const {
  if (from != other.from || steps != other.steps) return false;
  for (int i = 0; i < steps; i++) {
    if (path[i] != other.path[i]) return false;
  }
  return true;
}

int Move::captured(uint8_t* out) const {
  if (!jump) return 0;
  int at = from;
  for (int i = 0; i < steps; i++) {
    out[i] = static_cast<uint8_t>(middleOf(at, path[i]));
    at = path[i];
  }
  return steps;
}

// ---- Position ----

void Position::setStart() {
  for (int s = 0; s < SQUARES; s++) board[s] = s < 12 ? BLACK_MAN : (s >= 20 ? WHITE_MAN : EMPTY);
  side = BLACK;
  quiet = 0;
  key = computeHash();
}

bool Position::setup(const char* squares, const int sideToMove) {
  for (int s = 0; s < SQUARES; s++) {
    switch (squares[s]) {
      case 'b':
        board[s] = BLACK_MAN;
        break;
      case 'B':
        board[s] = BLACK_KING;
        break;
      case 'w':
        board[s] = WHITE_MAN;
        break;
      case 'W':
        board[s] = WHITE_KING;
        break;
      case '.':
        board[s] = EMPTY;
        break;
      default:
        return false;
    }
  }
  side = static_cast<uint8_t>(sideToMove == WHITE ? WHITE : BLACK);
  quiet = 0;
  key = computeHash();
  return true;
}

uint64_t Position::computeHash() const {
  uint64_t h = side == WHITE ? KEYS.side : 0;
  for (int s = 0; s < SQUARES; s++) {
    if (board[s]) h ^= KEYS.piece[board[s]][s];
  }
  return h;
}

int Position::pieces(const int color) const {
  int n = 0;
  for (const uint8_t p : board) n += p && colorOf(p) == color;
  return n;
}

void Position::addJumps(const int from, const uint8_t piece, const int at, Move& current, const uint32_t taken,
                        Move* out, int& n) const {
  const bool king = isKing(piece);
  const int color = colorOf(piece);
  bool extended = false;
  for (int d = 0; d < 4; d++) {
    if (!king && !forward(color, d)) continue;
    const int mid = GEO.step[at][d];
    const int land = GEO.jump[at][d];
    if (mid < 0 || land < 0) continue;
    const uint8_t victim = board[mid];
    if (!victim || colorOf(victim) == color || (taken >> mid) & 1u) continue;
    // The moving piece has left its square, so it may land there again.
    if (board[land] && land != from) continue;
    if (current.steps >= Move::MAX_STEPS) continue;
    current.path[current.steps++] = static_cast<uint8_t>(land);
    extended = true;
    if (!king && rowOf(land) == crownRow(color)) {
      // Crowning ends the move.
      if (n < MAX_MOVES) out[n++] = current;
    } else {
      addJumps(from, piece, land, current, taken | (1u << mid), out, n);
    }
    current.steps--;
  }
  if (!extended && current.steps > 0 && n < MAX_MOVES) out[n++] = current;
}

int Position::legalMoves(Move* out) const {
  int n = 0;
  for (int s = 0; s < SQUARES; s++) {
    const uint8_t piece = board[s];
    if (!piece || colorOf(piece) != side) continue;
    Move current;
    current.from = static_cast<uint8_t>(s);
    current.jump = true;
    addJumps(s, piece, s, current, 0, out, n);
  }
  if (n > 0) return n;
  for (int s = 0; s < SQUARES; s++) {
    const uint8_t piece = board[s];
    if (!piece || colorOf(piece) != side) continue;
    for (int d = 0; d < 4; d++) {
      if (!isKing(piece) && !forward(side, d)) continue;
      const int to = GEO.step[s][d];
      if (to < 0 || board[to] || n >= MAX_MOVES) continue;
      Move& m = out[n++];
      m = Move{};
      m.from = static_cast<uint8_t>(s);
      m.steps = 1;
      m.path[0] = static_cast<uint8_t>(to);
    }
  }
  return n;
}

void Position::make(const Move& move, Undo& undo) {
  undo.key = key;
  undo.quiet = quiet;
  const uint8_t piece = board[move.from];
  undo.moved = piece;
  board[move.from] = EMPTY;
  key ^= KEYS.piece[piece][move.from];
  int at = move.from;
  for (int i = 0; i < move.steps; i++) {
    if (move.jump) {
      const int mid = middleOf(at, move.path[i]);
      undo.captured[i] = board[mid];
      key ^= KEYS.piece[board[mid]][mid];
      board[mid] = EMPTY;
    }
    at = move.path[i];
  }
  const int to = move.to();
  const uint8_t placed = !isKing(piece) && rowOf(to) == crownRow(colorOf(piece)) ? crowned(piece) : piece;
  board[to] = placed;
  key ^= KEYS.piece[placed][to];
  if (move.jump || !isKing(piece)) {
    quiet = 0;
  } else if (quiet < 255) {
    quiet++;
  }
  side ^= 1;
  key ^= KEYS.side;
}

void Position::unmake(const Move& move, const Undo& undo) {
  side ^= 1;
  board[move.to()] = EMPTY;
  board[move.from] = undo.moved;
  if (move.jump) {
    int at = move.from;
    for (int i = 0; i < move.steps; i++) {
      board[middleOf(at, move.path[i])] = undo.captured[i];
      at = move.path[i];
    }
  }
  quiet = undo.quiet;
  key = undo.key;
}

int Position::evaluate() const {
  int score[2] = {0, 0};
  int material[2] = {0, 0};
  int total = 0;
  for (int s = 0; s < SQUARES; s++) {
    const uint8_t p = board[s];
    if (!p) continue;
    const int c = colorOf(p);
    const int r = rowOf(s);
    const int col = columnOf(s);
    total++;
    if (isKing(p)) {
      material[c] += 160;
      // Kings like the middle of the board.
      score[c] += 160 + 8 - 2 * (abs(2 * r - 7) + abs(2 * col - 7)) / 2;
      continue;
    }
    material[c] += 100;
    const int advance = c == BLACK ? r : 7 - r;  // 0..6
    score[c] += 100 + advance * 4 + (advance >= 5 ? 6 : 0);
    // Men on the back row keep the other side from crowning.
    if (advance == 0) score[c] += 8;
    if (col >= 2 && col <= 5 && r >= 3 && r <= 4) score[c] += 5;
  }
  int diff = score[BLACK] - score[WHITE];
  // Ahead in material: trading down helps.
  const int lead = material[BLACK] - material[WHITE];
  if (lead != 0) diff += lead * (24 - total) / 48;
  // In an ending, send the stronger side's kings after the other pieces.
  if (total <= 8 && (lead >= 150 || lead <= -150)) {
    const int winner = lead > 0 ? BLACK : WHITE;
    int bonus = 0;
    for (int s = 0; s < SQUARES; s++) {
      if (!board[s] || colorOf(board[s]) != winner || !isKing(board[s])) continue;
      int nearest = 14;
      for (int t = 0; t < SQUARES; t++) {
        if (!board[t] || colorOf(board[t]) == winner) continue;
        const int dist = abs(rowOf(s) - rowOf(t)) + abs(columnOf(s) - columnOf(t));
        if (dist < nearest) nearest = dist;
      }
      bonus += (14 - nearest) * 3;
    }
    diff += winner == BLACK ? bonus : -bonus;
  }
  return side == BLACK ? diff : -diff;
}

// ---- Game ----

void Game::reset() {
  pos.setStart();
  count = 0;
}

bool Game::setup(const char* squares, const int sideToMove) {
  count = 0;
  return pos.setup(squares, sideToMove);
}

bool Game::play(const Move& move) {
  if (count >= MAX_PLIES) return false;
  Move legal[Position::MAX_MOVES];
  const int n = pos.legalMoves(legal);
  for (int i = 0; i < n; i++) {
    if (!legal[i].sameAs(move)) continue;
    pos.make(legal[i], undos[count]);
    moves[count] = legal[i];
    indices[count] = static_cast<uint8_t>(i);
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

int Game::recentHashes(uint64_t* out, const int max) const {
  int n = pos.quietPlies();
  if (n > count) n = count;
  if (n > max) n = max;
  for (int i = 0; i < n; i++) out[i] = undos[count - n + i].key;
  return n;
}

Game::Status Game::status() const {
  Move legal[Position::MAX_MOVES];
  if (pos.legalMoves(legal) == 0) return pos.sideToMove() == BLACK ? Status::WhiteWins : Status::BlackWins;
  if (pos.quietPlies() >= QUIET_DRAW_PLIES) return Status::QuietDraw;
  int seen = 0;
  for (int k = 2; k <= pos.quietPlies() && k <= count; k += 2) {
    if (undos[count - k].key == pos.hash()) seen++;
  }
  if (seen >= 2) return Status::Repetition;
  if (count >= MAX_PLIES) return Status::TooLong;
  return Status::Playing;
}

void Game::save(uint8_t* out) const {
  memset(out, 0, STATE_SIZE);
  out[0] = static_cast<uint8_t>(count);
  out[1] = static_cast<uint8_t>(count >> 8);
  memcpy(out + 2, indices, count);
}

bool Game::restore(const uint8_t* in) {
  const int n = in[0] | (in[1] << 8);
  if (n > MAX_PLIES) return false;
  reset();
  Move legal[Position::MAX_MOVES];
  for (int i = 0; i < n; i++) {
    const int k = pos.legalMoves(legal);
    if (in[2 + i] >= k || !play(legal[in[2 + i]])) {
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
  const int reach = pos.quietPlies();
  for (int k = 2; k <= reach && k <= repCount; k += 2) {
    if (rep[repCount - k] == pos.hash()) return true;
  }
  return false;
}

int Engine::search(int depth, int alpha, int beta, const int ply) {
  if (tick()) return 0;
  if (ply > 0) {
    if (pos.quietPlies() >= Game::QUIET_DRAW_PLIES || isRepetition()) return 0;
    if (alpha < -WIN + ply) alpha = -WIN + ply;
    if (beta > WIN - ply - 1) beta = WIN - ply - 1;
    if (alpha >= beta) return alpha;
  }
  if (stackTop + Position::MAX_MOVES > STACK || ply >= MAX_PLY - 1 || repCount >= REP) return pos.evaluate();

  const int base = stackTop;
  const int n = pos.legalMoves(moveStack + base);
  if (n == 0) return -WIN + ply;
  // Jumps are never cut off; a single forced move costs no depth.
  if (depth <= 0 && !moveStack[base].jump) return pos.evaluate();
  if (n == 1 && ply > 0) depth++;
  stackTop += n;

  TTEntry& entry = tt[pos.hash() & (TT_SIZE - 1)];
  int ttBest = -1;
  if (entry.key == pos.hash()) {
    ttBest = entry.best;
    if (ply > 0 && entry.depth >= depth && depth > 0) {
      const int s = fromTT(entry.score, ply);
      if (entry.bound == BOUND_EXACT || (entry.bound == BOUND_LOWER && s >= beta) ||
          (entry.bound == BOUND_UPPER && s <= alpha)) {
        stackTop = base;
        return s;
      }
    }
  }
  for (int i = 0; i < n; i++) {
    const Move& m = moveStack[base + i];
    int32_t s = m.jump ? 1000 + m.steps * 100 : 0;
    if (i == ttBest) {
      s = 1000000;
    } else if (ply < MAX_PLY && m.sameAs(killers[ply])) {
      s += 500;
    }
    scoreStack[base + i] = s;
    indexStack[base + i] = static_cast<uint8_t>(i);
  }

  const int startAlpha = alpha;
  int best = -INF;
  int bestIndex = 0;
  for (int i = 0; i < n; i++) {
    int pick = i;
    for (int j = i + 1; j < n; j++) {
      if (scoreStack[base + j] > scoreStack[base + pick]) pick = j;
    }
    if (pick != i) {
      const Move m = moveStack[base + i];
      moveStack[base + i] = moveStack[base + pick];
      moveStack[base + pick] = m;
      const int32_t s = scoreStack[base + i];
      scoreStack[base + i] = scoreStack[base + pick];
      scoreStack[base + pick] = s;
      const uint8_t k = indexStack[base + i];
      indexStack[base + i] = indexStack[base + pick];
      indexStack[base + pick] = k;
    }
    const Move m = moveStack[base + i];
    Undo undo;
    pos.make(m, undo);
    rep[repCount++] = undo.key;
    const int s = -search(depth - 1, -beta, -alpha, ply + 1);
    repCount--;
    pos.unmake(m, undo);
    if (stopped) {
      stackTop = base;
      return 0;
    }
    if (s <= best) continue;
    best = s;
    bestIndex = indexStack[base + i];
    if (s <= alpha) continue;
    alpha = s;
    if (ply == 0) {
      rootBest = m;
      rootFound = true;
    }
    if (alpha >= beta) {
      if (!m.jump && ply < MAX_PLY) killers[ply] = m;
      break;
    }
  }
  stackTop = base;
  if (depth > 0) {
    entry.key = pos.hash();
    entry.best = static_cast<uint8_t>(bestIndex);
    entry.score = static_cast<int16_t>(toTT(best, ply));
    entry.depth = static_cast<uint8_t>(depth);
    entry.bound = best <= startAlpha ? BOUND_UPPER : (best >= beta ? BOUND_LOWER : BOUND_EXACT);
  }
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
  for (auto& k : killers) k = Move{};

  Move legal[Position::MAX_MOVES];
  const int n = pos.legalMoves(legal);
  if (n == 0) return Move{};
  if (n == 1) return legal[0];
  const int maxDepth = limits.maxDepth < 1 ? 1 : (limits.maxDepth > MAX_PLY - 16 ? MAX_PLY - 16 : limits.maxDepth);

  if (limits.noise > 0) {
    Move best = legal[0];
    int bestScore = -INF;
    for (int i = 0; i < n; i++) {
      Undo undo;
      pos.make(legal[i], undo);
      rep[repCount++] = undo.key;
      int s = -search(maxDepth - 1, -INF, INF, 1);
      repCount--;
      pos.unmake(legal[i], undo);
      if (stopped) break;
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      if (s > -WIN_BOUND && s < WIN_BOUND) s += static_cast<int>(seed % static_cast<uint32_t>(limits.noise + 1));
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
    rootFound = false;
    const int s = search(depth, -INF, INF, 0);
    if (rootFound) best = rootBest;
    if (stopped) break;
    reachedDepth = depth;
    lastScore = s;
    if (s > WIN_BOUND || s < -WIN_BOUND) break;
  }
  return best;
}

}  // namespace games::checkers
