#include "HomeActivity.h"

#include <Bitmap.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Memory.h>
#include <Utf8.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "activities/plugins/PluginCatalogActivity.h"  // anyPluginInstalled()
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/CoverReport.h"
#if CROSSPOINT_MESH
#include "mesh/MeshRadioStore.h"
#include "mesh/MeshService.h"
#endif

namespace {

// A thumbnail that is on the card and readable. One cut short by a power
// loss or a crash while it was written is deleted, so it is made again
// instead of showing the placeholder for ever.
bool thumbUsable(const std::string& path) {
  if (!Storage.exists(path.c_str())) return false;
  HalFile file;
  if (!Storage.openFileForRead("HOME", path, file)) return false;
  Bitmap bitmap(file);
  bool ok = bitmap.parseHeaders() == BmpReaderError::Ok && bitmap.getWidth() > 0 && bitmap.getHeight() > 0;
  // The header is written first: a file cut short has a good one and too
  // few pixel rows after it.
  if (ok) {
    const uint64_t needed = static_cast<uint64_t>(bitmap.getDataOffset()) +
                            static_cast<uint64_t>(bitmap.getRowBytes()) * static_cast<uint64_t>(bitmap.getHeight());
    ok = file.fileSize64() >= needed;
  }
  file.close();
  if (!ok) {
    LOG_ERR("HOME", "Damaged thumbnail %s; making it again", path.c_str());
    Storage.remove(path.c_str());
  }
  return ok;
}

}  // namespace

int HomeActivity::getMenuItemCount() const {
  int count = 4 + CROSSPOINT_MESH;  // File Browser, Library, File transfer, Settings, and Mesh when built in
  if (!recentBooks.empty()) {
    count += recentBooks.size();
  }
  if (hasLibrarySlot()) {
    count++;
  }
  return count;
}

void HomeActivity::loadRecentBooks(int maxBooks) {
  recentBooks.clear();
  const auto& books = RECENT_BOOKS.getBooks();
  bool coverHome = coverGridUi != nullptr;
#if CROSSPOINT_MESH
  coverHome = coverHome || carouselUi != nullptr;
#endif
  recentBooks.reserve(coverHome ? maxBooks : std::min(static_cast<int>(books.size()), maxBooks));

  for (const RecentBook& book : books) {
    // Limit to maximum number of recent books
    if (recentBooks.size() >= maxBooks) {
      break;
    }

    // Skip if file no longer exists
    if (RecentBooksStore::isMissing(book)) {
      continue;
    }

    recentBooks.push_back(book);
  }
}

void HomeActivity::fillCoverGridFromLibrary(const size_t maxBooks) {
  if (recentBooks.size() >= maxBooks) return;
  // Keep the index and record together off the task stack; reuse for every row.
  struct LibraryReader {
    library::LibraryIndexFile index;
    library::ClixRecord record;
  };
  auto reader = makeUniqueNoThrow<LibraryReader>();
  if (!reader) {
    LOG_ERR("HOME", "OOM: library index");
    return;
  }
  auto& index = reader->index;
  auto& record = reader->record;
  if (!index.open(library::libraryIndexPath())) {
    index.close();
    GUI.drawPopup(renderer, tr(STR_LIBRARY_REBUILDING));
    library::BuildStats stats;
    if (!library::buildLibraryIndex("/", stats, SETTINGS.libraryUseMetadata != 0) ||
        !index.open(library::libraryIndexPath())) {
      LOG_ERR("HOME", "Cannot populate cover grid from library");
      return;
    }
  }
  for (uint16_t row = 0; row < index.bookCount() && recentBooks.size() < maxBooks; ++row) {
    RecentBook book;
    if (!index.readRecord(index.ordinalForRow(library::SortOrder::RecentDesc, row), record) ||
        !index.readPath(record, book.path))
      continue;
    if (std::any_of(recentBooks.begin(), recentBooks.end(),
                    [&](const RecentBook& existing) { return existing.path == book.path; }) ||
        RecentBooksStore::isMissing(book))
      continue;
    if (!index.readTitle(record, book.title) && !index.readName(record, book.title)) continue;
    index.readAuthor(record, book.author);
    if (index.ioFailed()) break;
    recentBooks.push_back(std::move(book));
  }
}

