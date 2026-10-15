#include "MeshChatActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "MeshHistoryActivity.h"
#include "activities/util/KeyboardEntryActivity.h"

namespace fui = freeink::ui;

namespace {

// A reconnect delivers the radio's queued messages in a burst, and each
// e-ink repaint takes about a second.
constexpr uint32_t REPAINT_INTERVAL_MS = 1500;

bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

// The radio did not take it, or nobody acknowledged it: worth sending again.
bool canRetry(const meshc::StoredMessage& message) {
  return message.outgoing &&
         (message.delivery == meshc::Delivery::Failed || message.delivery == meshc::Delivery::Unconfirmed);
}

}  // namespace

MeshChatActivity::MeshChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* threadKey,
                                   const char* title)
    : UiListActivity("MeshChat", renderer, mappedInput, /*wantsTouchLongPress=*/true) {
  snprintf(this->threadKey, sizeof(this->threadKey), "%s", threadKey);
  snprintf(this->title, sizeof(this->title), "%s", title);
  uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
  direct = meshhistory::parseContactThreadKey(threadKey, keyPrefix);
}

MeshChatActivity::~MeshChatActivity() { MESH_SERVICE.closeThread(*this); }

void MeshChatActivity::onEnter() {
  {
    // A render requested by the screen below may still be running.
    RenderLock lock(*this);
    messageCount = 0;
    olderEnd = 0;
    // One block for as long as the screen is open, about 9 KB.
    messages = makeUniqueNoThrow<meshc::StoredMessage[]>(CAPACITY);
    if (messages) messageCount = meshhistory::loadPage(threadKey, 0, messages.get(), CAPACITY, olderEnd);
    memset(tags, 0, sizeof(tags));
    droppedCount = 0;
    shownCount = static_cast<uint32_t>(messageCount);
    shownDropped = 0;
  }
  UiListActivity::onEnter();
  if (!messages) {
    LOG_ERR("MCHAT", "OOM: message list");
    finish();
    return;
  }

  MESH_SERVICE.openThread(threadKey, *this);
  linkReady = MESH_SERVICE.phase() == MeshService::Phase::Ready;
  nowEpoch = MESH_SERVICE.now();
  repaintPending = false;
  nextRepaintMs = millis() + REPAINT_INTERVAL_MS;
  // Start on the last row, with the newest messages above it.
  nav.requestSelection(composeRow());
}

void MeshChatActivity::onExit() {
  MESH_SERVICE.closeThread(*this);
  // ActivityManager holds the render lock around onExit().
  messageCount = 0;
  messages.reset();
  UiListActivity::onExit();
}

void MeshChatActivity::loop() {
  if (pendingQuickReply >= 0) {
    const int choice = pendingQuickReply;
    pendingQuickReply = -1;
    if (choice < static_cast<int>(quickReplyCount) && linkReady) {
      const char* text = quickReplies[choice];
      if (!MESH_SERVICE.sendText(threadKey, text, strlen(text))) LOG_ERR("MCHAT", "Quick reply refused");
    }
  }
  nowEpoch = MESH_SERVICE.now();
  const bool ready = MESH_SERVICE.phase() == MeshService::Phase::Ready;
  if (ready != linkReady) {
    linkReady = ready;
    requestRepaint(true);
  }

  const uint32_t nowMs = millis();
  if (repaintPending && reached(nowMs, nextRepaintMs)) {
    repaintPending = false;
    nextRepaintMs = nowMs + REPAINT_INTERVAL_MS;
    requestUpdate();
  }
  UiListActivity::loop();
}

// Never repaints directly: this also runs while another screen is on top,
// and requestUpdate() would redraw that screen. loop() picks the request up.
void MeshChatActivity::requestRepaint(const bool urgent) {
  repaintPending = true;
  if (urgent) nextRepaintMs = millis();
}

