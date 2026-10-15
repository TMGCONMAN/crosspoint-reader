#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshFrames.h>

#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "mesh/MeshService.h"

// A repeater or room server: log in, read its status, and for a room open
// its message board. Opened from the contact list or the nearby list.
class MeshServerActivity final : public UiListActivity {
 public:
  MeshServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t* publicKey, const char* name,
                     uint8_t type);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class RowAction : uint8_t { None, Login, Status, OpenRoom };
  static constexpr int MAX_ROWS = 16;
  static constexpr size_t VALUE_SIZE = 32;

  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override { return name; }

  // Caller holds the render lock.
  void refreshRows();
  char* addRow(const char* label, RowAction action = RowAction::None);
  void login(const std::string& password);
  void openRoom();
  void showMessage(const char* text);

  uint8_t publicKey[meshc::PUB_KEY_SIZE] = {0};
  char name[meshc::NAME_FIELD_SIZE + 1] = {0};
  uint8_t type = 0;

  freeink::ui::ListItem items[MAX_ROWS]{};
  RowAction actions[MAX_ROWS]{};
  char values[MAX_ROWS][VALUE_SIZE]{};
  int rowCount = 0;
  uint32_t shownRevision = 0;

  OptionPopup optionPopup;
  MeshService::Request watched = MeshService::Request::None;
  char messageText[192] = {0};  // the popup keeps its own copy
};

#endif  // CROSSPOINT_MESH
