#include "HomeTabBar.h"

#include <GfxRenderer.h>

#include "icons/apps.h"
#include "icons/blocks.h"
#include "icons/folder.h"
#include "icons/hotspot.h"
#include "icons/library.h"
#include "icons/settings2.h"
#include "icons/transfer.h"

namespace fui = freeink::ui;

void HomeTabBar::draw(UiAppHost::UiScreen& screen, const fui::Rect rect, const int first, const int selectedValue,
                      const bool librarySlot, const fui::ActionId action, const bool apps) {
  firstValue = first;
  hasLibrarySlot = librarySlot;
  appsSlot = apps;
  int count = 0;
  for (int i = 0; i < SLOTS; ++i) {
    if (i == 2 && !hasLibrarySlot) continue;
    auto& tab = items[count];
    tab.value = firstValue + count;
    tab.selected = selectedValue == tab.value;
    tab.label = nullptr;
    ++count;
  }
  tabs.tabs = items.data();
  tabs.count = count;
  tabs.layout = fui::TabBarLayout::SpaceBetween;
  tabs.action = action;
  tabs.inputMask = fui::InputTouch;
  tabs.iconSize = 32;
  tabs.iconPainterUserData = this;
  tabs.iconPainter = [](fui::DrawTarget&, fui::Rect iconRect, const fui::TabItem& tab, uint8_t, void* user) {
    // Slot order matches HomeActivity::indexToMenuItem().
    static constexpr const uint8_t* ICONS[SLOTS] = {FolderIcon,   LibraryIcon,  BlocksIcon,
#if CROSSPOINT_MESH
                                                    HotspotIcon,
#endif
                                                    TransferIcon, Settings2Icon};
    const auto& self = *static_cast<HomeTabBar*>(user);
    const int index = tab.value - self.firstValue;
    const int icon = !self.hasLibrarySlot && index >= 2 ? index + 1 : index;
    if (icon < 0 || icon >= SLOTS) return false;
    self.renderer.drawIcon(icon == 1 && self.appsSlot ? AppsIcon : ICONS[icon], iconRect.x, iconRect.y, iconRect.width);
    return true;
  };
  tabs.tabStyles.normal.background = fui::Paint::solid(fui::Color::White);
  tabs.tabStyles.selected.background = fui::Paint::solid(fui::Color::White);
  tabs.selectedUnderline = 2;
  tabs.distributedSlotWidth = 0;
  fui::tabBar(screen.frame(), rect, tabs);
}
