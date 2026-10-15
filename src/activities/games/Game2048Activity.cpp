#include "Game2048Activity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

using Direction = games::Game2048::Direction;

namespace {

constexpr char DIR_PATH[] = "/.crosspoint/games";
constexpr char SAVE_PATH[] = "/.crosspoint/games/2048.bin";
constexpr uint8_t SAVE_MAGIC[4] = {'G', '2', 'K', 1};
constexpr size_t SAVE_SIZE = sizeof(SAVE_MAGIC) + games::Game2048::STATE_SIZE + 4 + 1;
constexpr int FULL_REFRESH_EVERY = 25;
constexpr int TILE_RADIUS = 10;

}  // namespace

Game2048Activity::Game2048Activity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Game2048", renderer, mappedInput) {}

void Game2048Activity::onEnter() {
  Activity::onEnter();
  load();
  fullRefresh = true;
  requestUpdate();
}

void Game2048Activity::onExit() {
  // Also runs before sleep, so a game survives it.
  if (dirty) save();
  Activity::onExit();
}

void Game2048Activity::load() {
  best = 0;
  winShown = false;
  HalFile file;
  uint8_t data[SAVE_SIZE];
  bool restored = false;
  if (Storage.exists(SAVE_PATH) && Storage.openFileForRead("G2048", SAVE_PATH, file)) {
    const int got = file.read(data, sizeof(data));
    file.close();
    if (got == static_cast<int>(sizeof(data)) && memcmp(data, SAVE_MAGIC, sizeof(SAVE_MAGIC)) == 0) {
      const uint8_t* p = data + sizeof(SAVE_MAGIC);
      restored = game.restore(p);
      p += games::Game2048::STATE_SIZE;
      best = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
             (static_cast<uint32_t>(p[3]) << 24);
      winShown = p[4] != 0;
    }
  }
  if (!restored) {
    newGame();
  }
  dirty = !restored;
}

void Game2048Activity::save() const {
  uint8_t data[SAVE_SIZE];
  memcpy(data, SAVE_MAGIC, sizeof(SAVE_MAGIC));
  uint8_t* p = data + sizeof(SAVE_MAGIC);
  game.save(p);
  p += games::Game2048::STATE_SIZE;
  for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(best >> (8 * i));
  p[4] = winShown ? 1 : 0;
  if (!Storage.ensureDirectoryExists(DIR_PATH)) return;
  HalFile file;
  if (!Storage.openFileForWrite("G2048", SAVE_PATH, file)) return;
  file.write(data, sizeof(data));
  file.close();
}

void Game2048Activity::newGame() {
  // The render task reads the board.
  RenderLock lock(*this);
  game.reset(esp_random(), esp_random());
  winShown = false;
  dirty = true;
  fullRefresh = true;
}

void Game2048Activity::slide(const Direction direction) {
  {
    RenderLock lock(*this);
    if (!game.move(direction)) return;
    game.addRandomTile(esp_random());
  }
  best = std::max(best, game.score());
  dirty = true;
  if (!winShown && game.reachedWin()) {
    winShown = true;
    pending = Pending::Won;
  } else if (!game.canMove()) {
    pending = Pending::Over;
  }
  requestUpdate();
}

void Game2048Activity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (pending != Pending::None) {
    const Pending kind = pending;
    pending = Pending::None;
    // The board with the last tile first, then the message over it.
    showEnd(kind);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToApps();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmNewGame();
    return;
  }

  switch (mappedInput.wasSwipe()) {
    case MappedInputManager::SwipeDir::Up:
      haptic_feedback::touchAction();
      slide(Direction::Up);
      return;
    case MappedInputManager::SwipeDir::Down:
      haptic_feedback::touchAction();
      slide(Direction::Down);
      return;
    case MappedInputManager::SwipeDir::Left:
      haptic_feedback::touchAction();
      slide(Direction::Left);
      return;
    case MappedInputManager::SwipeDir::Right:
      haptic_feedback::touchAction();
      slide(Direction::Right);
      return;
    default:
      break;
  }
  // Front Left/Right slide sideways, the side buttons up and down.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    slide(Direction::Left);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    slide(Direction::Right);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    slide(Direction::Up);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    slide(Direction::Down);
  }
}

void Game2048Activity::confirmNewGame() {
  const char* options[2] = {tr(STR_CANCEL), tr(STR_GAME_NEW)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", tr(STR_GAME_NEW_CONFIRM), options, 2, 0, [this](const int choice) {
      if (choice == 1) newGame();
    });
  }
  requestUpdate();
}