void MeshChatActivity::requestRepaintSoon() {
  // The radio takes a message within a few hundred ms: wait for that, so a
  // send costs one e-ink refresh, not one for "Sending" and one for "Sent".
  constexpr uint32_t SEND_SETTLE_MS = 400;
  const uint32_t at = millis() + SEND_SETTLE_MS;
  if (!repaintPending || static_cast<int32_t>(at - nextRepaintMs) < 0) nextRepaintMs = at;
  repaintPending = true;
}

void MeshChatActivity::resumeFromChild() {
  linkReady = MESH_SERVICE.phase() == MeshService::Phase::Ready;
  nowEpoch = MESH_SERVICE.now();
  repaintPending = false;
  nextRepaintMs = millis() + REPAINT_INTERVAL_MS;
}

void MeshChatActivity::appendMessage(const meshc::StoredMessage& message, const uint32_t tag) {
  if (!messages) return;
  if (messageCount == CAPACITY) {
    // Drop the oldest, and move the selection and the view with the rows.
    memmove(&messages[0], &messages[1], (CAPACITY - 1) * sizeof(meshc::StoredMessage));
    memmove(&tags[0], &tags[1], (CAPACITY - 1) * sizeof(tags[0]));
    messageCount--;
    droppedCount++;
    if (nav.selected > FIRST_MESSAGE_ROW) nav.selected--;
    if (nav.top > FIRST_MESSAGE_ROW) nav.top--;
  }

  // Swipes move the view and leave the selection, so "at the end" needs both
  // the selection on the last row and that row on screen.
  const int lastRow = composeRow();
  const bool selectionOnLastRow = nav.selected >= lastRow;
  const bool lastRowShown = nav.drawnRows == 0 || nav.top + nav.drawnRows > lastRow;

  messages[messageCount] = message;
  tags[messageCount] = tag;
  messageCount++;

  if (message.outgoing || (selectionOnLastRow && lastRowShown)) {
    // Follow the conversation.
    nav.requestSelection(composeRow());
    return;
  }
  // The user scrolled back to read: leave the view where it is.
  if (selectionOnLastRow) nav.selected = composeRow();
  // The list distrusts its measured page size when the row count changes and
  // would then move the view by its estimate, which is wrong for tall rows.
  // Appending a row does not change what a page holds.
  if (nav.drawnCount != 0) nav.drawnCount = listCount();
}

void MeshChatActivity::onThreadMessage(const meshc::StoredMessage& message, const uint32_t tag) {
  {
    RenderLock lock(*this);
    appendMessage(message, tag);
  }
  if (message.outgoing && tag != 0) {
    requestRepaintSoon();
  } else {
    requestRepaint(message.outgoing);
  }
}

void MeshChatActivity::onOutgoingState(const uint32_t tag, const meshc::StoredMessage& message, const bool settled) {
  {
    RenderLock lock(*this);
    size_t row = 0;
    while (row < messageCount && tags[row] != tag) row++;
    if (row == messageCount) {
      // Sent from an earlier visit to this screen. If the radio had taken it
      // by then, onEnter() read it from the history as "sent", without its
      // tag. Take the newest such row with the same time and text.
      for (size_t i = messageCount; i > 0 && row == messageCount; i--) {
        const meshc::StoredMessage& loaded = messages[i - 1];
        if (tags[i - 1] == 0 && loaded.outgoing && loaded.delivery == meshc::Delivery::Sent &&
            loaded.timestamp == message.timestamp && strcmp(loaded.text, message.text) == 0) {
          row = i - 1;
        }
      }
    }
    if (row < messageCount) {
      messages[row].delivery = message.delivery;
      tags[row] = settled ? 0 : tag;
    } else {
      appendMessage(message, settled ? 0 : tag);
    }
  }
  // Sent follows Sending at once; a later ack or failure shows right away.
  if (message.delivery == meshc::Delivery::Sent) {
    requestRepaintSoon();
  } else {
    requestRepaint(true);
  }
}