void HomeActivity::resolveGridCoverPaths() {
  for (auto& book : recentBooks) {
    if (!book.coverBmpPath.empty()) continue;
    // Constructors only derive cache paths; no metadata parsing or image generation.
    // Keep these large objects off the task stack and release each before the next book.
    if (FsHelpers::hasReflowableBookExtension(book.path)) {
      auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
      if (!epub) {
        LOG_ERR("HOME", "OOM: EPUB thumbnail path");
        continue;
      }
      book.coverBmpPath = epub->getThumbBmpPath();
    } else if (FsHelpers::hasXtcExtension(book.path)) {
      auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
      if (!xtc) {
        LOG_ERR("HOME", "OOM: XTC thumbnail path");
        continue;
      }
      book.coverBmpPath = xtc->getThumbBmpPath();
    }
  }
}

void HomeActivity::loadGridCover(RecentBook& book, int height, bool& showingLoading, Rect& popupRect) {
  if (!book.coverBmpPath.empty() && thumbUsable(UITheme::getCoverThumbPath(book.coverBmpPath, height))) return;
  // Only one parser lives at a time; EPUB/XTC objects exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return;
    }
    book.coverBmpPath = epub->getThumbBmpPath();
    if (thumbUsable(epub->getThumbBmpPath(height))) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (epub->generateThumbBmpFromSource(height)) {
      return;
    }
    reportCoverFailure(book.path, height);
  } else if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return;
    }
    book.coverBmpPath = xtc->getThumbBmpPath();
    if (thumbUsable(xtc->getThumbBmpPath(height))) return;
    if (!showingLoading) {
      showingLoading = true;
      popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      GUI.fillPopupProgress(renderer, popupRect, 0);
    }
    if (xtc->load() && xtc->generateThumbBmp(height)) {
      return;
    }
    reportCoverFailure(book.path, height);
  }
  book.coverBmpPath.clear();
}

void HomeActivity::loadRecentCovers(int coverHeight) {
  recentsLoading = true;
  bool showingLoading = false;
  Rect popupRect;

  int progress = 0;
  for (RecentBook& book : recentBooks) {
    // The cover grid shares one slot size; generating at any other height
    // would rescale the dithered thumb at draw time and alias badly.
    const int thumbHeight = coverGridUi ? coverGridUi->thumbHeightFor() : coverHeight;
    if (coverGridUi) {
      loadGridCover(book, thumbHeight, showingLoading, popupRect);
      ++progress;
      if (showingLoading) GUI.fillPopupProgress(renderer, popupRect, progress * 100 / recentBooks.size());
      continue;
    }
    if (!book.coverBmpPath.empty()) {
      std::string coverPath = UITheme::getCoverThumbPath(book.coverBmpPath, thumbHeight);
      if (!Storage.exists(coverPath.c_str())) {
        // If epub/txt/md, try to load the metadata for title/author and cover
        if (FsHelpers::hasReflowableBookExtension(book.path)) {
          Epub epub(book.path, "/.crosspoint");
          // Skip loading css since we only need metadata here
          epub.load(false, true);

          // Try to generate thumbnail image for Continue Reading card
          if (!showingLoading) {
            showingLoading = true;
            popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
          }
          GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
          bool success = epub.generateThumbBmp(thumbHeight);
          if (!success) {
            reportCoverFailure(book.path, thumbHeight);
            RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
            book.coverBmpPath = "";
          }
          coverRendered = false;
          requestUpdate();
        } else if (FsHelpers::hasXtcExtension(book.path)) {
          // Handle XTC file
          Xtc xtc(book.path, "/.crosspoint");
          if (xtc.load()) {
            // Try to generate thumbnail image for Continue Reading card
            if (!showingLoading) {
              showingLoading = true;
              popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
            }
            GUI.fillPopupProgress(renderer, popupRect, 10 + progress * (90 / recentBooks.size()));
            bool success = xtc.generateThumbBmp(thumbHeight);
            if (!success) {
              RECENT_BOOKS.updateBook(book.path, book.title, book.author, "");
              book.coverBmpPath = "";
            }
            coverRendered = false;
            requestUpdate();
          }
        }
      }
    }
    progress++;
  }

  recentsLoaded = true;
  recentsLoading = false;
}

