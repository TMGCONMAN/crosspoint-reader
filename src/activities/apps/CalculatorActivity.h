#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <Calculator.h>

#include "activities/Activity.h"

// A pocket calculator: a keypad to tap, or a cursor moved with the buttons
// and Confirm to press. The display shows the sum, its value as you type,
// and the last few results. Back returns to Apps.
class CalculatorActivity final : public Activity {
 public:
  CalculatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static constexpr int COLUMNS = 4;
  static constexpr int ROWS = 6;
  struct Rect4 {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  struct Layout {
    Rect4 display;
    Rect4 keys[ROWS][COLUMNS];  // a wide key fills its neighbour's cell too
  };

  Layout layout() const;
  void press(int row, int column);

  calc::Calculator calculator;
  int cursorRow = 5;
  int cursorColumn = 2;
  bool showCursor = false;
  bool fullRefresh = true;
  int fastRefreshes = 0;
};

#endif  // CROSSPOINT_MESH
