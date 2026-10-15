#include "SudokuActivity.h"

#if CROSSPOINT_MESH

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

using Level = games::Sudoku::Difficulty;

namespace {

constexpr char DIR_PATH[] = "/.crosspoint/games";
constexpr char SAVE_PATH[] = "/.crosspoint/games/sudoku.bin";
constexpr uint8_t SAVE_MAGIC[4] = {'S', 'D', 'K', 1};
constexpr size_t SAVE_SIZE = sizeof(SAVE_MAGIC) + games::Sudoku::STATE_SIZE + 4 * 4 + 1;
constexpr int FULL_REFRESH_EVERY = 30;
constexpr unsigned long NOTES_HOLD_MS = 800;

void putU32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

const char* levelName(Level level) {
  switch (level) {
    case Level::Easy:
      return tr(STR_GAME_EASY);
    case Level::Medium:
      return tr(STR_GAME_MEDIUM);
    case Level::Hard:
    default:
      return tr(STR_GAME_HARD);
  }
}

void formatTime(char* out, size_t size, uint32_t seconds) {
  if (seconds >= 3600) {
    snprintf(out, size, "%lu:%02lu:%02lu", static_cast<unsigned long>(seconds / 3600),
             static_cast<unsigned long>(seconds / 60 % 60), static_cast<unsigned long>(seconds % 60));
  } else {
    snprintf(out, size, "%lu:%02lu", static_cast<unsigned long>(seconds / 60),
             static_cast<unsigned long>(seconds % 60));
  }
}

}  // namespace

SudokuActivity::SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Sudoku", renderer, mappedInput) {}

void SudokuActivity::onEnter() {
  Activity::onEnter();
  load();
  visitStartMs = millis();
  fullRefresh = true;
  requestUpdate();
}

void SudokuActivity::onExit() {
  // Also runs before sleep, so a puzzle survives it.
  save();
  Activity::onExit();
}

uint32_t SudokuActivity::elapsedSeconds() const {
  if (finished) return savedSeconds;
  return savedSeconds + (millis() - visitStartMs) / 1000;
}

void SudokuActivity::load() {
  HalFile file;
  uint8_t data[SAVE_SIZE];
  bool restored = false;
  if (Storage.exists(SAVE_PATH) && Storage.openFileForRead("SUDOKU", SAVE_PATH, file)) {
    const int got = file.read(data, sizeof(data));
    file.close();
    if (got == static_cast<int>(sizeof(data)) && memcmp(data, SAVE_MAGIC, sizeof(SAVE_MAGIC)) == 0) {
      const uint8_t* p = data + sizeof(SAVE_MAGIC);
      restored = game.restore(p);
      p += games::Sudoku::STATE_SIZE;
      savedSeconds = getU32(p);
      for (int i = 0; i < 3; i++) bestSeconds[i] = getU32(p + 4 + 4 * i);
      finished = p[16] != 0;
    }
  }
  if (!restored) {
    // A first puzzle to look at; New Game offers the other levels.
    startGame(Level::Easy);
  }
}

void SudokuActivity::save() {
  uint8_t data[SAVE_SIZE];
  memcpy(data, SAVE_MAGIC, sizeof(SAVE_MAGIC));
  uint8_t* p = data + sizeof(SAVE_MAGIC);
  game.save(p);
  p += games::Sudoku::STATE_SIZE;
  putU32(p, elapsedSeconds());
  for (int i = 0; i < 3; i++) putU32(p + 4 + 4 * i, bestSeconds[i]);
  p[16] = finished ? 1 : 0;
  if (!Storage.ensureDirectoryExists(DIR_PATH)) return;
  HalFile file;
  if (!Storage.openFileForWrite("SUDOKU", SAVE_PATH, file)) return;
  file.write(data, sizeof(data));
  file.close();
  dirty = false;
}

void SudokuActivity::startGame(const Level level) {
  {
    RenderLock lock(*this);
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    game.generate(level, esp_random());
    selected = 40;
    notesMode = false;
    finished = false;
  }
  savedSeconds = 0;
  visitStartMs = millis();
  fullRefresh = true;
  save();
  requestUpdate();
}

SudokuActivity::Layout SudokuActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  const int side = metrics.contentSidePadding;
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int left = content.x + side;
  const int width = content.width - 2 * side;
  constexpr int GAP = 8;
  const int statusH = renderer.getLineHeight(UI_12_FONT_ID) + 12;
  Layout out;
  out.newGame = Rect4{left + width - width / 3, top, width / 3, statusH};
  out.status = Rect4{left, top, width - width / 3 - GAP, statusH};
  const int boardTop = top + statusH + GAP;

  const bool landscape = renderer.getScreenWidth() > renderer.getScreenHeight();
  int padCols = 6;
  int padRows = 2;
  int board = 0;
  int padX = left;
  int padY = 0;
  int padW = width;
  if (landscape) {
    // Board on the left, a 3x4 pad on the right.
    padCols = 3;
    padRows = 4;
    board = std::min(bottom - boardTop, width * 3 / 5);
    padX = left + board + GAP * 2;
    padY = boardTop;
    padW = left + width - padX;
  } else {
    const int padH = 2 * 56 + GAP;
    board = std::min(width, bottom - boardTop - padH - GAP * 2);
    padY = boardTop + board + GAP * 2;
  }
  out.cell = std::max(8, board / 9);
  out.boardX = left + (landscape ? 0 : (width - out.cell * 9) / 2);
  out.boardY = boardTop;

  const int padH = landscape ? std::min(bottom - padY, out.cell * 9) : 2 * 56 + GAP;
  const int keyW = (padW - GAP * (padCols - 1)) / padCols;
  const int keyH = (padH - GAP * (padRows - 1)) / padRows;
  for (int k = 0; k < KEY_COUNT; k++) {
    const int r = k / padCols;
    const int c = k % padCols;
    out.keys[k] = Rect4{padX + c * (keyW + GAP), padY + r * (keyH + GAP), keyW, keyH};
  }
  return out;
}

