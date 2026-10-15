#include "MeshHistoryActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "mesh/MeshService.h"

namespace fui = freeink::ui;

MeshHistoryActivity::MeshHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* threadKey,
                                         const char* title, const bool direct, const uint32_t endOffset)
    : UiListActivity("MeshHistory", renderer, mappedInput), direct(direct), firstEndOffset(endOffset) {
  snprintf(this->threadKey, sizeof(this->threadKey), "%s", threadKey);
  snprintf(this->title, sizeof(this->title), "%s", title);
}

void MeshHistoryActivity::onEnter() {
  {
    // A render requested by the screen below may still be running.
    RenderLock lock(*this);
    messageCount = 0;
    // One block for as long as the screen is open, about 9 KB.
    messages = makeUniqueNoThrow<meshc::StoredMessage[]>(PAGE_SIZE);
  }
  UiListActivity::onEnter();
  if (!messages) {
    LOG_ERR("MHISTV", "OOM: message list");
    finish();
    return;
  }
  nowEpoch = MESH_SERVICE.now();
  showPage(firstEndOffset);
}

void MeshHistoryActivity::onExit() {
  // ActivityManager holds the render lock around onExit().
  messageCount = 0;
  messages.reset();
  UiListActivity::onExit();
}

void MeshHistoryActivity::showPage(const uint32_t endOffset) {
  {
    RenderLock lock(*this);
    pageDrawn = false;
    messageCount = meshhistory::loadPage(threadKey, endOffset, messages.get(), PAGE_SIZE, olderEnd);
  }
  // Start at the newest message of the page; reading goes upwards from there.
  nav.requestSelection(static_cast<int>(messageCount) + FIRST_MESSAGE_ROW - 1);
  requestUpdate();
}

// Touch only. A tap on rows the panel does not show yet must not move the
// selection either.
void MeshHistoryActivity::onRowAction(const fui::ActionEvent& event) {
  if (!pageDrawn) {
    app.clearTapFlash();
    return;
  }
  UiListActivity::onRowAction(event);
}

void MeshHistoryActivity::activateIndex(const int index) {
  // The page changes under the tap; a lingering flash would gray a new row.
  app.clearTapFlash();
  if (index == 0 && pageDrawn && meshhistory::hasOlderPage(olderEnd)) showPage(olderEnd);
}

// fui::ListProps::rowProvider. Runs on the render task, which holds the
// render lock, so the page cannot change under it.
void MeshHistoryActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<MeshHistoryActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  const size_t message = static_cast<size_t>(index) - FIRST_MESSAGE_ROW;
  if (index < FIRST_MESSAGE_ROW || !self->messages || message >= self->messageCount) {
    item.label =
        meshhistory::hasOlderPage(self->olderEnd) ? tr(STR_MESH_EARLIER_MESSAGES) : tr(STR_MESH_NO_EARLIER_MESSAGES);
    return;
  }
  meshrow::formatHeader(self->messages[message], self->direct ? self->title : nullptr, self->nowEpoch, self->rowHeader,
                        sizeof(self->rowHeader));
  item.label = self->rowHeader;
  item.subtitle = self->messages[message].text;
}

void MeshHistoryActivity::buildScreen(UiScreen& screen) {
  meshrow::setListMargins(screen, renderer);

  fui::ListProps props;
  props.rowProvider = &MeshHistoryActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  meshrow::setMessageRowStyle(screen, props);
  syncListViewport(screen, props);
  screen.list(props);
}

// The render task holds the render lock until this returns, so a page load
// cannot slip in between the panel refresh and the flag.
void MeshHistoryActivity::render(RenderLock&& lock) {
  UiListActivity::render(std::move(lock));
  pageDrawn = true;
}

#endif  // CROSSPOINT_MESH