void Game2048Activity::showEnd(const Pending kind) {
  if (dirty) {
    save();
    dirty = false;
  }
  if (kind == Pending::Won) {
    snprintf(messageText, sizeof(messageText), "%s", tr(STR_GAME_2048_WON));
  } else {
    snprintf(messageText, sizeof(messageText), tr(STR_GAME_OVER_FORMAT), static_cast<unsigned long>(game.score()));
  }
  const char* options[2] = {kind == Pending::Won ? tr(STR_GAME_KEEP_PLAYING) : tr(STR_BACK), tr(STR_GAME_NEW)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 2, 0, [this, kind](const int choice) {
      if (choice == 1) {
        newGame();
      } else if (kind == Pending::Over) {
        activityManager.goToApps();
      }
    });
  }
  requestUpdate();
}

void Game2048Activity::drawTile(const int x, const int y, const int size, const uint8_t exponent) const {
  if (exponent == 0) {
    renderer.drawRoundedRect(x, y, size, size, 1, TILE_RADIUS, true);
    return;
  }
  // Paper for small tiles, gray for the middle, a heavy frame, then black
  // with paper digits for the biggest: readable shades on 1-bit e-ink.
  bool inverted = false;
  if (exponent <= 2) {
    renderer.drawRoundedRect(x, y, size, size, 2, TILE_RADIUS, true);
  } else if (exponent <= 5) {
    renderer.fillRoundedRect(x, y, size, size, TILE_RADIUS, Color::LightGray);
    renderer.drawRoundedRect(x, y, size, size, 2, TILE_RADIUS, true);
  } else if (exponent <= 8) {
    renderer.drawRoundedRect(x, y, size, size, 5, TILE_RADIUS, true);
  } else {
    renderer.fillRoundedRect(x, y, size, size, TILE_RADIUS, Color::Black);
    inverted = true;
  }

  char number[12];
  snprintf(number, sizeof(number), "%lu", static_cast<unsigned long>(games::Game2048::value(exponent)));
  // The largest font the number fits in.
  static constexpr int FONTS[] = {NOTOSANS_18_FONT_ID, NOTOSANS_16_FONT_ID, NOTOSANS_14_FONT_ID, NOTOSANS_12_FONT_ID,
                                  UI_10_FONT_ID};
  int font = FONTS[std::size(FONTS) - 1];
  for (const int candidate : FONTS) {
    if (renderer.getTextWidth(candidate, number, EpdFontFamily::BOLD) <= size - 12) {
      font = candidate;
      break;
    }
  }
  const int width = renderer.getTextWidth(font, number, EpdFontFamily::BOLD);
  const int height = renderer.getLineHeight(font);
  renderer.drawText(font, x + (size - width) / 2, y + (size - height) / 2, number, !inverted, EpdFontFamily::BOLD);
}

void Game2048Activity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, "2048",
                 nullptr);

  // Score and best in two boxes under the header.
  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int side = metrics.contentSidePadding;
  const int boxW = (content.width - 2 * side - 12) / 2;
  const int labelH = renderer.getLineHeight(SMALL_FONT_ID);
  const int valueH = renderer.getLineHeight(UI_12_FONT_ID);
  const int boxH = labelH + valueH + 12;
  char value[16];
  for (int i = 0; i < 2; i++) {
    const int x = content.x + side + i * (boxW + 12);
    renderer.drawRoundedRect(x, top, boxW, boxH, 2, 8, true);
    const char* label = i == 0 ? tr(STR_GAME_SCORE) : tr(STR_GAME_BEST);
    snprintf(value, sizeof(value), "%lu", static_cast<unsigned long>(i == 0 ? game.score() : best));
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, x + (boxW - lw) / 2, top + 4, label);
    const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, x + (boxW - vw) / 2, top + 6 + labelH, value, true, EpdFontFamily::BOLD);
  }

  // The board: as large a square as fits between the scores and the hints.
  const int boardTop = top + boxH + metrics.verticalSpacing * 2;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int board = std::max(0, std::min(content.width - 2 * side, bottom - boardTop));
  const int boardX = content.x + (content.width - board) / 2;
  constexpr int GAP = 8;
  const int tile = (board - GAP * (games::Game2048::SIZE + 1)) / games::Game2048::SIZE;
  renderer.drawRoundedRect(boardX, boardTop, board, board, 3, TILE_RADIUS + 4, true);
  for (int r = 0; r < games::Game2048::SIZE; r++) {
    for (int c = 0; c < games::Game2048::SIZE; c++) {
      drawTile(boardX + GAP + c * (tile + GAP), boardTop + GAP + r * (tile + GAP), tile, game.at(r, c));
    }
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_GAME_NEW), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
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
