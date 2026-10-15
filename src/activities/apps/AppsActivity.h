#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <I18n.h>

#include <cstdint>

#include "activities/Activity.h"

// The Apps launcher of the Mesh Carousel home: a grid of rounded square
// tiles, each opening an app or feature. Tap a tile, or move with the
// buttons and press Confirm. Back returns Home.
class AppsActivity final : public Activity {
 public:
  AppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Layout {
    int columns = 1;
    int x = 0;
    int y = 0;
    int size = 0;
    int gap = 0;
  };

  Layout layout() const;
  void openTile(int index);

  int selected = 0;
};

#endif  // CROSSPOINT_MESH