void HomeActivity::onEnter() {
  Activity::onEnter();

  hasOpdsServers = OPDS_STORE.hasServers();
  hasPlugins = anyPluginInstalled();

  const auto& metrics = UITheme::getInstance().getMetrics();
  if (UITheme::getInstance().hasCoverGridHome()) {
    // Screen-lifetime interaction tables and component properties exceed the stack budget.
    coverGridUi = makeUniqueNoThrow<CoverGridHomeUi>(renderer);
    if (!coverGridUi) LOG_ERR("HOME", "OOM: cover grid UI; using standard home");
  }
  int maxBooks = coverGridUi ? CoverGridHomeUi::MAX_BOOKS : metrics.homeRecentBooksCount;
#if CROSSPOINT_MESH
  // Built here and published only once begin() has run: a render left over
  // from the last screen may run before onEnter() returns.
  std::unique_ptr<CarouselHomeUi> carousel;
  if (UITheme::getInstance().hasCarouselHome()) {
    carousel = makeUniqueNoThrow<CarouselHomeUi>(renderer);
    if (!carousel) LOG_ERR("HOME", "OOM: carousel UI; using standard home");
  }
  if (carousel) maxBooks = CarouselHomeUi::MAX_BOOKS;
#endif
  loadRecentBooks(maxBooks);
  hasContinueReading = !recentBooks.empty();
  if (coverGridUi) {
    fillCoverGridFromLibrary(CoverGridHomeUi::MAX_BOOKS);
    resolveGridCoverPaths();
    coverGridUi->begin(recentBooks, hasLibrarySlot(), hasContinueReading);
  }
#if CROSSPOINT_MESH
  if (carousel) {
    fillCoverGridFromLibrary(CarouselHomeUi::MAX_BOOKS);
    resolveGridCoverPaths();
    carousel->begin(recentBooks, hasLibrarySlot());
    carouselCenter = 0;
    centerThumbReady = sideThumbReady = centerThumbFailed = sideThumbFailed = 0;
    coversSettled = false;
    meshHeld = false;
    leavingToMesh = false;
    MESH_RADIO_STORE.loadFromFile();
    // A link that is already up (back from the Mesh screen) is kept from
    // the start; a new one waits for the first covers.
    if (MESH_SERVICE.phase() != MeshService::Phase::Off) holdMeshLink();
    buildMeshBanner(shownBanner);
    carousel->setBanner(shownBanner);
    lastMeshRevision = MESH_SERVICE.revision();
    lastMeshPhase = static_cast<uint8_t>(MESH_SERVICE.phase());
    bannerPending = false;
    RenderLock lock(*this);
    carouselUi = std::move(carousel);
  }
#endif

  const auto base = static_cast<int>(recentBooks.size());
  selectorIndex = initialMenuItem == HomeMenuItem::NONE ? 0 : base + menuItemToIndex(initialMenuItem, hasLibrarySlot());

  // Trigger first update
  requestUpdate();
}

void HomeActivity::onExit() {
  Activity::onExit();

  coverGridUi.reset();
#if CROSSPOINT_MESH
  carouselUi.reset();
  // Kept for a few seconds only for the Mesh screen; a book, Wi-Fi or USB
  // Drive gets BLE's memory back at once.
  if (meshHeld) MESH_SERVICE.release(leavingToMesh);
  meshHeld = false;
#endif

  // Free the stored cover buffer if any
  freeCoverBuffer();
}

bool HomeActivity::storeCoverBuffer() {
  // render() must have already set the cover rect; without it we'd be back to
  // cloning the whole framebuffer.
  if (coverRectW <= 0 || coverRectH <= 0) return false;
  freeCoverBuffer();
  const size_t needed = renderer.getRegionByteSize(coverRectX, coverRectY, coverRectW, coverRectH);
  if (needed == 0) return false;
  coverBuffer = static_cast<uint8_t*>(malloc(needed));
  if (!coverBuffer) {
    LOG_ERR("HOME", "OOM: cover buffer (%u bytes)", (unsigned)needed);
    return false;
  }
  coverBufferSize = needed;
  if (!renderer.copyRegionToBuffer(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize)) {
    free(coverBuffer);
    coverBuffer = nullptr;
    coverBufferSize = 0;
    return false;
  }
  return true;
}

bool HomeActivity::restoreCoverBuffer() {
  if (!coverBuffer || coverRectW <= 0 || coverRectH <= 0) return false;
  return renderer.copyBufferToRegion(coverRectX, coverRectY, coverRectW, coverRectH, coverBuffer, coverBufferSize);
}

void HomeActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferSize = 0;
  coverBufferStored = false;
}

