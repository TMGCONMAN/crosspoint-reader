#pragma once

#include <array>

#include "UiAppHost.h"
#include "components/bars/tab-bar.h"
#include "mesh/MeshSupport.h"

class GfxRenderer;

// The icon tab row at the bottom of the cover homes: Folder, Library,
// Plugins/OPDS (when present), Mesh (when built in), File Transfer, Settings.
// Tab values continue after the books, matching HomeActivity's flat index.
class HomeTabBar {
 public:
  static constexpr int SLOTS = 5 + CROSSPOINT_MESH;

  explicit HomeTabBar(const GfxRenderer& renderer) : renderer(renderer) {}
  // appsSlot shows the Apps icon in the Library's place.
  void draw(UiAppHost::UiScreen& screen, freeink::ui::Rect rect, int firstValue, int selectedValue, bool hasLibrarySlot,
            freeink::ui::ActionId action, bool appsSlot = false);

 private:
  const GfxRenderer& renderer;
  int firstValue = 0;
  bool hasLibrarySlot = false;
  bool appsSlot = false;
  // Component properties stay off the render task's stack.
  freeink::ui::TabBarProps tabs;
  std::array<freeink::ui::TabItem, SLOTS> items;
};
