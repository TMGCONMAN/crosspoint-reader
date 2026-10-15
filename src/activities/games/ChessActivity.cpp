#include "ChessActivity.h"

#if CROSSPOINT_MESH

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/chess_pieces.h"
#include "fontIds.h"

namespace chess = games::chess;

namespace {

constexpr char DIR_PATH[] = "/.crosspoint/games";
constexpr char SAVE_PATH[] = "/.crosspoint/games/chess.bin";
constexpr uint8_t SAVE_MAGIC[4] = {'C', 'H', 'S', 2};
// Version 1 had three levels (Easy, Medium, Hard), now 1..3.
constexpr uint8_t SAVE_MAGIC_V1[4] = {'C', 'H', 'S', 1};
constexpr size_t HEADER_SIZE = sizeof(SAVE_MAGIC) + 3 + 3 * 4;
constexpr size_t SAVE_SIZE = HEADER_SIZE + chess::Game::STATE_SIZE;
constexpr int FULL_REFRESH_EVERY = 20;
// The engine task's stack: the search recurses up to 64 plies.
constexpr uint32_t ENGINE_STACK_BYTES = 20480;
constexpr uint32_t STOP_WAIT_MS = 3000;

struct LevelSpec {
  int depth;
  int noise;
  bool quiescence;
  int blunderPercent;
  uint32_t thinkMs;
};
// Beginner looks one move ahead, misses what it leaves hanging and plays a
// random move one time in four. Easy looks one move ahead but sees captures
// through, with a random move now and then. Medium searches four plies
// within 3 s; Hard as deep as 6 s allows. In host matches each level beat
// the one below in nearly every game (ChessTest checks Easy over Beginner).
constexpr LevelSpec LEVEL_SPECS[4] = {
    {1, 250, false, 25, 1500}, {1, 120, true, 8, 2000}, {4, 0, true, 0, 3000}, {30, 0, true, 0, 6000}};

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

const char* levelName(int level) {
  if (level == 0) return tr(STR_GAME_BEGINNER);
  if (level == 1) return tr(STR_GAME_EASY);
  if (level == 2) return tr(STR_GAME_MEDIUM);
  return tr(STR_GAME_HARD);
}

void squareName(int square, char* out) {
  out[0] = static_cast<char>('a' + chess::fileOf(square));
  out[1] = static_cast<char>('1' + chess::rankOf(square));
  out[2] = 0;
}

// Upright 1-bit art (see chess_pieces.h) drawn pixel by pixel.
void blit(const GfxRenderer& renderer, const uint8_t* bits, int x, int y, bool state) {
  constexpr int size = CHESS_PIECE_SIZE;
  constexpr int rowBytes = (size + 7) / 8;
  for (int r = 0; r < size; r++) {
    const uint8_t* row = bits + r * rowBytes;
    for (int c = 0; c < size; c++) {
      if (row[c >> 3] & (0x80 >> (c & 7))) renderer.drawPixel(x + c, y + r, state);
    }
  }
}

}  // namespace

void ChessActivity::EngineDeleter::operator()(chess::Engine* e) const {
  e->~Engine();
  heap_caps_free(e);
}

ChessActivity::ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Chess", renderer, mappedInput) {}

ChessActivity::~ChessActivity() { stopComputer(); }

void ChessActivity::onEnter() {
  Activity::onEnter();
  // About 170 KB of search tables, in PSRAM, only while this screen is open.
  void* memory = heap_caps_malloc(sizeof(chess::Engine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory) {
    engine.reset(new (memory) chess::Engine());
  } else {
    LOG_ERR("CHESS", "No memory for the engine; the computer plays at random");
  }
  load();
  fullRefresh = true;
  requestUpdate();
  startComputer();
}

void ChessActivity::onExit() {
  stopComputer();
  // Also runs before sleep, so a game survives it.
  save();
  engine.reset();
  Activity::onExit();
}

void ChessActivity::load() {
  HalFile file;
  auto data = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[SAVE_SIZE]);
  bool restored = false;
  if (data && Storage.exists(SAVE_PATH) && Storage.openFileForRead("CHESS", SAVE_PATH, file)) {
    const int got = file.read(data.get(), SAVE_SIZE);
    file.close();
    const bool v2 = memcmp(data.get(), SAVE_MAGIC, sizeof(SAVE_MAGIC)) == 0;
    const bool v1 = memcmp(data.get(), SAVE_MAGIC_V1, sizeof(SAVE_MAGIC_V1)) == 0;
    if (got == static_cast<int>(SAVE_SIZE) && (v1 || v2)) {
      const uint8_t* p = data.get() + sizeof(SAVE_MAGIC);
      humanColor = p[0] == chess::BLACK ? chess::BLACK : chess::WHITE;
      level = std::min<int>(p[1] + (v1 ? 1 : 0), LEVELS - 1);
      resultCounted = p[2] != 0;
      for (int i = 0; i < 3; i++) record[i] = getU32(p + 3 + 4 * i);
      restored = game.restore(data.get() + HEADER_SIZE);
    }
  }
  if (!restored) {
    game.reset();
    resultCounted = false;
  }
  chosenSide = humanColor;
  cursorRow = 6;
  cursorColumn = 4;
}

