#include "CheckersActivity.h"

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
#include "fontIds.h"

namespace ck = games::checkers;

namespace {

constexpr char DIR_PATH[] = "/.crosspoint/games";
constexpr char SAVE_PATH[] = "/.crosspoint/games/checkers.bin";
constexpr uint8_t SAVE_MAGIC[4] = {'C', 'K', 'R', 1};
constexpr size_t HEADER_SIZE = sizeof(SAVE_MAGIC) + 3 + 3 * 4;
constexpr size_t SAVE_SIZE = HEADER_SIZE + ck::Game::STATE_SIZE;
constexpr int FULL_REFRESH_EVERY = 20;
constexpr uint32_t ENGINE_STACK_BYTES = 20480;  // the search recurses up to 64 plies
constexpr uint32_t STOP_WAIT_MS = 3000;

struct LevelSpec {
  int depth;
  int noise;
  uint32_t thinkMs;
};
// Easy looks three moves ahead and plays loosely; Hard searches for up to 6 s.
constexpr LevelSpec LEVEL_SPECS[3] = {{3, 90, 2000}, {6, 0, 3000}, {40, 0, 6000}};

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

const char* levelName(int level) {
  if (level == 0) return tr(STR_GAME_EASY);
  if (level == 1) return tr(STR_GAME_MEDIUM);
  return tr(STR_GAME_HARD);
}

}  // namespace

void CheckersActivity::EngineDeleter::operator()(ck::Engine* e) const {
  e->~Engine();
  heap_caps_free(e);
}

CheckersActivity::CheckersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Checkers", renderer, mappedInput) {}

CheckersActivity::~CheckersActivity() { stopComputer(); }

