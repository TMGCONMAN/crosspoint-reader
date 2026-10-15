#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshThreadLog.h>

#include <atomic>
#include <memory>

#include "MeshMessageRow.h"
#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "mesh/MeshHistory.h"
#include "mesh/MeshQuickReplies.h"
#include "mesh/MeshService.h"

// One conversation, with a channel or with a contact: the newest messages
// from the history, new ones as they arrive, a first row that opens the older
// history, a row that opens the keyboard to write a message and a last row
// of quick replies. A long press on a message, or holding Confirm on it,
// shows its details.
// MeshActivity stays on the stack underneath and keeps the link up.
class MeshChatActivity final : public UiListActivity, private MeshService::ThreadListener {
 public:
  MeshChatActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* threadKey, const char* title);
  ~MeshChatActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Messages kept on screen; older ones are read with MeshHistoryActivity.
  static constexpr size_t CAPACITY = 40;
  // Row 0 opens the older history, the messages follow, the last row writes one.
  static constexpr int FIRST_MESSAGE_ROW = 1;

  int listCount() const override { return quickRow() + 1; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override { return title; }

  // MeshService::ThreadListener
  void onThreadMessage(const meshc::StoredMessage& message, uint32_t tag) override;
  void onOutgoingState(uint32_t tag, const meshc::StoredMessage& message, bool settled) override;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  int composeRow() const { return static_cast<int>(messageCount) + FIRST_MESSAGE_ROW; }
  int quickRow() const { return composeRow() + 1; }
  // The row a touch of the last render meant, or -1 for one that is gone.
  int rowForTouch(int touchedValue) const;
  void showDetails(int index);
  void showQuickReplies();
  bool hasOlder() const { return droppedCount > 0 || meshhistory::hasOlderPage(olderEnd); }
  // Caller holds the render lock.
  void appendMessage(const meshc::StoredMessage& message, uint32_t tag);
  void compose(const char* initialText);
  void openHistory();
  // urgent skips the wait that groups a burst of incoming messages into one repaint.
  void requestRepaint(bool urgent);
  void requestRepaintSoon();
  // The repaint ActivityManager does after a screen above this one closes
  // covers everything that changed meanwhile; loop() did not run.
  void resumeFromChild();

  char threadKey[meshhistory::THREAD_KEY_SIZE] = {0};
  char title[meshc::NAME_FIELD_SIZE + 1] = {0};
  bool direct = false;  // with a contact, not a channel

  // Written on the main loop under the render lock; read by the render task.
  // The list lives from onEnter() to onExit().
  std::unique_ptr<meshc::StoredMessage[]> messages;
  uint32_t tags[CAPACITY]{};  // non-zero while that message's delivery state can change
  size_t messageCount = 0;
  // Where the history before the loaded messages ends.
  uint32_t olderEnd = 0;
  // Messages dropped from the front of the full list since onEnter().
  uint32_t droppedCount = 0;
  // Written on the main loop without the lock: single values, and a row
  // drawn from a value one pass old is harmless.
  bool linkReady = false;
  uint32_t nowEpoch = 0;

  // The list as the last render laid it out. Touch rows carry those indexes
  // until the next render, which can be more than a second later.
  std::atomic<uint32_t> shownCount{0};
  std::atomic<uint32_t> shownDropped{0};

  char rowHeader[meshrow::HEADER_SIZE] = {0};  // render task only

  bool repaintPending = false;
  uint32_t nextRepaintMs = 0;

  OptionPopup optionPopup;
  char detailsText[256] = {0};  // the popup keeps its own copy
  char retryText[meshc::MAX_STORED_TEXT_BYTES + 1] = {0};
  char quickReplies[meshquick::MAX_REPLIES][meshquick::REPLY_SIZE] = {};
  size_t quickReplyCount = 0;
  // Set from a popup callback, which must not open another popup.
  int pendingQuickReply = -1;
};

#endif  // CROSSPOINT_MESH