void ChessActivity::save() {
  auto data = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[SAVE_SIZE]);
  if (!data) return;
  memcpy(data.get(), SAVE_MAGIC, sizeof(SAVE_MAGIC));
  uint8_t* p = data.get() + sizeof(SAVE_MAGIC);
  p[0] = static_cast<uint8_t>(humanColor);
  p[1] = static_cast<uint8_t>(level);
  p[2] = resultCounted ? 1 : 0;
  for (int i = 0; i < 3; i++) putU32(p + 3 + 4 * i, record[i]);
  game.save(data.get() + HEADER_SIZE);
  if (!Storage.ensureDirectoryExists(DIR_PATH)) return;
  HalFile file;
  if (!Storage.openFileForWrite("CHESS", SAVE_PATH, file)) return;
  file.write(data.get(), SAVE_SIZE);
  file.close();
}

// ---- The computer ----

bool ChessActivity::enginePoll(void* user) {
  auto* self = static_cast<ChessActivity*>(user);
  const uint32_t now = millis();
  // Let the idle task run so the watchdog stays quiet.
  if (now - self->lastYieldMs >= 20) {
    vTaskDelay(1);
    self->lastYieldMs = millis();
  }
  return self->abortSearch.load() || static_cast<int32_t>(now - self->deadlineMs) >= 0;
}

void ChessActivity::engineTask(void* user) {
  auto* self = static_cast<ChessActivity*>(user);
  const LevelSpec& spec = LEVEL_SPECS[self->level];
  chess::Engine::Limits limits;
  limits.maxDepth = spec.depth;
  limits.noise = spec.noise;
  limits.quiescence = spec.quiescence;
  limits.blunderPercent = spec.blunderPercent;
  self->computerMove = self->engine->think(self->game, limits, esp_random(), &enginePoll, self);
  self->done.store(true);
  // The activity may be destroyed once this is false; touch nothing after.
  self->thinking.store(false);
  vTaskDelete(nullptr);
}

void ChessActivity::startComputer() {
  if (thinking.load() || game.status() != Game::Status::Playing) return;
  if (game.position().sideToMove() == humanColor) return;
  if (!engine) {
    // Without the engine, any legal move.
    Move moves[chess::Position::MAX_MOVES];
    const int n = game.legalMoves(moves);
    if (n == 0) return;
    computerMove = moves[esp_random() % n];
    done.store(true);
    return;
  }
  abortSearch.store(false);
  done.store(false);
  deadlineMs = millis() + LEVEL_SPECS[level].thinkMs;
  lastYieldMs = millis();
  thinking.store(true);
  // CPU 0, beside the radio; rendering and input stay on CPU 1.
  if (xTaskCreatePinnedToCore(&engineTask, "chess-ai", ENGINE_STACK_BYTES, this, 1, &engineHandle, 0) != pdPASS) {
    LOG_ERR("CHESS", "Engine task not created");
    thinking.store(false);
    engineHandle = nullptr;
  }
}

void ChessActivity::stopComputer() {
  if (!thinking.load()) return;
  abortSearch.store(true);
  const uint32_t start = millis();
  // The search checks the flag every 1024 nodes, a few milliseconds.
  while (thinking.load() && millis() - start < STOP_WAIT_MS) delay(2);
  if (thinking.load()) {
    LOG_ERR("CHESS", "Engine did not stop");
    while (thinking.load()) delay(10);  // never free what it is using
  }
  done.store(false);
  engineHandle = nullptr;
}