void CheckersActivity::onEnter() {
  Activity::onEnter();
  // About 170 KB of search tables, in PSRAM, only while this screen is open.
  void* memory = heap_caps_malloc(sizeof(ck::Engine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory) {
    engine.reset(new (memory) ck::Engine());
  } else {
    LOG_ERR("CHECKERS", "No memory for the engine; the computer plays at random");
  }
  load();
  fullRefresh = true;
  requestUpdate();
  startComputer();
}

void CheckersActivity::onExit() {
  stopComputer();
  // Also runs before sleep, so a game survives it.
  save();
  engine.reset();
  Activity::onExit();
}

void CheckersActivity::load() {
  HalFile file;
  auto data = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[SAVE_SIZE]);
  bool restored = false;
  if (data && Storage.exists(SAVE_PATH) && Storage.openFileForRead("CHECKERS", SAVE_PATH, file)) {
    const int got = file.read(data.get(), SAVE_SIZE);
    file.close();
    if (got == static_cast<int>(SAVE_SIZE) && memcmp(data.get(), SAVE_MAGIC, sizeof(SAVE_MAGIC)) == 0) {
      const uint8_t* p = data.get() + sizeof(SAVE_MAGIC);
      humanColor = p[0] == ck::WHITE ? ck::WHITE : ck::BLACK;
      level = std::min<int>(p[1], LEVELS - 1);
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
}

void CheckersActivity::save() {
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
  if (!Storage.openFileForWrite("CHECKERS", SAVE_PATH, file)) return;
  file.write(data.get(), SAVE_SIZE);
  file.close();
}

// ---- The computer ----

bool CheckersActivity::enginePoll(void* user) {
  auto* self = static_cast<CheckersActivity*>(user);
  const uint32_t now = millis();
  // Let the idle task run so the watchdog stays quiet.
  if (now - self->lastYieldMs >= 20) {
    vTaskDelay(1);
    self->lastYieldMs = millis();
  }
  return self->abortSearch.load() || static_cast<int32_t>(now - self->deadlineMs) >= 0;
}

void CheckersActivity::engineTask(void* user) {
  auto* self = static_cast<CheckersActivity*>(user);
  const LevelSpec& spec = LEVEL_SPECS[self->level];
  ck::Engine::Limits limits;
  limits.maxDepth = spec.depth;
  limits.noise = spec.noise;
  self->computerMove = self->engine->think(self->game, limits, esp_random(), &enginePoll, self);
  self->done.store(true);
  // The activity may be destroyed once this is false; touch nothing after.
  self->thinking.store(false);
  vTaskDelete(nullptr);
}

void CheckersActivity::startComputer() {
  if (thinking.load() || game.status() != Game::Status::Playing) return;
  if (game.position().sideToMove() == humanColor) return;
  if (!engine) {
    Move moves[ck::Position::MAX_MOVES];
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
  if (xTaskCreatePinnedToCore(&engineTask, "checkers-ai", ENGINE_STACK_BYTES, this, 1, &engineHandle, 0) != pdPASS) {
    LOG_ERR("CHECKERS", "Engine task not created");
    thinking.store(false);
    engineHandle = nullptr;
  }
}

void CheckersActivity::stopComputer() {
  if (!thinking.load()) return;
  abortSearch.store(true);
  const uint32_t start = millis();
  while (thinking.load() && millis() - start < STOP_WAIT_MS) delay(2);
  if (thinking.load()) {
    LOG_ERR("CHECKERS", "Engine did not stop");
    while (thinking.load()) delay(10);  // never free what it is using
  }
  done.store(false);
  engineHandle = nullptr;
}

void CheckersActivity::finishComputer() {
  {
    RenderLock lock(*this);
    if (!game.play(computerMove)) {
      LOG_ERR("CHECKERS", "Engine move refused");
      return;
    }
  }
  engineHandle = nullptr;
  afterMove();
}

// ---- Moves ----

void CheckersActivity::clearSelection() {
  selected = -1;
  hopCount = 0;
}

bool CheckersActivity::continues(const Move& move) const {
  if (move.from != selected || move.steps <= hopCount) return false;
  for (int i = 0; i < hopCount; i++) {
    if (move.path[i] != hops[i]) return false;
  }
  return true;
}

void CheckersActivity::playHuman(const Move& move) {
  bool played = false;
  {
    RenderLock lock(*this);
    played = game.play(move);
    clearSelection();
  }
  if (played) {
    haptic_feedback::touchAction();
    afterMove();
  } else {
    requestUpdate();
  }
}

void CheckersActivity::afterMove() {
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

void CheckersActivity::countResult() {
  resultCounted = true;
  const auto status = game.status();
  if (status == Game::Status::BlackWins || status == Game::Status::WhiteWins) {
    const int winner = status == Game::Status::BlackWins ? ck::BLACK : ck::WHITE;
    record[winner == humanColor ? 0 : 2]++;
  } else {
    record[1]++;
  }
}

void CheckersActivity::tapCell(const int row, const int column) {
  if (thinking.load() || done.load() || game.status() != Game::Status::Playing) return;
  if (game.position().sideToMove() != humanColor) return;
  const int square = ck::squareAt(row, column);
  Move moves[ck::Position::MAX_MOVES];
  const int n = game.legalMoves(moves);

  if (selected >= 0 && square >= 0) {
    // The next hop of a route chosen so far.
    int matches = 0;
    int complete = -1;
    int first = -1;
    for (int i = 0; i < n; i++) {
      if (!continues(moves[i]) || moves[i].path[hopCount] != square) continue;
      matches++;
      if (first < 0) first = i;
      if (moves[i].steps == hopCount + 1) complete = i;
    }
    if (matches == 1 || complete >= 0) {
      playHuman(moves[complete >= 0 ? complete : first]);
      return;
    }
    if (matches > 1) {
      RenderLock lock(*this);
      hops[hopCount++] = static_cast<uint8_t>(square);
      requestUpdate();
      return;
    }
    // Or straight to where one route ends.
    int ending = -1;
    int endings = 0;
    for (int i = 0; i < n; i++) {
      if (continues(moves[i]) && moves[i].to() == square) {
        ending = i;
        endings++;
      }
    }
    if (endings == 1) {
      playHuman(moves[ending]);
      return;
    }
  }

  // Select a piece that can move (when jumps are due, one that can jump).
  bool canMove = false;
  for (int i = 0; i < n && square >= 0; i++) canMove = canMove || moves[i].from == square;
  {
    RenderLock lock(*this);
    if (canMove && square != selected) {
      selected = square;
      hopCount = 0;
    } else {
      clearSelection();
    }
  }
  requestUpdate();
}

void CheckersActivity::undo() {
  stopComputer();
  {
    RenderLock lock(*this);
    clearSelection();
    if (game.plies() == 0) return;
    game.undo();
    while (game.plies() > 0 && game.position().sideToMove() != humanColor) game.undo();
  }
  pending = Pending::None;
  requestUpdate();
  startComputer();  // you play White and took back everything
}

void CheckersActivity::newGame(const int color, const int newLevel) {
  stopComputer();
  {
    RenderLock lock(*this);
    game.reset();
    humanColor = color;
    level = newLevel;
    clearSelection();
    resultCounted = false;
  }
  fullRefresh = true;
  save();
  requestUpdate();
  startComputer();
}

void CheckersActivity::pressTool(const int tool) {
  if (tool == TOOL_UNDO) {
    undo();
  } else {
    chooseSide();
  }
}

void CheckersActivity::chooseSide() {
  const char* options[3] = {tr(STR_CHECKERS_PLAY_BLACK), tr(STR_GAME_PLAY_WHITE), tr(STR_CANCEL)};
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_GAME_NEW), options, 3, humanColor == ck::WHITE ? 1 : 0, [this](const int choice) {
      if (choice != 0 && choice != 1) return;
      chosenSide = choice == 1 ? ck::WHITE : ck::BLACK;
      pending = Pending::ChooseLevel;
    });
  }
  requestUpdate();
}

void CheckersActivity::chooseLevel() {
  const char* options[4] = {tr(STR_GAME_EASY), tr(STR_GAME_MEDIUM), tr(STR_GAME_HARD), tr(STR_CANCEL)};
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_GAME_NEW), options, 4, level, [this](const int choice) {
      if (choice >= 0 && choice < LEVELS) newGame(chosenSide, choice);
    });
  }
  requestUpdate();
}