void HomeActivity::loop() {
  const int menuCount = getMenuItemCount();
  const auto& metrics = UITheme::getInstance().getMetrics();

  auto activateSelection = [this] {
    if (selectorIndex < recentBooks.size()) {
      onSelectBook(recentBooks[selectorIndex].path);
      return;
    }
    const int menuIndex = selectorIndex - static_cast<int>(recentBooks.size());
    switch (indexToMenuItem(menuIndex, hasLibrarySlot())) {
      case HomeMenuItem::FILE_BROWSER:
        onFileBrowserOpen();
        break;
      case HomeMenuItem::LIBRARY:
#if CROSSPOINT_MESH
        // The Mesh Carousel's tab opens Apps, where the Library is a tile.
        if (carouselUi) {
          activityManager.goToApps();
          break;
        }
#endif
        onLibraryOpen();
        break;
      case HomeMenuItem::OPDS_BROWSER:  // the library slot
        hasPlugins ? onPluginsOpen() : onOpdsBrowserOpen();
        break;
#if CROSSPOINT_MESH
      case HomeMenuItem::MESH:
        leavingToMesh = activityManager.goToMesh();
        break;
#endif
      case HomeMenuItem::FILE_TRANSFER:
        onFileTransferOpen();
        break;
      case HomeMenuItem::SETTINGS_MENU:
        onSettingsOpen();
        break;
      default:
        break;
    }
  };

  // Cover grid home splits navigation by button group (see below); the flat
  // next/previous cycle is for the classic list home only.
  bool flatMenu = !coverGridUi;
#if CROSSPOINT_MESH
  flatMenu = flatMenu && !carouselUi;
#endif
  if (flatMenu) {
    buttonNavigator.onNext([this, menuCount] {
      selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, menuCount] {
      selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
      requestUpdate();
    });
  }

  const auto swipe = mappedInput.wasSwipe();
#if CROSSPOINT_MESH
  if (carouselUi) {
    updateMeshBanner();
    if (swipe == MappedInputManager::SwipeDir::Left || swipe == MappedInputManager::SwipeDir::Right) {
      stepCarousel(swipe == MappedInputManager::SwipeDir::Left ? 1 : -1);
      return;
    }
  }
#endif
  if (swipe == MappedInputManager::SwipeDir::Up) {
    selectorIndex = ButtonNavigator::nextIndex(selectorIndex, menuCount);
#if CROSSPOINT_MESH
    if (carouselUi) syncCarouselCenter();
#endif
    requestUpdate();
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Down) {
    selectorIndex = ButtonNavigator::previousIndex(selectorIndex, menuCount);
#if CROSSPOINT_MESH
    if (carouselUi) syncCarouselCenter();
#endif
    requestUpdate();
    return;
  }

  // Back is otherwise unused on the home menu: open the most recently read
  // book directly (recentBooks is most-recent-first and already pruned of
  // files missing from the SD card).
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && hasContinueReading && !recentBooks.empty()) {
    onSelectBook(recentBooks[0].path);
    return;
  }

  bool coverHome = coverGridUi != nullptr;
#if CROSSPOINT_MESH
  coverHome = coverHome || carouselUi != nullptr;
  if (carouselUi) {
    const int touched = carouselUi->selectedAction(mappedInput);
    if (touched == CarouselHomeUi::BANNER_VALUE) {
      leavingToMesh = activityManager.goToMesh();
      return;
    }
    const int bookCount = static_cast<int>(recentBooks.size());
    if (touched >= 0 && touched < bookCount && touched != carouselCenter) {
      // A neighbor comes to the middle; a tap on the middle book opens it.
      selectorIndex = touched;
      syncCarouselCenter();
      requestUpdate();
      return;
    }
    if (touched >= 0 && touched < menuCount) {
      selectorIndex = touched;
      activateSelection();
      return;
    }
  }
#endif
  if (coverHome) {
    if (coverGridUi) {
      const int touched = coverGridUi->selectedAction(mappedInput);
      if (touched >= 0 && touched < menuCount) {
        selectorIndex = touched;
        activateSelection();
        return;
      }
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelection();
      return;
    }
    // Side page buttons walk the covers, front Left/Right walk the tabs
    // (selectorIndex is flat: books first, then the tab items). A press while
    // selection sits in the other band jumps into this band first.
    const int bookCount = static_cast<int>(recentBooks.size());
    const auto cycleBand = [this](const int base, const int count, const int dir) {
      if (count <= 0) return;
      int idx = selectorIndex - base;
      if (idx < 0 || idx >= count) {
        idx = dir > 0 ? 0 : count - 1;
      } else {
        idx = (idx + count + dir) % count;
      }
      selectorIndex = base + idx;
      requestUpdate();
    };
    // From the tabs, the carousel's first press selects the book in the middle.
    const auto enterCarousel = [this, bookCount] {
#if CROSSPOINT_MESH
      if (carouselUi && selectorIndex >= bookCount && bookCount > 0) {
        selectorIndex = carouselCenter;
        requestUpdate();
        return true;
      }
#endif
      return false;
    };
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Up}, [&cycleBand, &enterCarousel, bookCount] {
      if (!enterCarousel()) cycleBand(0, bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Down}, [&cycleBand, &enterCarousel, bookCount] {
      if (!enterCarousel()) cycleBand(0, bookCount, +1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Left}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, -1);
    });
    buttonNavigator.onPressAndContinuous({MappedInputManager::Button::Right}, [this, &cycleBand] {
      const int bookCount = static_cast<int>(recentBooks.size());
      cycleBand(bookCount, getMenuItemCount() - bookCount, +1);
    });
