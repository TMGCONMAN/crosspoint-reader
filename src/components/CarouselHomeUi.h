#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <I18n.h>

#include <cstdint>
#include <vector>

#include "HomeCoverCache.h"
#include "HomeTabBar.h"
#include "RecentBooksStore.h"
#include "UiAppHost.h"
#include "components/media/cover-carousel.h"

class MappedInputManager;

// What the Mesh banner shows. HomeActivity copies it from MeshService on the
// main loop, under the render lock; the render task only reads this copy.
struct MeshBanner {
  enum class State : uint8_t { NoRadio, Off, Connecting, Ready };
  State state = State::NoRadio;
  StrId status = StrId::STR_MESH_NOT_CONNECTED;  // second line when there is nothing unread
  uint16_t unread = 0;
  char radioName[33] = {0};
  // The conversations and channels with unread messages, newest first.
  static constexpr uint8_t MAX_ENTRIES = 3;
  uint8_t entryCount = 0;
  char from[MAX_ENTRIES][33] = {};
  char preview[MAX_ENTRIES][64] = {};
  uint16_t entryUnread[MAX_ENTRIES] = {};

  bool operator==(const MeshBanner& other) const;
  bool operator!=(const MeshBanner& other) const { return !(*this == other); }
};

// The Mesh Carousel home: the recent books as a carousel (a large center
// cover between its neighbors), the Mesh banner, and the Cover Grid's tab row.
// Values of touch actions: a book's index, books + n for the tabs (as on the
// Cover Grid), BANNER_VALUE for the banner.
class CarouselHomeUi final : public UiAppHost {
 public:
  static constexpr int MAX_BOOKS = 10;
  static constexpr int BANNER_VALUE = 900;
  static_assert(MAX_BOOKS < BANNER_VALUE);

  explicit CarouselHomeUi(GfxRenderer& renderer);
  void begin(const std::vector<RecentBook>& books, bool hasLibrarySlot);
  // Render task, before drawing. selection is HomeActivity's flat index;
  // center the book in the middle.
  void setSelection(int selection, int center);
  void setBanner(const MeshBanner& banner) { this->banner = banner; }
  // A touch released on an action this pass, or -1.
  int selectedAction(const MappedInputManager& input);
  // Thumbnail heights the last draw laid out; 0 before the first draw.
  int centerThumbHeight() const { return centerThumb; }
  int sideThumbHeight() const { return sideThumb; }
  bool takeThumbHeightsChanged();
  // A thumbnail file was written: draw the covers again from the card.
  void invalidateCovers() { coverCache.invalidate(); }

 private:
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  void drawEmpty(UiScreen& screen);
  void drawCarousel(UiScreen& screen, freeink::ui::Rect body);
  void drawBanner(UiScreen& screen, freeink::ui::Rect rect);
  void paintCover(freeink::ui::Rect rect, int slot, int book, int thumbHeight);
  void loadProgress();

  HomeCoverCache coverCache;
  GfxRenderer& renderer;
  HomeTabBar tabBar;
  const std::vector<RecentBook>* books = nullptr;
  bool hasLibrarySlot = false;
  int selected = 0;
  int centerIndex = 0;
  int progressBook = -1;  // the book progress was read for
  int progress = -1;
  char progressText[12]{};
  int centerThumb = 0;
  int sideThumb = 0;
  bool thumbsChanged = false;
  int pending = -1;
  MeshBanner banner;
  char bannerCount[8]{};
  char bannerTitle[112]{};
  char bannerLine[112]{};
  // Component properties stay off the render task's stack.
  freeink::ui::CarouselProps carousel;
  freeink::ui::CarouselSlot slots[3];
};

#endif  // CROSSPOINT_MESH