// fui::ListProps::rowProvider. Runs on the render task, which holds the
// render lock, so the message list cannot change under it.
void MeshChatActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<MeshChatActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  if (index < FIRST_MESSAGE_ROW) {
    item.label = self->hasOlder() ? tr(STR_MESH_EARLIER_MESSAGES) : tr(STR_MESH_NO_EARLIER_MESSAGES);
    return;
  }
  if (index == self->quickRow()) {
    item.label = tr(STR_MESH_QUICK_REPLIES);
    return;
  }
  const size_t message = static_cast<size_t>(index - FIRST_MESSAGE_ROW);
  if (!self->messages || message >= self->messageCount) {
    item.label = self->linkReady ? tr(STR_MESH_WRITE_MESSAGE) : tr(STR_MESH_NOT_CONNECTED);
    return;
  }
  meshrow::formatHeader(self->messages[message], self->direct ? self->title : nullptr, self->nowEpoch, self->rowHeader,
                        sizeof(self->rowHeader));
  item.label = self->rowHeader;
  item.subtitle = self->messages[message].text;
}

// Touch only. event.value is a row of the last render; messages may have
// arrived since.
int MeshChatActivity::rowForTouch(const int touchedValue) const {
  if (touchedValue < FIRST_MESSAGE_ROW) return 0;
  const auto shown = static_cast<int>(shownCount.load());
  if (touchedValue - FIRST_MESSAGE_ROW == shown) return composeRow();
  if (touchedValue - FIRST_MESSAGE_ROW == shown + 1) return quickRow();
  const int message = touchedValue - FIRST_MESSAGE_ROW - static_cast<int>(droppedCount - shownDropped.load());
  if (message < 0 || message >= static_cast<int>(messageCount)) return -1;
  return message + FIRST_MESSAGE_ROW;
}

void MeshChatActivity::onRowAction(const fui::ActionEvent& event) {
  // Rows change under a tap that does nothing or opens another screen; a
  // lingering flash would gray whichever row is there next.
  app.clearTapFlash();
  const int index = rowForTouch(event.value);
  if (index < 0) return;
  const bool messageRow = index >= FIRST_MESSAGE_ROW && index < composeRow();
  if (event.longPress) {
    if (messageRow) showDetails(index);
    return;
  }
  if (index < FIRST_MESSAGE_ROW && !hasOlder()) return;
  // Most message rows do nothing when tapped. Leaving the selection alone
  // keeps the view following new messages.
  if (messageRow && !canRetry(messages[index - FIRST_MESSAGE_ROW])) return;
  nav.selected = index;
  activateIndex(index);
}

void MeshChatActivity::onRowLongPress(const int index) {
  if (index >= FIRST_MESSAGE_ROW && index < composeRow()) showDetails(index);
}

