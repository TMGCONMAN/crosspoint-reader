#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshContactBook.h>

#include <atomic>
#include <memory>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

// The radio's contacts: favourites first, then the rest, each group with
// people ahead of rooms and repeaters. Choosing a person opens the
// conversation with them. A long press, or holding Confirm, offers to mark
// or unmark the contact as a favourite. Opened from MeshActivity, which stays
// on the stack and keeps the link up.
class MeshContactsActivity final : public UiListActivity {
 public:
  explicit MeshContactsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // A copy of what the list shows of one contact: the service's table
  // changes while the render task draws.
  struct Row {
    char name[meshc::NAME_FIELD_SIZE + 1];
    uint8_t keyPrefix[meshc::KEY_PREFIX_SIZE];
    uint8_t type;
    bool favorite;
    bool hasRoute;
    uint8_t routeHops;
  };

  static constexpr size_t SUBTITLE_SIZE = 48;

  int listCount() const override { return static_cast<int>(rowCount); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  const char* headerTitle() const override;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  static bool before(const Row& a, const Row& b);
  // Copies and sorts the contacts, keeping the selection on the same contact.
  void refresh();
  // The row of the contact with this key, or -1.
  int findRow(const uint8_t* keyPrefix) const;
  void openChat(const Row& row);
  // A person opens the chat; a repeater or room its server screen.
  void openRow(const Row& row);
  void openServer(const Row& row);
  void showQr(const Row& row);
  void showDetails();
  // The choices for one contact: open its chat, change its favourite mark.
  void showActions(int index);
  void showFavoriteFailure();

  // Written on the main loop under the render lock; read by the render task.
  std::unique_ptr<Row[]> rows;
  size_t rowCapacity = 0;
  size_t rowCount = 0;

  uint32_t shownRevision = 0;
  uint32_t nextRefreshMs = 0;
  // False from a refresh until the panel shows it. Until then the screen
  // still has the old order, and a tap or Confirm would open the chat of a
  // contact the user cannot see.
  std::atomic<bool> rowsDrawn{false};

  OptionPopup optionPopup;
  // The contact the open popup is about. The rows can re-sort under it.
  uint8_t actionKey[meshc::KEY_PREFIX_SIZE] = {0};
  // A favourite change was sent; refresh as soon as the radio has answered.
  bool awaitingFavorite = false;
  // Set from the popup's own callback, which must not open another popup.
  bool favoriteRefused = false;
  // Details were asked for; shown in a popup once the radio has answered.
  bool awaitingDetails = false;
  char detailsText[320] = {0};  // the popup keeps its own copy
  char detailsName[meshc::NAME_FIELD_SIZE + 1] = {0};

  char rowSubtitle[SUBTITLE_SIZE] = {0};  // render task only
};

#endif  // CROSSPOINT_MESH
