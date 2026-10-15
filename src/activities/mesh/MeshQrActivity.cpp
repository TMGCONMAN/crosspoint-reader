#include "MeshQrActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

MeshQrActivity::MeshQrActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title,
                               const char* payload, const char* caption)
    : Activity("MeshQr", renderer, mappedInput) {
  snprintf(this->title, sizeof(this->title), "%s", title ? title : "");
  snprintf(this->payload, sizeof(this->payload), "%s", payload ? payload : "");
  snprintf(this->caption, sizeof(this->caption), "%s", caption ? caption : "");
}

void MeshQrActivity::loop() {
  int x = 0;
  int y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
}

void MeshQrActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect content = UITheme::getContentArea(renderer);
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, renderer.getScreenWidth(), metrics.headerHeight}, title,
                 nullptr);

  const int top = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 2;
  const int captionHeight = renderer.getLineHeight(UI_10_FONT_ID) * 3;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight - metrics.verticalSpacing;
  // Square, as large as fits; the scanner needs a quiet margin around it.
  const int side = std::max(0, std::min(content.width - 2 * metrics.contentSidePadding,
                                        bottom - top - captionHeight - metrics.verticalSpacing * 2));
  const Rect qr{content.x + (content.width - side) / 2, top, side, side};
  QrUtils::drawQrCode(renderer, qr, std::string(payload));
  UITheme::drawCenteredWrappedText(
      renderer,
      Rect{content.x + metrics.contentSidePadding, top + side + metrics.verticalSpacing * 2,
           content.width - 2 * metrics.contentSidePadding, captionHeight},
      UI_10_FONT_ID, caption, 3);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // A full refresh: a QR code with ghosting scans badly.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

#endif  // CROSSPOINT_MESH
