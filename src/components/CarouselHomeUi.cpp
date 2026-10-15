#include "CarouselHomeUi.h"

#if CROSSPOINT_MESH

#include <BoardConfig.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "MappedInputManager.h"
#include "UITheme.h"
#include "icons/book.h"
#include "icons/hotspot.h"
#include "util/BookProgress.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId SELECT = 1;
// Matches the Cover Grid, so the tabs sit on the same line.
constexpr int16_t COVER_CELL_INSET = 6;
// Side covers are this share of the center cover's width.
constexpr int SIDE_PERCENT = 45;
constexpr int16_t CAROUSEL_GAP = 10;
// Room inside each frame for the selection ring.
constexpr int16_t FRAME_INSET = 4;
constexpr int16_t PROGRESS_HEIGHT = 5;
constexpr int16_t DOT_SIZE = 8;
constexpr int16_t DOT_GAP = 6;
constexpr int16_t ICON_SIZE = 32;
constexpr uint8_t BANNER_RADIUS = 8;
// Generated a little taller than the slot, like the Cover Grid's thumbs:
// the generator fits a 0.6 aspect, and the clip hides the overshoot.
constexpr int THUMB_BLEED = 8;
}  // namespace

bool MeshBanner::operator==(const MeshBanner& other) const {
  if (state != other.state || status != other.status || unread != other.unread || entryCount != other.entryCount ||
      strcmp(radioName, other.radioName) != 0) {
    return false;
  }
  for (uint8_t i = 0; i < entryCount; i++) {
    if (entryUnread[i] != other.entryUnread[i] || strcmp(from[i], other.from[i]) != 0 ||
        strcmp(preview[i], other.preview[i]) != 0) {
      return false;
    }
  }
  return true;
}

CarouselHomeUi::CarouselHomeUi(GfxRenderer& renderer)
    : UiAppHost(renderer), coverCache(renderer), renderer(renderer), tabBar(renderer) {}

void CarouselHomeUi::begin(const std::vector<RecentBook>& recent, const bool librarySlot) {
  books = &recent;
  hasLibrarySlot = librarySlot;
  if (!recent.empty()) coverCache.begin();
  resetUi();
  app.on(SELECT, &CarouselHomeUi::onAction, this);
  app.setScreen(&CarouselHomeUi::screenFn, this);
}

void CarouselHomeUi::setSelection(const int selection, int center) {
  selected = selection;
  if (center < 0 || center >= static_cast<int>(books->size())) center = 0;
  if (center != centerIndex) {
    centerIndex = center;
    // The cache holds what each of the three places showed.
    coverCache.invalidate();
  }
}

bool CarouselHomeUi::takeThumbHeightsChanged() { return std::exchange(thumbsChanged, false); }

void CarouselHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<CarouselHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int CarouselHomeUi::selectedAction(const MappedInputManager& input) {
  pending = -1;
  const auto touch = routeTouch(input);
  return touch.snap.touchReleased ? pending : -1;
}

void CarouselHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<CarouselHomeUi*>(user)->draw(screen); }

void CarouselHomeUi::draw(UiScreen& screen) {
  coverCache.prepare();
  const auto& theme = screen.theme();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y), static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  const ThemeMetrics& metrics = UITheme::getInstance().getMetrics();
  const int16_t hInset =
      std::max<int16_t>(0, static_cast<int16_t>(metrics.headerSidePadding - COVER_CELL_INSET - safe.x));
  const int16_t topInset =
      BoardConfig::hasTouch() ? static_cast<int16_t>(theme.spaceLg) : static_cast<int16_t>(theme.spaceSm + 4);
  screen.insetContent(fui::Insets{topInset, hInset, theme.spaceSm, hInset});
  // The status band's slot; it draws at its fixed place below.
  screen.takeTop(metrics.batteryBarHeight, theme.spaceSm);
  const int16_t tabGap = BoardConfig::hasTouch() ? theme.spaceSm : static_cast<int16_t>(4);
  auto tabRect = screen.takeBottom(metrics.coverGridTabBarHeight, tabGap);
  tabRect = tabRect.inset(fui::Insets{0, COVER_CELL_INSET, 0, COVER_CELL_INSET});
  // A fixed height: the covers are sized from what is left, and a new size
  // would mean making every thumbnail again.
  const int16_t bannerHeight =
      static_cast<int16_t>(screen.target().lineHeight(theme.bodyText.font) +
                           screen.target().lineHeight(theme.smallText.font) + 2 * theme.spaceMd);
  const auto bannerRect =
      screen.takeBottom(std::max<int16_t>(bannerHeight, ICON_SIZE + 2 * theme.spaceMd), theme.spaceMd)
          .inset(fui::Insets{0, COVER_CELL_INSET, 0, COVER_CELL_INSET});

  if (books->empty()) {
    drawEmpty(screen);
  } else {
    drawCarousel(screen, screen.body());
  }
  drawBanner(screen, bannerRect);
  tabBar.draw(screen, tabRect, static_cast<int>(books->size()), selected, hasLibrarySlot, SELECT, /*appsSlot=*/true);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.batteryBarHeight}, nullptr);
}

