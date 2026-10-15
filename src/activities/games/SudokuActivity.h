#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <Sudoku.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Sudoku: tap a cell, then a number on the pad below the board. Notes mode
// pencils candidates in instead; Undo steps back. Buttons move the cursor and
// Confirm picks a number from a popup. The game, its time and the best time
// for each level are kept on the SD card. Opened from Apps; Back returns there.
class SudokuActivity final : public Activity {
 public:
  SudokuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Pad keys after the nine digits.
  enum Key : int { KEY_ERASE = 9, KEY_NOTES = 10, KEY_UNDO = 11, KEY_COUNT = 12 };
  enum class Pending : uint8_t { None, Solved, ChooseLevel };

  struct Rect4 {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct Layout {
    Rect4 status;  // level and time, left of the New Game button
    Rect4 newGame;
    int boardX = 0, boardY = 0, cell = 0;
    Rect4 keys[KEY_COUNT];
  };

  Layout layout() const;
  void startGame(games::Sudoku::Difficulty level);
  void pressKey(int key);
  void enterDigit(uint8_t digit);  // 0 erases
  void afterChange();
  void chooseLevel();
  void pickDigit();
  void showSolved();
  void load();
  void save();
  uint32_t elapsedSeconds() const;
  void drawCell(const Layout& grid, int cell) const;

  games::Sudoku game;
  int selected = 40;  // the middle cell
  bool notesMode = false;
  // Time spent on this puzzle: saved seconds plus this visit's.
  uint32_t savedSeconds = 0;
  uint32_t visitStartMs = 0;
  uint32_t bestSeconds[3] = {};
  bool finished = false;  // solved; the time no longer runs
  bool dirty = false;
  bool fullRefresh = true;
  int fastRefreshes = 0;
  Pending pending = Pending::None;
  OptionPopup optionPopup;
  char messageText[96] = {0};
};

#endif  // CROSSPOINT_MESH