const char* CheckersActivity::resultText() const {
  switch (game.status()) {
    case Game::Status::BlackWins:
    case Game::Status::WhiteWins: {
      const int winner = game.status() == Game::Status::BlackWins ? ck::BLACK : ck::WHITE;
      return winner == humanColor ? tr(STR_CHECKERS_YOU_WIN) : tr(STR_CHECKERS_YOU_LOSE);
    }
    case Game::Status::QuietDraw:
      return tr(STR_CHECKERS_DRAW_QUIET);
    case Game::Status::Repetition:
      return tr(STR_GAME_DRAW_REPETITION);
    case Game::Status::TooLong:
      return tr(STR_GAME_DRAW_LONG);
    case Game::Status::Playing:
    default:
      return "";
  }
}

void CheckersActivity::showGameOver() {
  snprintf(messageText, sizeof(messageText), "%s", resultText());
  const char* options[2] = {tr(STR_GAME_NEW), tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 2, 0, [this](const int choice) {
      if (choice == 0) chooseSide();
    });
  }
  requestUpdate();
}

// ---- Input ----

void CheckersActivity::loop() {
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
      {
        RenderLock lock(*this);
        // Step back one hop, then drop the selection.
        if (hopCount > 0) {
          hopCount--;
        } else {
          clearSelection();
        }
      }
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
      int row = 0;
      int column = 0;
      fromScreen((tx - grid.boardX) / grid.square, (ty - grid.boardY) / grid.square, row, column);
      tapCell(row, column);
    } else if (selected >= 0) {
      {
        RenderLock lock(*this);
        clearSelection();
      }
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!showCursor) {
      showCursor = true;
      requestUpdate();
    } else if (cursorRow < 0) {
      pressTool(cursorColumn);
    } else {
      int row = 0;
      int column = 0;
      fromScreen(cursorColumn, cursorRow, row, column);
      tapCell(row, column);
    }
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

CheckersActivity::Layout CheckersActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int side = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int left = content.x + side;
  const int width = content.width - 2 * side;
  const int line = renderer.getLineHeight(UI_12_FONT_ID);
  const int toolH = line + 12;
  constexpr int GAP = 8;
  Layout out;
  if (renderer.getScreenWidth() > renderer.getScreenHeight()) {
    out.square = std::min((bottom - top) / 8, width * 3 / 5 / 8);
    out.boardX = left;
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
  out.square = std::min(width / 8, (bottom - boardTop - 2 * line - GAP) / 8);
  out.boardX = left + (width - out.square * 8) / 2;
  out.boardY = boardTop;
  return out;
}