void CarouselHomeUi::drawEmpty(UiScreen& screen) {
  const auto& theme = screen.theme();
  const auto body = screen.body();
  auto title = theme.titleText;
  title.bold = true;
  title.align = fui::TextAlign::Center;
  auto message = theme.bodyText;
  message.align = fui::TextAlign::Center;
  const int16_t titleHeight = screen.target().lineHeight(title.font);
  const int16_t messageHeight = screen.target().lineHeight(message.font);
  const int16_t contentHeight = ICON_SIZE + theme.spaceLg + titleHeight + theme.spaceSm + messageHeight;
  int16_t y = static_cast<int16_t>(body.y + std::max(0, (body.height - contentHeight) / 2));
  renderer.drawIcon(BookIcon, body.x + (body.width - ICON_SIZE) / 2, y, ICON_SIZE);
  y = static_cast<int16_t>(y + ICON_SIZE + theme.spaceLg);
  screen.target().text(fui::Rect{body.x, y, body.width, titleHeight}, tr(STR_NO_OPEN_BOOK), title);
  y = static_cast<int16_t>(y + titleHeight + theme.spaceSm);
  screen.target().text(fui::Rect{body.x, y, body.width, messageHeight}, tr(STR_START_READING), message);
}

void CarouselHomeUi::loadProgress() {
  if (progressBook == centerIndex) return;
  progressBook = centerIndex;
  progress = loadBookProgress((*books)[centerIndex].path);
  if (progress >= 0) snprintf(progressText, sizeof(progressText), "%d%%", progress);
}