bool MeshChatActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool MeshChatActivity::handleButtons() {
  constexpr unsigned long DETAILS_HOLD_MS = 1000;
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, DETAILS_HOLD_MS)) {
    onRowLongPress(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void MeshChatActivity::render(RenderLock&& lock) {
  // The popup is drawn over the messages already on the panel.
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void MeshChatActivity::showDetails(const int index) {
  if (optionPopup.isActive() || !messages || index < FIRST_MESSAGE_ROW || index >= composeRow()) return;
  const meshc::StoredMessage& message = messages[index - FIRST_MESSAGE_ROW];
  const bool retry = canRetry(message) && linkReady;
  const char* options[2] = {tr(STR_OK_BUTTON), tr(STR_MESH_SEND_AGAIN)};
  {
    // The render task reads the popup's text.
    RenderLock lock(*this);
    meshrow::formatDetails(message, direct ? title : nullptr, detailsText, sizeof(detailsText));
    // A copy: new messages can shift the rows while the popup is open.
    snprintf(retryText, sizeof(retryText), "%s", message.text);
    optionPopup.showMessage("", detailsText, options, retry ? 2 : 1, 0, [this](const int choice) {
      // The popup closes before this runs; the keyboard may open from here.
      if (choice == 1 && linkReady) compose(retryText);
    });
  }
  requestUpdate();
}

void MeshChatActivity::showQuickReplies() {
  if (optionPopup.isActive() || !linkReady) return;
  const char* options[meshquick::MAX_REPLIES];
  // Read from the card before taking the lock; the popup copies the texts.
  quickReplyCount = meshquick::load(quickReplies, meshquick::MAX_REPLIES);
  {
    RenderLock lock(*this);
    const size_t limit = MESH_SERVICE.maxTextBytes(threadKey);
    size_t usable = 0;
    for (size_t i = 0; i < quickReplyCount; i++) {
      // One the radio would cut is left out.
      if (strlen(quickReplies[i]) > limit) continue;
      if (usable != i) memcpy(quickReplies[usable], quickReplies[i], sizeof(quickReplies[i]));
      usable++;
    }
    quickReplyCount = usable;
    for (size_t i = 0; i < quickReplyCount; i++) options[i] = quickReplies[i];
    if (quickReplyCount == 0) return;
    optionPopup.show(tr(STR_MESH_QUICK_REPLIES), options, static_cast<int>(quickReplyCount), 0,
                     [this](const int choice) { pendingQuickReply = choice; });
  }
  requestUpdate();
}

void MeshChatActivity::activateIndex(const int index) {
  if (index < 0 || index > quickRow()) return;
  if (index == quickRow()) {
    showQuickReplies();
    return;
  }
  if (index < FIRST_MESSAGE_ROW) {
    openHistory();
    return;
  }
  // The link is down: nothing can be sent until MeshService reconnects.
  if (!linkReady) return;
  if (index == composeRow()) {
    compose("");
    return;
  }
  // A message that did not get through opens the keyboard with its text.
  const meshc::StoredMessage& message = messages[index - FIRST_MESSAGE_ROW];
  if (canRetry(message)) compose(message.text);
}

void MeshChatActivity::openHistory() {
  if (!hasOlder()) return;
  // Once the list has dropped messages, olderEnd no longer marks where the
  // loaded ones begin; the history then starts from its newest page.
  const uint32_t endOffset = droppedCount == 0 ? olderEnd : 0;
  auto history = makeUniqueNoThrow<MeshHistoryActivity>(renderer, mappedInput, threadKey, title, direct, endOffset);
  if (!history) {
    LOG_ERR("MCHAT", "OOM: history screen");
    return;
  }
  startActivityForResult(std::move(history), [this](const ActivityResult&) {
    // Back to the newest messages.
    nav.requestSelection(composeRow());
    resumeFromChild();
  });
}

void MeshChatActivity::compose(const char* initialText) {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, title, initialText,
                                                           MESH_SERVICE.maxTextBytes(threadKey), InputType::Text);
  if (!keyboard) {
    LOG_ERR("MCHAT", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    if (!result.isCancelled) {
      const std::string& text = std::get<KeyboardResult>(result.data).text;
      // The service reports the message back through onThreadMessage().
      if (!text.empty() && !MESH_SERVICE.sendText(threadKey, text.data(), text.size())) {
        LOG_ERR("MCHAT", "Message of %u bytes refused", static_cast<unsigned>(text.size()));
      }
    }
    resumeFromChild();
  });
}

void MeshChatActivity::buildScreen(UiScreen& screen) {
  meshrow::setListMargins(screen, renderer);

  fui::ListProps props;
  props.rowProvider = &MeshChatActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(listCount());
  shownCount = static_cast<uint32_t>(messageCount);
  shownDropped = droppedCount;
  props.action = ACTION_ROW;
  // Tap acts, long press shows a message's details; buttons stay in loop().
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  meshrow::setMessageRowStyle(screen, props);
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_MESH