#if CROSSPOINT_MESH
    if (carouselUi) syncCarouselCenter();
#endif
    return;
  }

  // Match the rendered cover grid, which lays tiles inside getContentArea()
  // (origin content.x + contentSidePadding, span content.width). Using
  // screen-relative getScreenWidth()/contentSidePadding here offset the touch
  // targets from the drawn covers by content.x on bezel-inset panels (EEGO A4).
  const Rect content = UITheme::getInstance().getContentArea(renderer);
  const int coverColumnCount = std::max(1, metrics.homeRecentBooksCount);
  const int recentCount = std::min(static_cast<int>(recentBooks.size()), coverColumnCount);
  const int coverColumnWidth = (content.width - 2 * metrics.contentSidePadding) / coverColumnCount;
  int touchedBook = -1;
  const auto coverTouch = mappedInput.colTouch(touchedBook, content.x + metrics.contentSidePadding, coverColumnWidth,
                                               recentCount, metrics.homeTopPadding,
                                               metrics.homeTopPadding + metrics.homeCoverTileHeight, coverColumnWidth);
  if (coverTouch != MappedInputManager::RowTouch::None) {
    if (coverTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedBook) {
        selectorIndex = touchedBook;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedBook;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  const int menuTop = metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset;
  const int renderedMenuCount =
      menuCount - (metrics.homeContinueReadingInMenu ? 0 : static_cast<int>(recentBooks.size()));
  int menuRow = -1;
  // Row height from the theme, not the metrics table: RoundedRaff draws
  // font-derived rows and the touch grid must match the visuals exactly.
  const int menuRowHeight = GUI.getMenuRowHeight(renderer);
  const auto menuTouch = mappedInput.rowTouch(menuRow, menuTop, menuRowHeight + metrics.menuSpacing, renderedMenuCount,
                                              0, INT32_MAX, menuRowHeight);
  if (menuTouch != MappedInputManager::RowTouch::None) {
    const int touchedIndex =
        metrics.homeContinueReadingInMenu ? menuRow : menuRow + static_cast<int>(recentBooks.size());
    if (menuTouch == MappedInputManager::RowTouch::Down) {
      if (selectorIndex != touchedIndex) {
        selectorIndex = touchedIndex;
        requestUpdate();
      }
    } else {
      selectorIndex = touchedIndex;
      haptic_feedback::touchAction();
      activateSelection();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelection();
  }
}

void HomeActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // Content columns clear the bezel insets on rounded panels. The header
  // self-insets in drawHeader, so only the cover/menu need it here.
  const Rect content = UITheme::getInstance().getContentArea(renderer);

#if CROSSPOINT_MESH
  if (carouselUi) {
    // Missing thumbnails for the books about to show are made first, under
    // a loading popup over the last frame, so a swipe never flashes the
    // placeholder art.
    // One read: the loop may move it while this frame is drawn.
    const int center = carouselCenter.load();
    if (carouselUi->centerThumbHeight() > 0 && ensureCarouselThumbs(center)) carouselUi->invalidateCovers();
    renderer.clearScreen();
    carouselUi->setSelection(selectorIndex, center);
    carouselUi->renderUi();
    // The side buttons walk the books, front Left/Right the tabs.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    firstRenderDone = true;
    // The first layout sets the thumbnail sizes; any change means new files.
    if (carouselUi->takeThumbHeightsChanged()) {
      centerThumbReady = sideThumbReady = centerThumbFailed = sideThumbFailed = 0;
      requestUpdate();
      return;
    }
    coversSettled = true;
    return;
  }
#endif
  renderer.clearScreen();
  if (coverGridUi) {
    coverGridUi->setSelection(selectorIndex);
    UITheme::getInstance().drawCoverGridHome(*coverGridUi);
    // Front Left/Right walk the tabs, so their hints read Left/Right; the
    // side page buttons (unhinted) walk the covers.
    const auto labels = mappedInput.mapLabels(hasContinueReading ? tr(STR_RESUME) : "", tr(STR_SELECT),
                                              tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH
                                                                   : HalDisplay::FAST_REFRESH);
    // Slot heights are recorded during the draw above; a change (first layout
    // pass, orientation switch) means the paths must point at those sizes and
    // any missing thumbs must be generated. Refreshing the paths right away
    // lets the next pass draw already-cached thumbs before generation runs.
    const bool coverSpecChanged = coverGridUi->takeThumbHeightChanged();
    if (coverSpecChanged) {
      coverGridUi->refreshCoverPaths();
      recentsLoaded = false;
    }
    if (!firstRenderDone) {
      firstRenderDone = true;
      requestUpdate();
    } else if (!recentsLoaded && !recentsLoading) {
      loadRecentCovers(CoverGridHomeUi::THUMB_HEIGHT);
      coverGridUi->refreshCoverPaths();
      requestUpdate();
    }
    return;
  }
  bool bufferRestored = coverBufferStored && restoreCoverBuffer();

  // Band spans topPadding..homeTopPadding: the cover tile starts at the fixed
  // homeTopPadding, so the height must shrink by topPadding or the band (and a
  // centered title, e.g. RoundedRaff's book title) sinks into the tile.
  // Home is the stack root: no back button in its header.
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.homeTopPadding - metrics.topPadding},
                 metrics.homeContinueReadingInMenu && !recentBooks.empty() ? recentBooks[0].title.c_str() : nullptr,
                 nullptr, false);

  // Record the tile rect so storeCoverBuffer (called from the theme) knows
  // which sub-region of the framebuffer to snapshot. ~16 KB in Portrait
  // instead of the 48 KB full framebuffer the previous bind captured.
  coverRectX = content.x;
  coverRectY = metrics.homeTopPadding;
  coverRectW = content.width;
  coverRectH = metrics.homeCoverTileHeight;

  GUI.drawRecentBookCover(renderer, Rect{content.x, metrics.homeTopPadding, content.width, metrics.homeCoverTileHeight},
                          recentBooks, selectorIndex, coverRendered, coverBufferStored, bufferRestored,
                          std::bind(&HomeActivity::storeCoverBuffer, this));

  // Build menu items dynamically
  std::vector<const char*> menuItems = {tr(STR_BROWSE_FILES), tr(STR_LIBRARY), tr(STR_FILE_TRANSFER),
                                        tr(STR_SETTINGS_TITLE)};
  std::vector<UIIcon> menuIcons = {Folder, Library, Transfer, Settings};

  if (hasLibrarySlot()) {
    menuItems.insert(menuItems.begin() + 2, hasPlugins ? tr(STR_PLUGINS) : tr(STR_OPDS_BROWSER));
    menuIcons.insert(menuIcons.begin() + 2, Plugins);
  }