void CarouselHomeUi::drawCarousel(UiScreen& screen, const fui::Rect body) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const int count = static_cast<int>(books->size());
  const RecentBook& book = (*books)[centerIndex];
  loadProgress();

  auto titleStyle = theme.bodyText;
  titleStyle.bold = true;
  titleStyle.align = fui::TextAlign::Center;
  titleStyle.maxLines = 2;
  auto infoStyle = theme.smallText;
  infoStyle.align = fui::TextAlign::Center;
  const int16_t titleHeight = static_cast<int16_t>(target.lineHeight(titleStyle.font) * 2);
  const int16_t infoLine = target.lineHeight(infoStyle.font);
  const int16_t below = static_cast<int16_t>(infoLine + theme.spaceSm + PROGRESS_HEIGHT + theme.spaceMd + DOT_SIZE);

  // The center cover takes what the height leaves, as wide as the row allows
  // with both neighbors beside it.
  const int coverRoom = body.height - titleHeight - below - 2 * theme.spaceMd;
  const int widthLimit = (body.width - 2 * CAROUSEL_GAP) * 100 / (100 + 2 * SIDE_PERCENT);
  const int16_t centerW = static_cast<int16_t>(std::max(1, std::min(coverRoom * 2 / 3, widthLimit)));
  const int16_t centerH = static_cast<int16_t>(centerW * 3 / 2);
  const int16_t sideW = static_cast<int16_t>(std::max(1, centerW * SIDE_PERCENT / 100));
  const int16_t sideH = static_cast<int16_t>(sideW * 3 / 2);

  const int16_t used = static_cast<int16_t>(titleHeight + theme.spaceMd + centerH + theme.spaceMd + below);
  int16_t y = static_cast<int16_t>(body.y + std::max(0, (body.height - used) / 2));
  target.text(fui::Rect{body.x, y, body.width, titleHeight}, book.title.c_str(), titleStyle);
  y = static_cast<int16_t>(y + titleHeight + theme.spaceMd);

  const bool onBooks = selected >= 0 && selected < count;
  carousel.count = static_cast<uint16_t>(count);
  carousel.selectedIndex = static_cast<int16_t>(centerIndex);
  carousel.action = SELECT;
  carousel.inputMask = fui::InputTouch;
  carousel.centerSize = fui::Size{centerW, centerH};
  carousel.sideSize = fui::Size{sideW, sideH};
  carousel.gap = CAROUSEL_GAP;
  carousel.contentInset = FRAME_INSET;
  // With two books, wrapping would show the other book on both sides.
  carousel.wrap = count > 2;
  carousel.frameStyles = fui::StyleSet{};
  carousel.frameStyles.normal.background = fui::Paint::solid(fui::Color::White);
  carousel.frameStyles.selected.background = fui::Paint::solid(fui::Color::White);
  carousel.frameStyles.selected.border = fui::Paint::solid(fui::Color::Black);
  // Heavier while the buttons walk the books.
  carousel.frameStyles.selected.borderWidth = onBooks ? 3 : 1;
  carousel.frameStyles.selected.radius = 6;
  fui::coverCarousel(screen.frame(), fui::Rect{body.x, y, body.width, centerH}, carousel, slots);

  const int newCenterThumb = slots[1].content.height + THUMB_BLEED;
  const int newSideThumb = sideH - 2 * FRAME_INSET + THUMB_BLEED;
  if (newCenterThumb != centerThumb || newSideThumb != sideThumb) {
    centerThumb = newCenterThumb;
    sideThumb = newSideThumb;
    thumbsChanged = true;
    coverCache.invalidate();
  }
  for (int i = 0; i < 3; ++i) {
    if (!slots[i].valid) continue;
    paintCover(slots[i].content, i, slots[i].itemIndex, slots[i].isCenter ? centerThumb : sideThumb);
  }
  y = static_cast<int16_t>(y + centerH + theme.spaceMd);

  // Author and progress of the center book.
  char info[96];
  if (!book.author.empty() && progress >= 0) {
    snprintf(info, sizeof(info), "%s  |  %s", book.author.c_str(), progressText);
  } else {
    snprintf(info, sizeof(info), "%s",
             !book.author.empty() ? book.author.c_str() : (progress >= 0 ? progressText : ""));
  }
  target.text(fui::Rect{body.x, y, body.width, infoLine}, info, infoStyle);
  y = static_cast<int16_t>(y + infoLine + theme.spaceSm);
  const fui::Rect bar{static_cast<int16_t>(body.x + (body.width - centerW) / 2), y, centerW, PROGRESS_HEIGHT};
  if (progress >= 0) {
    target.fill(bar, fui::Paint::dither(fui::Color::LightGray));
    const int16_t filled = static_cast<int16_t>(bar.width * std::min(progress, 100) / 100);
    if (filled > 0) target.fill(fui::Rect{bar.x, bar.y, filled, bar.height}, fui::Paint::solid(fui::Color::Black));
  }
  y = static_cast<int16_t>(y + PROGRESS_HEIGHT + theme.spaceMd);

  // One dot per book; the center book's is filled.
  if (count > 1) {
    const int dotsWidth = count * DOT_SIZE + (count - 1) * DOT_GAP;
    int16_t x = static_cast<int16_t>(body.x + (body.width - dotsWidth) / 2);
    for (int i = 0; i < count; ++i) {
      const fui::Rect dot{x, y, DOT_SIZE, DOT_SIZE};
      if (i == centerIndex) {
        target.fill(dot, fui::Paint::solid(fui::Color::Black), DOT_SIZE / 2);
      } else {
        target.stroke(dot, fui::Paint::solid(fui::Color::Black), 1, DOT_SIZE / 2);
      }
      x = static_cast<int16_t>(x + DOT_SIZE + DOT_GAP);
    }
  }
}

void CarouselHomeUi::paintCover(const fui::Rect rect, const int slot, const int book, const int thumbHeight) {
  const RecentBook& entry = (*books)[book];
  const std::string path =
      entry.coverBmpPath.empty() ? std::string() : UITheme::getCoverThumbPath(entry.coverBmpPath, thumbHeight);
  coverCache.paint(rect, static_cast<size_t>(slot), path, entry.title.c_str());
}