void ChessActivity::finishComputer() {
  {
    RenderLock lock(*this);
    if (!game.play(computerMove)) {
      LOG_ERR("CHESS", "Engine move refused");
      return;
    }
  }
  engineHandle = nullptr;
  afterMove();
}

// ---- Moves ----

void ChessActivity::playHuman(const Move& move) {
  bool played = false;
  {
    RenderLock lock(*this);
    played = game.play(move);
    selected = -1;
  }
  if (played) {
    haptic_feedback::touchAction();
    afterMove();
  } else {
    requestUpdate();
  }
}

void ChessActivity::afterMove() {
  if (game.status() != Game::Status::Playing) {
    if (!resultCounted) countResult();
    save();
    pending = Pending::GameOver;
  } else {
    startComputer();
  }
  if (++fastRefreshes >= FULL_REFRESH_EVERY) fullRefresh = true;
  requestUpdate();
}

void ChessActivity::countResult() {
  resultCounted = true;
  const auto status = game.status();
  if (status == Game::Status::Checkmate) {
    // The side to move is mated.
    record[game.position().sideToMove() == humanColor ? 2 : 0]++;
  } else {
    record[1]++;
  }
}

void ChessActivity::tapSquare(const int square) {
  if (thinking.load() || done.load() || game.status() != Game::Status::Playing) return;
  if (game.position().sideToMove() != humanColor) return;
  const uint8_t piece = game.position().at(square);
  const bool own = piece && chess::colorOf(piece) == humanColor;
  if (selected >= 0 && square != selected) {
    Move moves[chess::Position::MAX_MOVES];
    const int n = game.legalMoves(moves);
    for (int i = 0; i < n; i++) {
      if (moves[i].from != selected || moves[i].to != square) continue;
      if (moves[i].promo) {
        // Ask which piece; the popup plays the move.
        promotion = moves[i];
        const char* options[4] = {tr(STR_CHESS_QUEEN), tr(STR_CHESS_ROOK), tr(STR_CHESS_BISHOP), tr(STR_CHESS_KNIGHT)};
        {
          RenderLock lock(*this);
          optionPopup.show(tr(STR_CHESS_PROMOTE), options, 4, 0, [this](const int choice) {
            static constexpr uint8_t PIECES[4] = {chess::QUEEN, chess::ROOK, chess::BISHOP, chess::KNIGHT};
            if (choice < 0 || choice > 3) return;
            Move m = promotion;
            m.promo = PIECES[choice];
            playHuman(m);
          });
        }
        requestUpdate();
        return;
      }
      playHuman(moves[i]);
      return;
    }
  }
  {
    RenderLock lock(*this);
    selected = own && square != selected ? square : -1;
  }
  requestUpdate();
}

void ChessActivity::undo() {
  stopComputer();
  {
    RenderLock lock(*this);
    selected = -1;
    if (game.plies() == 0) return;
    // Back to your turn, taking back at least your last move.
    game.undo();
    while (game.plies() > 0 && game.position().sideToMove() != humanColor) game.undo();
  }
  pending = Pending::None;
  requestUpdate();
  startComputer();  // you play Black and took back everything
}

void ChessActivity::newGame(const int color, const int newLevel) {
  stopComputer();
  {
    RenderLock lock(*this);
    game.reset();
    humanColor = color;
    level = newLevel;
    selected = -1;
    resultCounted = false;
    cursorRow = 6;
    cursorColumn = 4;
  }
  fullRefresh = true;
  save();
  requestUpdate();
  startComputer();
}

void ChessActivity::pressTool(const int tool) {
  if (tool == TOOL_UNDO) {
    undo();
  } else {
    chooseSide();
  }
}

void ChessActivity::chooseSide() {
  const char* options[3] = {tr(STR_GAME_PLAY_WHITE), tr(STR_GAME_PLAY_BLACK), tr(STR_CANCEL)};
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_GAME_NEW), options, 3, humanColor, [this](const int choice) {
      if (choice != 0 && choice != 1) return;
      chosenSide = choice == 1 ? chess::BLACK : chess::WHITE;
      pending = Pending::ChooseLevel;
    });
  }
  requestUpdate();
}

void ChessActivity::chooseLevel() {
  const char* options[LEVELS + 1] = {tr(STR_GAME_BEGINNER), tr(STR_GAME_EASY), tr(STR_GAME_MEDIUM), tr(STR_GAME_HARD),
                                     tr(STR_CANCEL)};
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_GAME_NEW), options, LEVELS + 1, level, [this](const int choice) {
      if (choice >= 0 && choice < LEVELS) newGame(chosenSide, choice);
    });
  }
  requestUpdate();
}

