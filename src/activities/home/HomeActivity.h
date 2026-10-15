#pragma once
#include <atomic>
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/CarouselHomeUi.h"
#include "components/CoverGridHomeUi.h"
#include "util/ButtonNavigator.h"

struct Rect;

class HomeActivity final : public Activity {
  std::unique_ptr<CoverGridHomeUi> coverGridUi;
#if CROSSPOINT_MESH
  // The Mesh Carousel home; the Mesh banner reads MeshService.
  std::unique_ptr<CarouselHomeUi> carouselUi;
  // The book in the middle; kept while the tabs are selected. Written by the
  // loop, read by the render task.
  std::atomic<int> carouselCenter{0};
  // Per book, one bit each: the center-size and side-size thumbnail is on
  // the card, or could not be made this visit.
  uint16_t centerThumbReady = 0;
  uint16_t sideThumbReady = 0;
  uint16_t centerThumbFailed = 0;
  uint16_t sideThumbFailed = 0;
  // Set by the render task once the first covers are in place; the loop
  // then brings the mesh link up, so BLE does not compete with the first
  // cover decoding for memory.
  std::atomic<bool> coversSettled{false};
  bool meshHeld = false;
  bool leavingToMesh = false;  // onExit hands the link to the Mesh screen
  void holdMeshLink();
  MeshBanner shownBanner;
  bool bannerPending = false;
  uint32_t lastMeshRevision = UINT32_MAX;
  uint8_t lastMeshPhase = UINT8_MAX;
  uint32_t nextBannerRepaintMs = 0;
  void syncCarouselCenter();
  void stepCarousel(int direction);
  void updateMeshBanner();
  MeshBanner nextBanner;
  void buildMeshBanner(MeshBanner& out) const;
  // Render task: makes missing thumbnails for the books on screen. True when
  // one was written.
  bool ensureCarouselThumbs(int center);
  bool generateThumb(RecentBook& book, int height);
#endif
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool hasPlugins = false;
  // The home "library" slot (index 2) shows Plugins when any plugin is
  // installed, otherwise OPDS. The index converters gate on its presence.
  bool hasLibrarySlot() const { return hasPlugins || hasOpdsServers; }
  bool hasContinueReading = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  const HomeMenuItem initialMenuItem;
  const bool cleanInitialRefresh;

  // Convert HomeMenuItem to menu index (used in onEnter)
  static int menuItemToIndex(HomeMenuItem item, bool hasOpdsUrl) {
    int i = 0;
    if (item == HomeMenuItem::FILE_BROWSER) return i;
    ++i;
    if (item == HomeMenuItem::LIBRARY) return i;
    ++i;
    if (item == HomeMenuItem::OPDS_BROWSER) return hasOpdsUrl ? i : 0;
    if (hasOpdsUrl) ++i;
#if CROSSPOINT_MESH
    if (item == HomeMenuItem::MESH) return i;
    ++i;
#endif
    if (item == HomeMenuItem::FILE_TRANSFER) return i;
    ++i;
    if (item == HomeMenuItem::SETTINGS_MENU) return i;
    return 0;
  }

  // Convert menu index to HomeMenuItem (used in loop)
  static HomeMenuItem indexToMenuItem(int idx, bool hasOpdsUrl) {
    int i = 0;
    if (idx == i++) return HomeMenuItem::FILE_BROWSER;
    if (idx == i++) return HomeMenuItem::LIBRARY;
    if (hasOpdsUrl && idx == i++) return HomeMenuItem::OPDS_BROWSER;
#if CROSSPOINT_MESH
    if (idx == i++) return HomeMenuItem::MESH;
#endif
    if (idx == i++) return HomeMenuItem::FILE_TRANSFER;
    if (idx == i) return HomeMenuItem::SETTINGS_MENU;
    return HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onFileBrowserOpen();
  void onLibraryOpen();
  void onSettingsOpen();
  void onFileTransferOpen();
  void onOpdsBrowserOpen();
  void onPluginsOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);
  void fillCoverGridFromLibrary(size_t maxBooks);
  void resolveGridCoverPaths();
  void loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE, bool cleanInitialRefresh = false)
      : Activity("Home", renderer, mappedInput),
        initialMenuItem(initialMenuItemValue),
        cleanInitialRefresh(cleanInitialRefresh) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
#if CROSSPOINT_MESH
  // While the banner's link is being set up, as on the Mesh screen.
  bool preventAutoSleep() override;
#endif
};
