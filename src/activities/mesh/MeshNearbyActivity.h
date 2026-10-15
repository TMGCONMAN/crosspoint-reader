#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include "activities/UiListActivity.h"
#include "mesh/MeshService.h"

// The repeaters and room servers in direct range, found by asking them to
// answer: each shows how well this radio heard it and how well it heard this
// radio. Choosing one that is a contact opens its server screen.
class MeshNearbyActivity final : public UiListActivity {
 public:
  MeshNearbyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  static constexpr int MAX_ROWS = 1 + MeshService::MAX_NEARBY;
  static constexpr size_t VALUE_SIZE = 32;

  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;

  // Caller holds the render lock.
  void refreshRows();

  freeink::ui::ListItem items[MAX_ROWS]{};
  char values[MAX_ROWS][VALUE_SIZE]{};
  char subtitles[MAX_ROWS][VALUE_SIZE]{};
  MeshService::NearbyNode nodes[MeshService::MAX_NEARBY];
  int rowCount = 0;
  uint32_t shownRevision = 0;
  MeshService::RequestState shownState = MeshService::RequestState::Idle;
};

#endif  // CROSSPOINT_MESH