const char* ChessActivity::resultText() const {
  switch (game.status()) {
    case Game::Status::Checkmate:
      return game.position().sideToMove() == humanColor ? tr(STR_CHESS_YOU_LOSE) : tr(STR_CHESS_YOU_WIN);
    case Game::Status::Stalemate:
      return tr(STR_CHESS_STALEMATE);
    case Game::Status::FiftyMoves:
      return tr(STR_CHESS_DRAW_FIFTY);
    case Game::Status::Repetition:
      return tr(STR_GAME_DRAW_REPETITION);
    case Game::Status::Material:
      return tr(STR_CHESS_DRAW_MATERIAL);
    case Game::Status::TooLong:
      return tr(STR_GAME_DRAW_LONG);
    case Game::Status::Playing:
    default:
      return "";
  }
}

void ChessActivity::showGameOver() {
  snprintf(messageText, sizeof(messageText), "%s", resultText());
  const char* options[2] = {tr(STR_GAME_NEW), tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 2, 0, [this](const int choice) {
      if (choice == 0) {
        chooseSide();
      }
    });
  }
  requestUpdate();
}

// ---- Input ----

void ChessActivity::loop() {
  if (done.load() && !thinking.load()) {
    done.store(false);
    finishComputer();
  }
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (pending != Pending::None) {
    const Pending next = pending;
    pending = Pending::None;
    if (next == Pending::GameOver) showGameOver();
    if (next == Pending::ChooseLevel) chooseLevel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (selected >= 0) {
      selected = -1;
      requestUpdate();
    } else {
      activityManager.goToApps();
    }
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    showCursor = false;
    const Layout grid = layout();
    for (int t = 0; t < TOOL_COUNT; t++) {
      if (grid.tools[t].contains(tx, ty)) {
        haptic_feedback::touchAction();
        pressTool(t);
        return;
      }
    }
    const int span = grid.square * 8;
    if (tx >= grid.boardX && tx < grid.boardX + span && ty >= grid.boardY && ty < grid.boardY + span) {
      tapSquare(squareAtScreen((tx - grid.boardX) / grid.square, (ty - grid.boardY) / grid.square));
    } else if (selected >= 0) {
      selected = -1;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!showCursor) {
      showCursor = true;
    } else if (cursorRow < 0) {
      pressTool(cursorColumn);
      return;
    } else {
      tapSquare(squareAtScreen(cursorColumn, cursorRow));
      return;
    }
    requestUpdate();
    return;
  }

  // Front Left/Right move along a row, the side buttons up and down; above
  // the board is the Undo/New row.
  const int columns = cursorRow < 0 ? TOOL_COUNT : 8;
  int row = cursorRow;
  int column = cursorColumn;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    column = (column + 1) % columns;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    column = (column + columns - 1) % columns;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    row = row >= 7 ? -1 : row + 1;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    row = row <= -1 ? 7 : row - 1;
  } else {
    return;
  }
  if (row < 0 && cursorRow >= 0) column = cursorColumn >= 4 ? TOOL_NEW : TOOL_UNDO;
  if (row >= 0 && cursorRow < 0) column = cursorColumn == TOOL_NEW ? 7 : 0;
  cursorRow = row;
  cursorColumn = column;
  showCursor = true;
  requestUpdate();
}

// ---- Drawing ----