#if CROSSPOINT_MESH
  {
    // Mesh sits before File Transfer, matching indexToMenuItem().
    const int meshPos = hasLibrarySlot() ? 3 : 2;
    menuItems.insert(menuItems.begin() + meshPos, tr(STR_MESH));
    menuIcons.insert(menuIcons.begin() + meshPos, Hotspot);
  }
#endif

  if (metrics.homeContinueReadingInMenu && !recentBooks.empty()) {
    // Insert Continue Reading at the top if enabled in theme
    menuItems.insert(menuItems.begin(), tr(STR_CONTINUE_READING));
    menuIcons.insert(menuIcons.begin(), Book);
  }

  GUI.drawButtonMenu(
      renderer,
      Rect{content.x, metrics.homeTopPadding + metrics.homeCoverTileHeight + metrics.homeMenuTopOffset, content.width,
           pageHeight - (metrics.headerHeight + metrics.homeTopPadding + metrics.verticalSpacing +
                         metrics.homeMenuTopOffset + metrics.buttonHintsHeight)},
      static_cast<int>(menuItems.size()),
      metrics.homeContinueReadingInMenu ? selectorIndex : selectorIndex - recentBooks.size(),
      [&menuItems](int index) { return std::string(menuItems[index]); },
      [&menuIcons](int index) { return menuIcons[index]; });

  const auto labels = mappedInput.mapLabels(recentBooks.empty() ? "" : tr(STR_RESUME), tr(STR_SELECT), tr(STR_DIR_UP),
                                            tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(cleanInitialRefresh && !firstRenderDone ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);

  if (!firstRenderDone) {
    firstRenderDone = true;
    requestUpdate();
  } else if (!recentsLoaded && !recentsLoading) {
    recentsLoading = true;
    const int themeThumbHeight = GUI.homeCoverThumbHeight(renderer);
    loadRecentCovers(themeThumbHeight > 0 ? themeThumbHeight : metrics.homeCoverHeight);
  }
}

