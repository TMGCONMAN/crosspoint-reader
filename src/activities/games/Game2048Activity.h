#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <Game2048.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// 2048: swipe, or press the arrow buttons, to slide the tiles; equal tiles
// merge. The game and the best score are kept on the SD card, so a game
// carries on after sleep. Opened from Apps; Back returns there.
class Game2048Activity final : public Activity {
 public:
  Game2048Activity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Pending : uint8_t { None, Won, Over };

  void newGame();
  void slide(games::Game2048::Direction direction);
  void load();
  void save() const;
  void confirmNewGame();
  void showEnd(Pending kind);
  void drawTile(int x, int y, int size, uint8_t exponent) const;

  games::Game2048 game;
  uint32_t best = 0;
  bool winShown = false;  // the 2048 message is shown once a game
  bool dirty = false;     // changed since the last save
  // Moves since the last full refresh; e-ink ghosts after many fast ones.
  int fastRefreshes = 0;
  bool fullRefresh = true;
  Pending pending = Pending::None;
  OptionPopup optionPopup;
  char messageText[96] = {0};
};

#endif  // CROSSPOINT_MESH
