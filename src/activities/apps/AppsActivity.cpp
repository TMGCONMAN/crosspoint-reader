#include "AppsActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <HalDisplay.h>

#include <algorithm>
#include <iterator>

#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "components/UITheme.h"
#include "components/icons/calculator.h"
#include "components/icons/checkers.h"
#include "components/icons/chess.h"
#include "components/icons/game2048.h"
#include "components/icons/library.h"
#include "components/icons/notes.h"
#include "components/icons/sudoku.h"
#include "fontIds.h"

namespace {

void openLibrary() { activityManager.goToLibrary(); }
void openNotes() { activityManager.goToNotes(); }
void openCalculator() { activityManager.goToCalculator(); }
void open2048() { activityManager.goToGame2048(); }
void openSudoku() { activityManager.goToSudoku(); }
void openChess() { activityManager.goToChess(); }
void openCheckers() { activityManager.goToCheckers(); }

// One entry per app; games and tools join this list.
const struct {
  StrId label;
  const uint8_t* icon;
  void (*open)();
} TILES[] = {
    {StrId::STR_LIBRARY, LibraryIcon, &openLibrary},          {StrId::STR_NOTES, NotesIcon, &openNotes},
    {StrId::STR_CALCULATOR, CalculatorIcon, &openCalculator}, {StrId::STR_GAME_2048, Game2048Icon, &open2048},
    {StrId::STR_GAME_SUDOKU, SudokuIcon, &openSudoku},        {StrId::STR_GAME_CHESS, ChessIcon, &openChess},
    {StrId::STR_GAME_CHECKERS, CheckersIcon, &openCheckers},
};
constexpr int TILE_COUNT = static_cast<int>(std::size(TILES));

constexpr int ICON_SIZE = 32;
constexpr int TILE_RADIUS = 14;

}  // namespace

AppsActivity::AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Apps", renderer, mappedInput) {}

void AppsActivity::onEnter() {
  Activity::onEnter();
  selected = 0;
  requestUpdate();
}

AppsActivity::Layout AppsActivity::layout() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  Layout out;
  // Three across in portrait, four in landscape.
  out.columns = renderer.getScreenWidth() > renderer.getScreenHeight() ? 4 : 3;
  out.gap = 16;
  const int width = content.width - 2 * metrics.contentSidePadding;
  out.size = std::max(48, (width - (out.columns - 1) * out.gap) / out.columns);
  out.x = content.x + metrics.contentSidePadding;
  out.y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  return out;
}

void AppsActivity::openTile(const int index) {
  if (index < 0 || index >= TILE_COUNT) return;
  TILES[index].open();
}

void AppsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome(HomeMenuItem::LIBRARY);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openTile(selected);
    return;
  }

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const Layout grid = layout();
    for (int i = 0; i < TILE_COUNT; i++) {
      const int x = grid.x + (i % grid.columns) * (grid.size + grid.gap);
      const int y = grid.y + (i / grid.columns) * (grid.size + grid.gap);
      if (tx >= x && tx < x + grid.size && ty >= y && ty < y + grid.size) {
        haptic_feedback::touchAction();
        selected = i;
        openTile(i);
        return;
      }
    }
    return;
  }

  // Left/Right step along the grid, Up/Down a row at a time.
  const int columns = layout().columns;
  int next = selected;
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::PageForward)) {
    next = (selected + 1) % TILE_COUNT;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
             mappedInput.wasReleased(MappedInputManager::Button::PageBack)) {
    next = (selected + TILE_COUNT - 1) % TILE_COUNT;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    next = std::min(TILE_COUNT - 1, selected + columns);
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    next = std::max(0, selected - columns);
  }
  if (next != selected) {
    selected = next;
    requestUpdate();
  }
}

void AppsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, tr(STR_APPS),
                 nullptr);

  const Layout grid = layout();
  const int labelHeight = renderer.getLineHeight(UI_10_FONT_ID);
  for (int i = 0; i < TILE_COUNT; i++) {
    const int x = grid.x + (i % grid.columns) * (grid.size + grid.gap);
    const int y = grid.y + (i / grid.columns) * (grid.size + grid.gap);
    // A heavier outline marks the button cursor.
    renderer.drawRoundedRect(x, y, grid.size, grid.size, i == selected ? 4 : 2, TILE_RADIUS, true);
    const int contentHeight = ICON_SIZE + 8 + labelHeight;
    const int iconY = y + (grid.size - contentHeight) / 2;
    renderer.drawIcon(TILES[i].icon, x + (grid.size - ICON_SIZE) / 2, iconY, ICON_SIZE);
    UITheme::drawCenteredWrappedText(renderer, Rect{x + 6, iconY + ICON_SIZE + 8, grid.size - 12, labelHeight},
                                     UI_10_FONT_ID, I18N.get(TILES[i].label), 1);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

#endif  // CROSSPOINT_MESH