#if CROSSPOINT_MESH
namespace {
// Below this a cover would be a smudge; the slot keeps its placeholder.
constexpr int MIN_THUMB_HEIGHT = 16;
}  // namespace

bool HomeActivity::preventAutoSleep() {
  if (!meshHeld) return false;
  using Phase = MeshService::Phase;
  const Phase phase = MESH_SERVICE.phase();
  return phase == Phase::Connecting || phase == Phase::Handshake;
}

void HomeActivity::syncCarouselCenter() {
  if (selectorIndex >= 0 && selectorIndex < static_cast<int>(recentBooks.size())) carouselCenter = selectorIndex;
}

void HomeActivity::stepCarousel(const int direction) {
  const int count = static_cast<int>(recentBooks.size());
  if (count < 2) return;
  int next = carouselCenter + direction;
  if (count > 2) {
    next = (next + count) % count;
  } else if (next < 0 || next >= count) {
    return;
  }
  selectorIndex = next;
  carouselCenter = next;
  requestUpdate();
}

void HomeActivity::buildMeshBanner(MeshBanner& banner) const {
  using Phase = MeshService::Phase;
  using Problem = MeshService::Problem;
  banner = MeshBanner{};
  snprintf(banner.radioName, sizeof(banner.radioName), "%s", MESH_RADIO_STORE.radioName());
  if (!MESH_RADIO_STORE.hasRadio()) {
    banner.state = MeshBanner::State::NoRadio;
    banner.status = StrId::STR_MESH_BANNER_SETUP;
    banner.radioName[0] = 0;
    return;
  }
  // Unread counts last as long as the service runs, through a lost link.
  banner.unread = MESH_SERVICE.unreadTotal();
  MeshService::UnreadView latest[MeshBanner::MAX_ENTRIES];
  banner.entryCount = banner.unread > 0 ? MESH_SERVICE.recentUnread(latest, MeshBanner::MAX_ENTRIES) : 0;
  for (uint8_t i = 0; i < banner.entryCount; i++) {
    snprintf(banner.from[i], sizeof(banner.from[i]), "%s", latest[i].name);
    snprintf(banner.preview[i], sizeof(banner.preview[i]), "%s", latest[i].preview);
    // Cut on a whole code point.
    banner.preview[i][utf8SafeTruncateBuffer(banner.preview[i], static_cast<int>(strlen(banner.preview[i])))] = 0;
    banner.entryUnread[i] = latest[i].unread;
  }
  switch (MESH_SERVICE.phase()) {
    case Phase::Ready:
      banner.state = MeshBanner::State::Ready;
      return;
    case Phase::Scanning:
    case Phase::Connecting:
    case Phase::Handshake:
      banner.state = MeshBanner::State::Connecting;
      banner.status = StrId::STR_MESH_BANNER_CONNECTING;
      return;
    case Phase::NeedPin:
      banner.state = MeshBanner::State::Connecting;
      banner.status = StrId::STR_MESH_PIN_NEEDED;
      return;
    case Phase::Off:
    case Phase::Idle:
    case Phase::Stopping:
      break;
  }
  banner.state = MeshBanner::State::Off;
  switch (MESH_SERVICE.problem()) {
    case Problem::ConnectFailed:
      banner.status = StrId::STR_MESH_CONNECT_FAILED;
      break;
    case Problem::PairingFailed:
      banner.status = StrId::STR_MESH_PAIRING_FAILED;
      break;
    case Problem::NotCompanion:
      banner.status = StrId::STR_MESH_NOT_COMPANION;
      break;
    case Problem::LinkLost:
      banner.status = StrId::STR_MESH_LINK_LOST;
      break;
    case Problem::RadioSilent:
      banner.status = StrId::STR_MESH_RADIO_SILENT;
      break;
    case Problem::None:
      banner.status = StrId::STR_MESH_NOT_CONNECTED;
      break;
  }
  return;
}

void HomeActivity::holdMeshLink() {
  // Only for a radio that was paired before and not disconnected by hand:
  // Home never starts Bluetooth for nothing.
  if (meshHeld || !MESH_RADIO_STORE.hasRadio() || MESH_SERVICE.disconnectedByUser()) return;
  MESH_SERVICE.acquire();
  meshHeld = true;
}