void CheckersActivity::toScreen(const int row, const int column, int& screenColumn, int& screenRow) const {
  // Black starts at the top of the board; play Black and it is turned.
  if (humanColor == ck::BLACK) {
    screenColumn = 7 - column;
    screenRow = 7 - row;
  } else {
    screenColumn = column;
    screenRow = row;
  }
}

void CheckersActivity::fromScreen(const int screenColumn, const int screenRow, int& row, int& column) const {
  toScreen(screenRow, screenColumn, column, row);  // the turn is its own inverse
}

void CheckersActivity::drawPiece(const int x, const int y, const int size, const uint8_t piece) const {
  const int d = size * 4 / 5;
  const int px = x + (size - d) / 2;
  const int py = y + (size - d) / 2;
  const bool black = ck::colorOf(piece) == ck::BLACK;
  // A white edge so a black piece stands off the gray square.
  renderer.fillRoundedRect(px - 2, py - 2, d + 4, d + 4, d / 2 + 2, Color::White);
  renderer.fillRoundedRect(px, py, d, d, d / 2, black ? Color::Black : Color::White);
  if (!black) renderer.drawRoundedRect(px, py, d, d, 3, d / 2, true);
  // An inner ring, like the groove on a real checker.
  const int inner = d * 3 / 5;
  const int ix = px + (d - inner) / 2;
  const int iy = py + (d - inner) / 2;
  renderer.drawRoundedRect(ix, iy, inner, inner, 1, inner / 2, !black);
  if (!ck::isKing(piece)) return;
  // A crown for a king.
  const int w = d / 2;
  const int h = d / 3;
  const int cx = px + d / 2;
  const int x0 = cx - w / 2;
  const int x1 = cx + w / 2;
  const int base = py + d / 2 + h / 2;
  const int topY = base - h;
  const bool ink = !black;
  renderer.fillRect(x0, base - h / 4, w + 1, h / 4 + 1, ink);
  renderer.drawLine(x0, base, x0, topY, 2, ink);
  renderer.drawLine(x0, topY, x0 + w / 4, base - h / 2, 2, ink);
  renderer.drawLine(x0 + w / 4, base - h / 2, cx, topY, 2, ink);
  renderer.drawLine(cx, topY, x1 - w / 4, base - h / 2, 2, ink);
  renderer.drawLine(x1 - w / 4, base - h / 2, x1, topY, 2, ink);
  renderer.drawLine(x1, topY, x1, base, 2, ink);
}

