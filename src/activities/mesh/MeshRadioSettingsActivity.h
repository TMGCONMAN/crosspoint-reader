#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <atomic>
#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "mesh/MeshService.h"

// The radio's own settings: node name, transmit power, the LoRa parameters,
// its position and whether adverts carry it. Each change is written to the
// radio at once and shown once the radio reports itself again. Opened from
// MeshActivity, which keeps the link up.
class MeshRadioSettingsActivity final : public UiListActivity {
 public:
  MeshRadioSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum Row : uint8_t { Name, TxPower, Frequency, Bandwidth, Spreading, Coding, Position, SharePosition, ROW_COUNT };
  // A radio change waits for the user to confirm it; see confirmRadio().
  enum class Pending : uint8_t { None, ConfirmRadio, Result };

  static constexpr size_t VALUE_SIZE = 40;

  int listCount() const override { return ROW_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  // Caller holds the render lock.
  void refreshRows();
  void askText(const char* title, const char* initial, size_t maxLen,
               void (MeshRadioSettingsActivity::*done)(const std::string&));
  void applyName(const std::string& text);
  void applyTxPower(const std::string& text);
  void applyFrequency(const std::string& text);
  void applyPosition(const std::string& text);
  void chooseBandwidth();
  void chooseNumber(const char* title, int first, int last, int current, uint8_t* target);
  void confirmRadio();
  void startedOrRefused(bool started);
  void showMessage(const char* text);

  freeink::ui::ListItem items[ROW_COUNT]{};
  char values[ROW_COUNT][VALUE_SIZE]{};
  uint32_t shownRevision = 0;

  OptionPopup optionPopup;
  Pending pending = Pending::None;
  // The LoRa settings the user picked, waiting for confirmation.
  uint32_t newFreqKhz = 0;
  uint32_t newBwHz = 0;
  uint8_t newSf = 0;
  uint8_t newCr = 0;
  bool watching = false;
  char messageText[192] = {0};  // the popup keeps its own copy
  char optionLabels[12][12] = {};
};

#endif  // CROSSPOINT_MESH