ChessActivity::Layout ChessActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int side = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int left = content.x + side;
  const int width = content.width - 2 * side;
  const int labelW = renderer.getTextWidth(SMALL_FONT_ID, "8") + 6;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID) + 2;
  const int line = renderer.getLineHeight(UI_12_FONT_ID);
  const int toolH = renderer.getLineHeight(UI_12_FONT_ID) + 12;
  constexpr int GAP = 8;
  Layout out;
  if (renderer.getScreenWidth() > renderer.getScreenHeight()) {
    // The board on the left, text and buttons on the right.
    out.square = std::min((bottom - top - labelH) / 8, (width - labelW) * 3 / 5 / 8);
    out.boardX = left + labelW;
    out.boardY = top;
    out.textX = out.boardX + out.square * 8 + GAP * 2;
    out.textW = left + width - out.textX;
    const int toolW = (out.textW - GAP) / 2;
    out.tools[TOOL_UNDO] = Rect4{out.textX, top, toolW, toolH};
    out.tools[TOOL_NEW] = Rect4{out.textX + toolW + GAP, top, toolW, toolH};
    out.textY = top + toolH + GAP * 2;
    return out;
  }
  const int toolW = std::max(84, width / 5);
  out.tools[TOOL_NEW] = Rect4{left + width - toolW, top, toolW, toolH};
  out.tools[TOOL_UNDO] = Rect4{out.tools[TOOL_NEW].x - GAP - toolW, top, toolW, toolH};
  out.textX = left;
  out.textY = top + toolH + GAP;
  out.textW = width;
  const int boardTop = out.textY + 2 * line + GAP * 2;
  out.square = std::min((width - labelW) / 8, (bottom - boardTop - labelH - line - GAP) / 8);
  out.boardX = left + labelW + (width - labelW - out.square * 8) / 2;
  out.boardY = boardTop;
  return out;
}

int ChessActivity::squareAtScreen(const int column, const int row) const {
  if (humanColor == chess::WHITE) return chess::squareAt(column, 7 - row);
  return chess::squareAt(7 - column, row);
}

void ChessActivity::screenOf(const int square, int& column, int& row) const {
  if (humanColor == chess::WHITE) {
    column = chess::fileOf(square);
    row = 7 - chess::rankOf(square);
  } else {
    column = 7 - chess::fileOf(square);
    row = chess::rankOf(square);
  }
}

void ChessActivity::drawPiece(const int x, const int y, const uint8_t piece) const {
  const int type = chess::typeOf(piece) - 1;
  blit(renderer, ChessPieceHalo[type], x, y, false);
  blit(renderer, chess::colorOf(piece) == chess::WHITE ? ChessPieceWhite[type] : ChessPieceBlack[type], x, y, true);
}

void ChessActivity::drawBoard(const Layout& grid) const {
  const int s = grid.square;
  const auto& pos = game.position();
  const bool hasLast = game.plies() > 0;
  const Move last = hasLast ? game.moveAt(game.plies() - 1) : Move{};
  const int checked = pos.inCheck() ? pos.kingSquare(pos.sideToMove()) : -1;
  Move moves[chess::Position::MAX_MOVES];
  const int n = selected >= 0 ? game.legalMoves(moves) : 0;

  for (int row = 0; row < 8; row++) {
    for (int column = 0; column < 8; column++) {
      const int sq = squareAtScreen(column, row);
      const int x = grid.boardX + column * s;
      const int y = grid.boardY + row * s;
      const bool dark = (chess::fileOf(sq) + chess::rankOf(sq)) % 2 == 0;
      if (sq == selected || sq == checked) {
        renderer.fillRectDither(x, y, s, s, Color::DarkGray);
      } else if (dark) {
        renderer.fillRectDither(x, y, s, s, Color::LightGray);
      }
      if (hasLast && (sq == last.from || sq == last.to)) renderer.drawRect(x + 2, y + 2, s - 4, s - 4, 2, true);
      const uint8_t piece = pos.at(sq);
      if (piece) drawPiece(x + (s - CHESS_PIECE_SIZE) / 2, y + (s - CHESS_PIECE_SIZE) / 2, piece);
    }
  }
  renderer.drawRect(grid.boardX - 1, grid.boardY - 1, s * 8 + 2, s * 8 + 2, 1, true);

  // Where the selected piece can go: a dot, or a ring around a capture.
  for (int i = 0; i < n; i++) {
    if (moves[i].from != selected) continue;
    int column = 0;
    int row = 0;
    screenOf(moves[i].to, column, row);
    const int x = grid.boardX + column * s;
    const int y = grid.boardY + row * s;
    if (pos.at(moves[i].to)) {
      renderer.drawRoundedRect(x + 2, y + 2, s - 4, s - 4, 3, s / 4, true);
    } else {
      const int d = std::max(10, s / 4);
      renderer.fillRoundedRect(x + (s - d) / 2 - 2, y + (s - d) / 2 - 2, d + 4, d + 4, d / 2 + 2, Color::White);
      renderer.fillRoundedRect(x + (s - d) / 2, y + (s - d) / 2, d, d, d / 2, Color::Black);
    }
  }

  if (showCursor && cursorRow >= 0) {
    const int x = grid.boardX + cursorColumn * s;
    const int y = grid.boardY + cursorRow * s;
    renderer.drawRect(x, y, s, s, 4, true);
    renderer.drawRect(x + 4, y + 4, s - 8, s - 8, 1, false);
  }

  // File letters below, rank numbers on the left.
  const int labelY = grid.boardY + s * 8 + 2;
  for (int i = 0; i < 8; i++) {
    char text[2] = {0, 0};
    const int sq = squareAtScreen(i, i);
    text[0] = static_cast<char>('a' + chess::fileOf(sq));
    const int w = renderer.getTextWidth(SMALL_FONT_ID, text);
    renderer.drawText(SMALL_FONT_ID, grid.boardX + i * s + (s - w) / 2, labelY, text);
    text[0] = static_cast<char>('1' + chess::rankOf(sq));
    const int h = renderer.getLineHeight(SMALL_FONT_ID);
    renderer.drawText(SMALL_FONT_ID, grid.boardX - renderer.getTextWidth(SMALL_FONT_ID, text) - 4,
                      grid.boardY + i * s + (s - h) / 2, text);
  }
}

void ChessActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_GAME_CHESS), nullptr);
  const Layout grid = layout();

  const char* toolLabels[TOOL_COUNT] = {tr(STR_GAME_UNDO), tr(STR_GAME_NEW)};
  for (int t = 0; t < TOOL_COUNT; t++) {
    const auto& r = grid.tools[t];
    const bool focus = showCursor && cursorRow < 0 && cursorColumn == t;
    if (focus) {
      renderer.fillRoundedRect(r.x, r.y, r.w, r.h, 10, Color::Black);
    } else {
      renderer.drawRoundedRect(r.x, r.y, r.w, r.h, t == TOOL_UNDO && game.plies() == 0 ? 1 : 2, 10, true);
    }
    const int w = renderer.getTextWidth(UI_10_FONT_ID, toolLabels[t], EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, r.x + (r.w - w) / 2, r.y + (r.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      toolLabels[t], !focus, EpdFontFamily::BOLD);
  }

  // Status: whose move or the result, then side, level and record.
  const int line = renderer.getLineHeight(UI_12_FONT_ID);
  char text[96];
  const bool over = game.status() != Game::Status::Playing;
  if (over) {
    snprintf(text, sizeof(text), "%s", resultText());
  } else if (thinking.load() || game.position().sideToMove() != humanColor) {
    snprintf(text, sizeof(text), "%s", tr(STR_GAME_THINKING));
  } else {
    snprintf(text, sizeof(text), "%s%s", tr(STR_GAME_YOUR_MOVE), game.position().inCheck() ? tr(STR_CHESS_CHECK) : "");
  }
  renderer.drawText(UI_12_FONT_ID, grid.textX, grid.textY, text, true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), tr(STR_GAME_SIDE_FORMAT),
           humanColor == chess::WHITE ? tr(STR_GAME_WHITE) : tr(STR_GAME_BLACK), levelName(level));
  renderer.drawText(UI_10_FONT_ID, grid.textX, grid.textY + line + 2, text);

  drawBoard(grid);

  // The last move and the record, under the board (or in the side panel).
  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  int y = landscape ? grid.textY + 3 * line : grid.boardY + grid.square * 8 + renderer.getLineHeight(SMALL_FONT_ID) + 8;
  if (game.plies() > 0) {
    const Move last = game.moveAt(game.plies() - 1);
    char from[3];
    char to[3];
    squareName(last.from, from);
    squareName(last.to, to);
    char move[8];
    snprintf(move, sizeof(move), "%s-%s", from, to);
    snprintf(text, sizeof(text), tr(STR_GAME_LAST_FORMAT), move);
    renderer.drawText(UI_10_FONT_ID, grid.textX, y, text);
    y += line;
  }
  snprintf(text, sizeof(text), tr(STR_GAME_RECORD_FORMAT), static_cast<unsigned long>(record[0]),
           static_cast<unsigned long>(record[1]), static_cast<unsigned long>(record[2]));
  renderer.drawText(UI_10_FONT_ID, grid.textX, y, text);

  const char* confirm = selected >= 0 ? tr(STR_GAME_MOVE) : tr(STR_SELECT);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (fullRefresh) {
    fullRefresh = false;
    fastRefreshes = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

#endif  // CROSSPOINT_MESH