void CarouselHomeUi::drawBanner(UiScreen& screen, const fui::Rect rect) {
  const auto& theme = screen.theme();
  auto& target = screen.target();
  const bool alert = banner.unread > 0;
  screen.frame().hit(rect, SELECT, BANNER_VALUE, fui::InputTouch);
  if (alert) {
    target.fill(rect, fui::Paint::solid(fui::Color::Black), BANNER_RADIUS);
  } else {
    target.fill(rect, fui::Paint::solid(fui::Color::White), BANNER_RADIUS);
    target.stroke(rect, fui::Paint::solid(fui::Color::Black), 2, BANNER_RADIUS);
  }

  // The icon sits on a paper square, so it shows on the dark banner too.
  const int16_t pad = theme.spaceMd;
  const fui::Rect iconBox{static_cast<int16_t>(rect.x + pad),
                          static_cast<int16_t>(rect.y + (rect.height - ICON_SIZE) / 2), ICON_SIZE, ICON_SIZE};
  if (alert) target.fill(iconBox.inset(fui::Insets{-2, -2, -2, -2}), fui::Paint::solid(fui::Color::White), 4);
  renderer.drawIcon(HotspotIcon, iconBox.x, iconBox.y, ICON_SIZE);

  int16_t textRight = static_cast<int16_t>(rect.right() - pad);
  auto countStyle = theme.smallText;
  countStyle.bold = true;
  countStyle.align = fui::TextAlign::Center;
  if (alert) {
    // The count as a paper pill at the right.
    snprintf(bannerCount, sizeof(bannerCount), "%u", static_cast<unsigned>(banner.unread));
    const int16_t lineH = target.lineHeight(countStyle.font);
    const int16_t pillW = static_cast<int16_t>(
        std::max<int>(lineH + 8, target.measureText(countStyle.font, bannerCount, countStyle).width + 16));
    const fui::Rect pill{static_cast<int16_t>(textRight - pillW),
                         static_cast<int16_t>(rect.y + (rect.height - lineH - 4) / 2), pillW,
                         static_cast<int16_t>(lineH + 4)};
    target.fill(pill, fui::Paint::solid(fui::Color::White), static_cast<uint8_t>(pill.height / 2));
    countStyle.color = fui::Color::Black;
    target.text(fui::Rect{pill.x, static_cast<int16_t>(pill.y + 2), pill.width, lineH}, bannerCount, countStyle);
    textRight = static_cast<int16_t>(pill.x - pad);
  }

  const char* title = banner.radioName[0] != 0 ? banner.radioName : tr(STR_MESH);
  const char* detail = I18n::getInstance().get(banner.status);
  if (alert && banner.entryCount > 1) {
    // Every waiting conversation by name, newest first, then the newest message.
    size_t used = 0;
    for (uint8_t i = 0; i < banner.entryCount && used + 1 < sizeof(bannerTitle); i++) {
      const int n = snprintf(bannerTitle + used, sizeof(bannerTitle) - used, "%s%s", i > 0 ? ", " : "", banner.from[i]);
      if (n > 0) used += static_cast<size_t>(n);
    }
    title = bannerTitle;
    snprintf(bannerLine, sizeof(bannerLine), "%s: %s", banner.from[0], banner.preview[0]);
    detail = bannerLine;
  } else if (alert) {
    if (banner.entryCount == 1 && banner.from[0][0] != 0) title = banner.from[0];
    detail = banner.entryCount == 1 && banner.preview[0][0] != 0 ? banner.preview[0] : tr(STR_MESH_BANNER_NEW);
  } else if (banner.state == MeshBanner::State::Ready) {
    detail = tr(STR_MESH_BANNER_NO_NEW);
  }

  auto titleStyle = theme.bodyText;
  titleStyle.bold = true;
  titleStyle.color = alert ? fui::Color::White : fui::Color::Black;
  auto detailStyle = theme.smallText;
  detailStyle.color = titleStyle.color;
  const int16_t titleH = target.lineHeight(titleStyle.font);
  const int16_t detailH = target.lineHeight(detailStyle.font);
  const int16_t textX = static_cast<int16_t>(iconBox.right() + pad);
  const int16_t textW = static_cast<int16_t>(std::max(0, textRight - textX));
  const int16_t textY = static_cast<int16_t>(rect.y + (rect.height - titleH - detailH) / 2);
  target.text(fui::Rect{textX, textY, textW, titleH}, title, titleStyle);
  target.text(fui::Rect{textX, static_cast<int16_t>(textY + titleH), textW, detailH}, detail, detailStyle);
}

#endif  // CROSSPOINT_MESH
