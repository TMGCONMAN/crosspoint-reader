#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <Checkers.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Checkers against the computer. Tap a piece, then where it lands; for a
// multiple jump tap the last square, or each hop in turn when two routes
// end on the same square. Jumps are compulsory. The computer thinks on its
// own task; Undo takes back your move and its reply. Buttons move a cursor
// over the board and the Undo/New buttons. The game, side, level and the
// won/drawn/lost record are kept on the SD card.
class CheckersActivity final : public Activity {
 public:
  CheckersActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~CheckersActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using Game = games::checkers::Game;
  using Move = games::checkers::Move;
  enum class Pending : uint8_t { None, GameOver, ChooseLevel };
  enum Tool : int { TOOL_UNDO = 0, TOOL_NEW = 1, TOOL_COUNT = 2 };
  static constexpr int LEVELS = 3;

  struct Rect4 {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct Layout {
    Rect4 tools[TOOL_COUNT];
    int textX = 0, textY = 0, textW = 0;
    int boardX = 0, boardY = 0, square = 0;
  };
  struct EngineDeleter {
    void operator()(games::checkers::Engine* engine) const;
  };

  Layout layout() const;
  // Board row/column to screen column/row, turned so your pieces are at
  // the bottom.
  void toScreen(int row, int column, int& screenColumn, int& screenRow) const;
  void fromScreen(int screenColumn, int screenRow, int& row, int& column) const;
  bool continues(const Move& move) const;  // matches the hops chosen so far
  void tapCell(int row, int column);
  void playHuman(const Move& move);
  void afterMove();
  void startComputer();
  void stopComputer();
  void finishComputer();
  void undo();
  void pressTool(int tool);
  void chooseSide();
  void chooseLevel();
  void newGame(int humanColor, int level);
  void showGameOver();
  void countResult();
  const char* resultText() const;
  void clearSelection();
  void load();
  void save();
  void drawPiece(int x, int y, int size, uint8_t piece) const;
  void drawBoard(const Layout& grid) const;

  static void engineTask(void* self);
  static bool enginePoll(void* self);

  Game game;
  std::unique_ptr<games::checkers::Engine, EngineDeleter> engine;
  int humanColor = games::checkers::BLACK;
  int level = 1;
  int selected = -1;  // square 0..31, or -1
  uint8_t hops[Move::MAX_STEPS] = {};
  int hopCount = 0;
  bool showCursor = false;
  int cursorColumn = 3;
  int cursorRow = 5;        // -1 is the Undo/New row
  uint32_t record[3] = {};  // won, drawn, lost
  bool resultCounted = false;
  bool fullRefresh = true;
  int fastRefreshes = 0;
  Pending pending = Pending::None;
  int chosenSide = games::checkers::BLACK;
  OptionPopup optionPopup;
  char messageText[96] = {0};

  // The computer's search; see ChessActivity for the same arrangement.
  TaskHandle_t engineHandle = nullptr;
  std::atomic<bool> thinking{false};
  std::atomic<bool> done{false};
  std::atomic<bool> abortSearch{false};
  Move computerMove;
  uint32_t deadlineMs = 0;
  uint32_t lastYieldMs = 0;
};

#endif  // CROSSPOINT_MESH