void HomeActivity::updateMeshBanner() {
  if (!meshHeld && coversSettled) holdMeshLink();
  const uint32_t revision = MESH_SERVICE.revision();
  const auto phase = static_cast<uint8_t>(MESH_SERVICE.phase());
  if (revision != lastMeshRevision || phase != lastMeshPhase) {
    lastMeshRevision = revision;
    lastMeshPhase = phase;
    // A member, not a local: the banner is about 340 bytes.
    buildMeshBanner(nextBanner);
    if (nextBanner != shownBanner) {
      shownBanner = nextBanner;
      bannerPending = true;
    }
  }
  // A reconnect delivers queued messages in a burst; repaint at most this often.
  constexpr uint32_t BANNER_REPAINT_MS = 1500;
  const uint32_t nowMs = millis();
  if (!bannerPending || static_cast<int32_t>(nowMs - nextBannerRepaintMs) < 0) return;
  {
    // A frame that is making thumbnails holds the lock for seconds; the
    // main loop must keep polling the radio meanwhile.
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    carouselUi->setBanner(shownBanner);
  }
  bannerPending = false;
  nextBannerRepaintMs = nowMs + BANNER_REPAINT_MS;
  requestUpdate();
}

bool HomeActivity::generateThumb(RecentBook& book, const int height) {
  // One parser at a time; both exceed the stack budget.
  if (FsHelpers::hasReflowableBookExtension(book.path)) {
    auto epub = makeUniqueNoThrow<Epub>(book.path, "/.crosspoint");
    if (!epub) {
      LOG_ERR("HOME", "OOM: cover EPUB");
      return false;
    }
    return epub->generateThumbBmpFromSource(height);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    auto xtc = makeUniqueNoThrow<Xtc>(book.path, "/.crosspoint");
    if (!xtc) {
      LOG_ERR("HOME", "OOM: cover XTC");
      return false;
    }
    return xtc->load() && xtc->generateThumbBmp(height);
  }
  return false;
}

bool HomeActivity::ensureCarouselThumbs(int center) {
  const int count = static_cast<int>(recentBooks.size());
  if (count == 0) return false;
  if (center < 0 || center >= count) center = 0;
  // The middle book and its neighbors, as the carousel lays them out.
  int visible[3] = {center, -1, -1};
  if (count > 2) {
    visible[1] = (center + count - 1) % count;
    visible[2] = (center + 1) % count;
  } else if (count == 2) {
    visible[1] = 1 - center;
  }
  bool popupShown = false;
  bool wrote = false;
  for (int i = 0; i < 3; ++i) {
    const int book = visible[i];
    if (book < 0 || book >= CarouselHomeUi::MAX_BOOKS) continue;
    const bool isCenter = i == 0;
    uint16_t& ready = isCenter ? centerThumbReady : sideThumbReady;
    uint16_t& failed = isCenter ? centerThumbFailed : sideThumbFailed;
    const uint16_t bit = static_cast<uint16_t>(1u << book);
    if ((ready | failed) & bit) continue;
    RecentBook& entry = recentBooks[book];
    const int height = isCenter ? carouselUi->centerThumbHeight() : carouselUi->sideThumbHeight();
    // No cover to make (a format without one, or no room on screen).
    if (entry.coverBmpPath.empty() || height < MIN_THUMB_HEIGHT) {
      failed |= bit;
      continue;
    }
    const bool present = thumbUsable(UITheme::getCoverThumbPath(entry.coverBmpPath, height));
    if (!present) {
      if (!popupShown) {
        popupShown = true;
        GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
      }
      if (!generateThumb(entry, height)) {
        reportCoverFailure(entry.path, height);
        LOG_ERR("HOME", "No %d px cover for %s", height, entry.path.c_str());
        failed |= bit;
        continue;
      }
      wrote = true;
    }
    ready |= bit;
  }
  return wrote;
}
#endif

void HomeActivity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void HomeActivity::onFileBrowserOpen() { activityManager.goToFileBrowser(); }

void HomeActivity::onLibraryOpen() { activityManager.goToLibrary(); }

void HomeActivity::onSettingsOpen() { activityManager.goToSettings(); }

void HomeActivity::onFileTransferOpen() { activityManager.goToFileTransfer(); }

void HomeActivity::onOpdsBrowserOpen() { activityManager.goToBrowser(); }

void HomeActivity::onPluginsOpen() { activityManager.goToPlugins(hasOpdsServers); }