void SudokuActivity::enterDigit(const uint8_t digit) {
  bool changed = false;
  {
    RenderLock lock(*this);
    if (notesMode && digit != 0) {
      changed = game.toggleNote(selected, digit);
    } else {
      changed = game.place(selected, digit);
    }
  }
  if (changed) afterChange();
}

void SudokuActivity::afterChange() {
  dirty = true;
  if (!finished && game.solved()) {
    savedSeconds = elapsedSeconds();
    finished = true;
    const int level = static_cast<int>(game.difficulty());
    if (bestSeconds[level] == 0 || savedSeconds < bestSeconds[level]) bestSeconds[level] = savedSeconds;
    save();
    pending = Pending::Solved;
  }
  requestUpdate();
}

void SudokuActivity::pressKey(const int key) {
  if (key < 9) {
    enterDigit(static_cast<uint8_t>(key + 1));
  } else if (key == KEY_ERASE) {
    enterDigit(0);
  } else if (key == KEY_NOTES) {
    notesMode = !notesMode;
    requestUpdate();
  } else if (key == KEY_UNDO) {
    bool undone = false;
    {
      RenderLock lock(*this);
      undone = game.undo();
    }
    if (undone) afterChange();
  }
}

void SudokuActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (pending != Pending::None) {
    const Pending next = pending;
    pending = Pending::None;
    if (next == Pending::Solved) showSolved();
    if (next == Pending::ChooseLevel) chooseLevel();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, NOTES_HOLD_MS)) {
    notesMode = !notesMode;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (!finished) pickDigit();
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Layout grid = layout();
    if (grid.newGame.contains(tx, ty)) {
      haptic_feedback::touchAction();
      chooseLevel();
      return;
    }
    if (tx >= grid.boardX && tx < grid.boardX + grid.cell * 9 && ty >= grid.boardY &&
        ty < grid.boardY + grid.cell * 9) {
      selected = (ty - grid.boardY) / grid.cell * 9 + (tx - grid.boardX) / grid.cell;
      requestUpdate();
      return;
    }
    for (int k = 0; k < KEY_COUNT && !finished; k++) {
      if (grid.keys[k].contains(tx, ty)) {
        haptic_feedback::touchAction();
        pressKey(k);
        return;
      }
    }
    return;
  }

  // Front Left/Right move along the row, the side buttons up and down.
  int next = selected;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    next = (selected + 1) % 81;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    next = (selected + 80) % 81;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    next = (selected + 9) % 81;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    next = (selected + 72) % 81;
  }
  if (next != selected) {
    selected = next;
    requestUpdate();
  }
}

void SudokuActivity::pickDigit() {
  if (game.isGiven(selected)) return;
  const char* options[10] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", tr(STR_GAME_ERASE)};
  {
    RenderLock lock(*this);
    optionPopup.show(notesMode ? tr(STR_GAME_NOTES) : tr(STR_GAME_NUMBER), options, 10, 0, [this](const int choice) {
      if (choice >= 0 && choice < 9) enterDigit(static_cast<uint8_t>(choice + 1));
      if (choice == 9) enterDigit(0);
    });
  }
  requestUpdate();
}

void SudokuActivity::chooseLevel() {
  const char* options[4] = {tr(STR_GAME_EASY), tr(STR_GAME_MEDIUM), tr(STR_GAME_HARD), tr(STR_CANCEL)};
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_GAME_NEW), options, 4, static_cast<int>(game.difficulty()), [this](const int choice) {
      if (choice >= 0 && choice < 3) startGame(static_cast<Level>(choice));
    });
  }
  requestUpdate();
}

void SudokuActivity::showSolved() {
  char time[16];
  char best[16];
  formatTime(time, sizeof(time), savedSeconds);
  formatTime(best, sizeof(best), bestSeconds[static_cast<int>(game.difficulty())]);
  snprintf(messageText, sizeof(messageText), tr(STR_GAME_SOLVED_FORMAT), time, best);
  const char* options[2] = {tr(STR_GAME_NEW), tr(STR_BACK)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 2, 0, [this](const int choice) {
      if (choice == 0) {
        pending = Pending::ChooseLevel;
      } else {
        activityManager.goToApps();
      }
    });
  }
  requestUpdate();
}

