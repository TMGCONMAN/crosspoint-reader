#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshThreadLog.h>

#include <atomic>
#include <memory>

#include "MeshMessageRow.h"
#include "activities/UiListActivity.h"
#include "mesh/MeshHistory.h"

// Reads back through one conversation's history, a page at a time. The first
// row loads the page before the one shown. Nothing here changes while it is
// open: new messages go to the chat screen underneath.
class MeshHistoryActivity final : public UiListActivity {
 public:
  // endOffset is where the first page ends, as MeshHistory reports it; 0
  // starts from the newest message.
  MeshHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* threadKey, const char* title,
                      bool direct, uint32_t endOffset);

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;

 private:
  static constexpr size_t PAGE_SIZE = 40;
  // Row 0 loads the page before; the messages follow.
  static constexpr int FIRST_MESSAGE_ROW = 1;

  int listCount() const override { return static_cast<int>(messageCount) + FIRST_MESSAGE_ROW; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  const char* headerTitle() const override { return title; }

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  // Replaces the page shown with the one that ends at endOffset.
  void showPage(uint32_t endOffset);

  char threadKey[meshhistory::THREAD_KEY_SIZE] = {0};
  char title[meshc::NAME_FIELD_SIZE + 1] = {0};
  bool direct = false;
  uint32_t firstEndOffset = 0;

  // Written on the main loop under the render lock; read by the render task.
  std::unique_ptr<meshc::StoredMessage[]> messages;
  size_t messageCount = 0;
  uint32_t olderEnd = 0;
  uint32_t nowEpoch = 0;

  // False from loading a page until the panel shows it. A second tap in
  // that time would skip a page the user has not seen.
  std::atomic<bool> pageDrawn{false};

  char rowHeader[meshrow::HEADER_SIZE] = {0};  // render task only
};

#endif  // CROSSPOINT_MESH