void CheckersActivity::drawBoard(const Layout& grid) const {
  const int s = grid.square;
  const auto& pos = game.position();
  Move moves[ck::Position::MAX_MOVES];
  const bool yourTurn = !thinking.load() && pos.sideToMove() == humanColor && game.status() == Game::Status::Playing;
  const int n = yourTurn ? game.legalMoves(moves) : 0;
  const bool mustJump = n > 0 && moves[0].jump;
  const auto cellRect = [&](int square, int& x, int& y) {
    int column = 0;
    int row = 0;
    toScreen(ck::rowOf(square), ck::columnOf(square), column, row);
    x = grid.boardX + column * s;
    y = grid.boardY + row * s;
  };

  // Squares: the light ones stay white.
  for (int sq = 0; sq < ck::SQUARES; sq++) {
    int x = 0;
    int y = 0;
    cellRect(sq, x, y);
    renderer.fillRectDither(x, y, s, s, sq == selected ? Color::DarkGray : Color::LightGray);
  }
  renderer.drawRect(grid.boardX - 1, grid.boardY - 1, s * 8 + 2, s * 8 + 2, 1, true);

  // The last move: its ends framed, the pieces it took crossed out.
  if (game.plies() > 0) {
    const Move last = game.moveAt(game.plies() - 1);
    for (const int sq : {static_cast<int>(last.from), static_cast<int>(last.to())}) {
      int x = 0;
      int y = 0;
      cellRect(sq, x, y);
      renderer.drawRect(x + 2, y + 2, s - 4, s - 4, 2, true);
    }
    uint8_t taken[Move::MAX_STEPS];
    const int k = last.captured(taken);
    for (int i = 0; i < k; i++) {
      int x = 0;
      int y = 0;
      cellRect(taken[i], x, y);
      renderer.drawLine(x + s / 3, y + s / 3, x + s - s / 3, y + s - s / 3, 3, true);
      renderer.drawLine(x + s - s / 3, y + s / 3, x + s / 3, y + s - s / 3, 3, true);
    }
  }

  for (int sq = 0; sq < ck::SQUARES; sq++) {
    if (!pos.at(sq)) continue;
    int x = 0;
    int y = 0;
    cellRect(sq, x, y);
    drawPiece(x, y, s, pos.at(sq));
  }

  // When a jump is due, frame the pieces that can make it.
  if (mustJump && selected < 0) {
    for (int i = 0; i < n; i++) {
      int x = 0;
      int y = 0;
      cellRect(moves[i].from, x, y);
      renderer.drawRect(x + 1, y + 1, s - 2, s - 2, 3, true);
    }
  }

  // The hops chosen so far, and where the selected piece can land next.
  for (int i = 0; i < hopCount; i++) {
    int x = 0;
    int y = 0;
    cellRect(hops[i], x, y);
    const int d = s / 2;
    renderer.drawRoundedRect(x + (s - d) / 2, y + (s - d) / 2, d, d, 3, d / 2, true);
  }
  for (int i = 0; i < n && selected >= 0; i++) {
    if (!continues(moves[i])) continue;
    int x = 0;
    int y = 0;
    cellRect(moves[i].path[hopCount], x, y);
    const int d = std::max(10, s / 4);
    renderer.fillRoundedRect(x + (s - d) / 2 - 2, y + (s - d) / 2 - 2, d + 4, d + 4, d / 2 + 2, Color::White);
    renderer.fillRoundedRect(x + (s - d) / 2, y + (s - d) / 2, d, d, d / 2, Color::Black);
  }

  if (showCursor && cursorRow >= 0) {
    const int x = grid.boardX + cursorColumn * s;
    const int y = grid.boardY + cursorRow * s;
    renderer.drawRect(x, y, s, s, 4, true);
    renderer.drawRect(x + 4, y + 4, s - 8, s - 8, 1, false);
  }
}

void CheckersActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_GAME_CHECKERS), nullptr);
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

  // Status: whose move or the result, then side and level.
  const int line = renderer.getLineHeight(UI_12_FONT_ID);
  char text[112];
  const auto& pos = game.position();
  if (game.status() != Game::Status::Playing) {
    snprintf(text, sizeof(text), "%s", resultText());
  } else if (thinking.load() || pos.sideToMove() != humanColor) {
    snprintf(text, sizeof(text), "%s", tr(STR_GAME_THINKING));
  } else {
    Move moves[ck::Position::MAX_MOVES];
    const bool mustJump = game.legalMoves(moves) > 0 && moves[0].jump;
    snprintf(text, sizeof(text), "%s%s", tr(STR_GAME_YOUR_MOVE), mustJump ? tr(STR_CHECKERS_MUST_JUMP) : "");
  }
  renderer.drawText(UI_12_FONT_ID, grid.textX, grid.textY, text, true, EpdFontFamily::BOLD);
  snprintf(text, sizeof(text), tr(STR_GAME_SIDE_FORMAT),
           humanColor == ck::WHITE ? tr(STR_GAME_WHITE) : tr(STR_GAME_BLACK), levelName(level));
  renderer.drawText(UI_10_FONT_ID, grid.textX, grid.textY + line + 2, text);

  drawBoard(grid);

  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  int y = landscape ? grid.textY + 3 * line : grid.boardY + grid.square * 8 + 8;
  snprintf(text, sizeof(text), tr(STR_CHECKERS_PIECES_FORMAT), pos.pieces(humanColor), pos.pieces(humanColor ^ 1));
  renderer.drawText(UI_10_FONT_ID, grid.textX, y, text);
  y += line;
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