void SudokuActivity::drawCell(const Layout& grid, const int cell) const {
  const int x = grid.boardX + (cell % 9) * grid.cell;
  const int y = grid.boardY + (cell / 9) * grid.cell;
  const int size = grid.cell;
  const uint8_t value = game.value(cell);
  const bool conflict = !game.isGiven(cell) && game.conflicts(cell);
  // A wrong-looking entry stands out in black.
  if (conflict) renderer.fillRect(x + 2, y + 2, size - 3, size - 3, true);
  if (cell == selected) {
    renderer.drawRect(x + 3, y + 3, size - 5, size - 5, 3, !conflict);
  } else if (value != 0 && game.value(selected) == value && !conflict) {
    // The same digit as the selected cell: a light underline.
    renderer.fillRect(x + size / 4, y + size - 7, size / 2, 2, true);
  }

  if (value != 0) {
    char digit[2] = {static_cast<char>('0' + value), 0};
    const bool given = game.isGiven(cell);
    const auto style = given ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int font = size >= 44 ? NOTOSANS_18_FONT_ID : (size >= 32 ? NOTOSANS_14_FONT_ID : UI_10_FONT_ID);
    const int w = renderer.getTextWidth(font, digit, style);
    const int h = renderer.getLineHeight(font);
    renderer.drawText(font, x + (size - w) / 2, y + (size - h) / 2, digit, !conflict, style);
    return;
  }
  const uint16_t notes = game.notes(cell);
  if (notes == 0) return;
  // Pencil marks in a 3x3 inside the cell.
  const int third = size / 3;
  const int h = renderer.getLineHeight(SMALL_FONT_ID);
  for (int d = 0; d < 9; d++) {
    if (!(notes & (1u << d))) continue;
    char digit[2] = {static_cast<char>('1' + d), 0};
    const int w = renderer.getTextWidth(SMALL_FONT_ID, digit);
    renderer.drawText(SMALL_FONT_ID, x + (d % 3) * third + (third - w) / 2, y + (d / 3) * third + (third - h) / 2,
                      digit);
  }
}

void SudokuActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight},
                 tr(STR_GAME_SUDOKU), nullptr);
  const Layout grid = layout();

  // Level and time, and the New Game button.
  char status[64];
  char time[16];
  formatTime(time, sizeof(time), elapsedSeconds());
  snprintf(status, sizeof(status), "%s  %s%s", levelName(game.difficulty()), time,
           notesMode ? tr(STR_GAME_NOTES_ON) : "");
  renderer.drawText(UI_12_FONT_ID, grid.status.x, grid.status.y + 6, status, true, EpdFontFamily::BOLD);
  renderer.drawRoundedRect(grid.newGame.x, grid.newGame.y, grid.newGame.w, grid.newGame.h, 2, 10, true);
  const char* newLabel = tr(STR_GAME_NEW);
  const int nw = renderer.getTextWidth(UI_10_FONT_ID, newLabel, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, grid.newGame.x + (grid.newGame.w - nw) / 2,
                    grid.newGame.y + (grid.newGame.h - renderer.getLineHeight(UI_10_FONT_ID)) / 2, newLabel, true,
                    EpdFontFamily::BOLD);

  // The board: thin cell lines, heavy box lines.
  const int span = grid.cell * 9;
  for (int i = 0; i <= 9; i++) {
    const int width = i % 3 == 0 ? 3 : 1;
    renderer.fillRect(grid.boardX + i * grid.cell - width / 2, grid.boardY, width, span + 1, true);
    renderer.fillRect(grid.boardX, grid.boardY + i * grid.cell - width / 2, span + 1, width, true);
  }
  for (int cell = 0; cell < games::Sudoku::CELLS; cell++) drawCell(grid, cell);

  // The pad: digits, Erase, Notes (filled while on) and Undo.
  for (int k = 0; k < KEY_COUNT; k++) {
    const auto& key = grid.keys[k];
    const bool on = k == KEY_NOTES && notesMode;
    if (on) {
      renderer.fillRoundedRect(key.x, key.y, key.w, key.h, 10, Color::Black);
    } else {
      renderer.drawRoundedRect(key.x, key.y, key.w, key.h, 2, 10, true);
    }
    char digit[2] = {static_cast<char>('1' + k), 0};
    const char* label =
        k < 9 ? digit
              : (k == KEY_ERASE ? tr(STR_GAME_ERASE) : (k == KEY_NOTES ? tr(STR_GAME_NOTES) : tr(STR_GAME_UNDO)));
    const int font = k < 9 ? NOTOSANS_18_FONT_ID : SMALL_FONT_ID;
    const int w = renderer.getTextWidth(font, label, EpdFontFamily::BOLD);
    renderer.drawText(font, key.x + (key.w - w) / 2, key.y + (key.h - renderer.getLineHeight(font)) / 2, label, !on,
                      EpdFontFamily::BOLD);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_NUMBER), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (fullRefresh || ++fastRefreshes >= FULL_REFRESH_EVERY) {
    fullRefresh = false;
    fastRefreshes = 0;
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}

#endif  // CROSSPOINT_MESH
