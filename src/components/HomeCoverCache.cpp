#include "HomeCoverCache.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>

#include "UITheme.h"
#include "fontIds.h"
#include "util/CoverReport.h"

namespace fui = freeink::ui;

void HomeCoverCache::begin() {
  // Cover regions do not overlap. Each may widen by one physical byte per row.
  coverCacheCapacity = renderer.getRegionByteSize(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()) +
                       MAX_COVERS * std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  coverCache = HalMemory::allocatePsram(coverCacheCapacity);
  if (!coverCache) {
    LOG_ERR("HOME", "PSRAM cover cache unavailable (%u bytes); rendering uncached", unsigned(coverCacheCapacity));
    coverCacheCapacity = 0;
  }
}

void HomeCoverCache::invalidate() {
  coverCacheUsed = 0;
  cachedCovers.fill(CachedCover{});
}

void HomeCoverCache::invalidate(size_t index) {
  if (index < cachedCovers.size()) cachedCovers[index].valid = false;
}

void HomeCoverCache::prepare() {
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (coverCacheOrientation != orientation) {
    invalidate();
    coverCacheOrientation = orientation;
  }
}

bool HomeCoverCache::paint(fui::Rect rect, size_t index, const std::string& path, const char* title) {
  if (index >= cachedCovers.size()) return false;
  auto& cached = cachedCovers[index];
  uint8_t* const cacheData = coverCache.get();
  if (coverCache && cached.valid && cached.rect.x == rect.x && cached.rect.y == rect.y &&
      cached.rect.width == rect.width && cached.rect.height == rect.height &&
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, cacheData + cached.offset, cached.bytes)) {
    return true;
  }
  cached.valid = false;
  bool drawn = false;
  if (!path.empty() && Storage.openFileForRead("HOME", path, coverFile)) {
    const BmpReaderError parsed = coverBitmap.parseHeaders();
    if (parsed != BmpReaderError::Ok) {
      LOG_ERR("HOME", "Thumbnail %s unreadable: %s", path.c_str(), Bitmap::errorToString(parsed));
    }
    if (parsed == BmpReaderError::Ok && coverBitmap.getWidth() > 0 && coverBitmap.getHeight() > 0) {
      // The art nudges a few px right of center; the spine below hugs its
      // left edge either way.
      constexpr int ART_SHIFT = 3;
      drawn = GUI.drawCoverThumbFill(renderer, coverBitmap, Rect{rect.x, rect.y, rect.width, rect.height}, ART_SHIFT);
      if (drawn) {
        renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
        // False spine glued to the art's left edge (centered art means that
        // edge moves with each cover's fit margin): a dark band with a
        // dithered crease makes every cover read as a bound book. Drawn here
        // so the PSRAM snapshot below captures it.
        const int artLeft = std::max<int>(rect.x, rect.x + (rect.width - coverBitmap.getWidth()) / 2 + ART_SHIFT);
        renderer.fillRect(artLeft, rect.y, 3, rect.height);
        renderer.fillRectDither(artLeft + 3, rect.y, 2, rect.height, Color::LightGray);
      } else {
        LOG_ERR("HOME", "Thumbnail %s (%dx%d, %u bpp) not drawn", path.c_str(), coverBitmap.getWidth(),
                coverBitmap.getHeight(), static_cast<unsigned>(coverBitmap.getBpp()));
      }
    }
    coverFile.close();
    if (!drawn) reportCoverFailure(path, rect.height, "Thumbnail on the card but not drawn");
  }
  if (!drawn) {
    GUI.drawCoverPlaceholder(renderer, Rect{rect.x, rect.y, rect.width, rect.height});
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height);
    // A book without cover art still says which book it is: its title in
    // paper on the placeholder's dark lower part.
    if (title && title[0] != 0 && rect.width > 40) {
      const int pad = std::max(4, rect.width / 12);
      const int top = rect.y + rect.height / 3 + rect.height / 8;
      const int fontId = rect.width >= 160 ? UI_12_FONT_ID : UI_10_FONT_ID;
      const int maxLines =
          std::max(1, (rect.y + rect.height - pad - top) / std::max(1, renderer.getLineHeight(fontId)));
      UITheme::drawCenteredWrappedText(renderer,
                                       Rect{rect.x + pad, top, rect.width - 2 * pad, rect.y + rect.height - pad - top},
                                       fontId, title, std::min(maxLines, 5), /*black=*/false, EpdFontFamily::BOLD);
    }
  }
  if (coverCache) {
    const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
    if (needed > cached.bytes && needed <= coverCacheCapacity - coverCacheUsed) {
      cached.offset = coverCacheUsed;
      cached.bytes = needed;
      coverCacheUsed += needed;
    }
    if (needed > 0 && needed <= cached.bytes) {
      cached.rect = rect;
      cached.valid =
          renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height, cacheData + cached.offset, cached.bytes);
    }
  }
  return true;  // The cover slot is painted, including fallback art.
}
