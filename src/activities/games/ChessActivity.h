#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <Chess.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Chess against the computer. Tap a piece, then where it should go; the
// possible squares are marked. The computer thinks on its own task so the
// screen stays responsive; Undo takes back your last move and its reply.
// Buttons move a cursor over the board and the Undo/New buttons. The game,
// side, level and the won/drawn/lost record are kept on the SD card.
class ChessActivity final : public Activity {
 public:
  ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~ChessActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  using Game = games::chess::Game;
  using Move = games::chess::Move;
  enum class Pending : uint8_t { None, GameOver, ChooseLevel };
  enum Tool : int { TOOL_UNDO = 0, TOOL_NEW = 1, TOOL_COUNT = 2 };
  static constexpr int LEVELS = 4;

  struct Rect4 {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct Layout {
    Rect4 tools[TOOL_COUNT];
    int textX = 0, textY = 0, textW = 0;  // status lines
    int boardX = 0, boardY = 0, square = 0;
  };
  struct EngineDeleter {
    void operator()(games::chess::Engine* engine) const;
  };

  Layout layout() const;
  // Screen column/row (0..7 from the top left) to a 0x88 square and back,
  // with the board turned when playing Black.
  int squareAtScreen(int column, int row) const;
  void screenOf(int square, int& column, int& row) const;
  void tapSquare(int square);
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
  void load();
  void save();
  void drawPiece(int x, int y, uint8_t piece) const;
  void drawBoard(const Layout& grid) const;

  static void engineTask(void* self);
  static bool enginePoll(void* self);

  Game game;
  std::unique_ptr<games::chess::Engine, EngineDeleter> engine;
  int humanColor = games::chess::WHITE;
  int level = 1;
  int selected = -1;  // 0x88 square, or -1
  bool showCursor = false;
  int cursorColumn = 4;
  int cursorRow = 6;        // -1 is the Undo/New row
  uint32_t record[3] = {};  // won, drawn, lost
  bool resultCounted = false;
  bool fullRefresh = true;
  int fastRefreshes = 0;
  Pending pending = Pending::None;
  int chosenSide = games::chess::WHITE;
  Move promotion;  // waiting for the piece choice
  OptionPopup optionPopup;
  char messageText[96] = {0};

  // The computer's search. Only the engine task touches the engine while
  // thinking is set; the main loop collects the move once done is set.
  TaskHandle_t engineHandle = nullptr;
  std::atomic<bool> thinking{false};
  std::atomic<bool> done{false};
  std::atomic<bool> abortSearch{false};
  Move computerMove;
  uint32_t deadlineMs = 0;
  uint32_t lastYieldMs = 0;
};

#endif  // CROSSPOINT_MESH
