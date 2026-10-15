#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshLink.h>

#include <atomic>
#include <string>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "mesh/MeshService.h"

// The Mesh screen: pair with a MeshCore companion radio, list its channels
// and direct-message conversations, and show its state. The BLE link exists only while this screen is on the
// activity stack: onEnter() starts MeshService and onExit() stops it, so the
// link stays up under a chat opened from here. The rows follow the service's
// phase: scan results while searching, channels and radio details once the
// handshake is done.
class MeshActivity final : public UiListActivity {
 public:
  explicit MeshActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Pairing and the handshake last seconds; do not sleep in the middle.
  bool preventAutoSleep() override;

 private:
  enum class RowAction : uint8_t {
    None,
    Scan,
    ConnectSaved,
    ForgetSaved,
    ConnectDevice,
    EnterPin,
    OpenChannel,
    OpenConversation,
    OpenContacts,
    SendAdvert,
    Disconnect,
    MarkAllRead,
    JoinChannel,
    OpenSettings,
    OpenNearby,
    ShowMyQr
  };
  // What to do once the popup that asked has closed: a popup's callback must
  // not open another popup.
  enum class Pending : uint8_t {
    None,
    ConfirmClear,
    ConfirmLeave,
    HashtagName,
    PrivateName,
    PrivateSecret,
    ShowChannelQr,
    RequestResult
  };

  // Every channel and conversation plus the status, radio and action rows.
  static constexpr int MAX_ROWS = MeshService::MAX_CHANNELS + MeshService::MAX_CONVERSATIONS + 12;
  static constexpr size_t TEXT_SIZE = 40;
  static constexpr size_t SUBTITLE_SIZE = 64;

  int listCount() const override { return rowCount; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void onBackButton() override;
  const char* headerTitle() const override;

  // What the rows show, and what activating each one would do. The service
  // reports many changes that alter neither.
  struct RowsDigest {
    uint32_t content = 0;
    uint32_t actions = 0;
  };

  void syncFromService();
  // Caller holds the render lock.
  RowsDigest digestRows() const;
  // Caller holds the render lock: the render task reads the row arrays.
  void rebuildRows(MeshService::Phase phase);
  void addStatusRow(MeshService::Phase phase);
  void addDeviceRows();
  void addChannelRows();
  void addConversationRows();
  void addRadioRows();
  // Appends a row and returns its value buffer (TEXT_SIZE bytes), or nullptr
  // when the list is full. target is the scan result, channel or conversation
  // the row acts on.
  char* addRow(const char* label, RowAction action = RowAction::None, uint8_t target = 0);
  // Points the row's subtitle at its buffer (SUBTITLE_SIZE bytes) and returns it.
  char* subtitleFor(int row);
  void promptForPin();
  // Result handler of every screen opened from here. The panel shows that
  // screen until the repaint after the handler, so taps wait for it.
  void resumeFromChild();
  // Opens the chat whose key and name were copied into these buffers.
  void openChat(const char* threadKey, const char* title);
  void openContacts();
  // The choices for a channel or conversation row.
  void showRowActions(int index);
  void showJoinChoices();
  void runPending();
  void askText(const char* title, const char* initial, size_t maxLen, void (MeshActivity::*done)(const std::string&));
  void joinHashtag(const std::string& name);
  void choosePrivateName(const std::string& name);
  void joinPrivate(const std::string& secretHex);
  void watchRequest(MeshService::Request kind, StrId doneText, StrId failedText);
  void showMessage(const char* text);
  void openQr(const char* title, const char* payload, const char* caption);
  void openChannelQr();

  freeink::ui::ListItem rowItems[MAX_ROWS]{};
  RowAction rowActions[MAX_ROWS]{};
  uint8_t rowTargets[MAX_ROWS]{};
  char rowValues[MAX_ROWS][TEXT_SIZE]{};
  char rowSubtitles[MAX_ROWS][SUBTITLE_SIZE]{};
  char deviceLabels[MeshLink::MAX_DEVICES][sizeof(MeshLinkDevice::name)]{};
  // Copies: the service's own strings change while the render task draws.
  char channelLabels[MeshService::MAX_CHANNELS][meshc::NAME_FIELD_SIZE + 1]{};
  char channelKeys[MeshService::MAX_CHANNELS][meshhistory::THREAD_KEY_SIZE]{};
  char conversationLabels[MeshService::MAX_CONVERSATIONS][meshc::NAME_FIELD_SIZE + 1]{};
  char conversationKeys[MeshService::MAX_CONVERSATIONS][meshhistory::THREAD_KEY_SIZE]{};
  int rowCount = 0;

  MeshService::Phase shownPhase = MeshService::Phase::Off;
  uint32_t shownRevision = 0;
  uint8_t shownDevices = 0;
  uint32_t lastRebuildMs = 0;
  uint32_t nextStartAttemptMs = 0;
  uint8_t startAttempts = 0;
  bool rowsBuilt = false;
  RowsDigest shownDigest;
  // False from a rebuild that changed what a row does until the panel shows
  // it. Until then the screen still has the old rows, and a tap or Confirm
  // would act on a row the user cannot see: another conversation, once the
  // list has re-ordered.
  std::atomic<bool> rowsDrawn{false};
  bool startFailed = false;
  bool pinPrompted = false;
  bool advertSent = false;

  OptionPopup optionPopup;
  Pending pending = Pending::None;
  // The row a popup is about, copied: the rows can change under it.
  char actionKey[meshhistory::THREAD_KEY_SIZE] = {0};
  char actionName[meshc::NAME_FIELD_SIZE + 1] = {0};
  bool actionIsChannel = false;
  uint8_t actionSlot = 0;
  uint8_t actionSecret[meshc::CHANNEL_SECRET_SIZE] = {0};
  char privateName[meshc::NAME_FIELD_SIZE + 1] = {0};
  uint8_t privateSecret[meshc::CHANNEL_SECRET_SIZE] = {};  // between the two join prompts
  // A request whose result is shown in a popup once it is done.
  MeshService::Request watched = MeshService::Request::None;
  StrId watchedDone = StrId::STR_OK_BUTTON;
  StrId watchedFailed = StrId::STR_OK_BUTTON;
  char messageText[192] = {0};  // the popup keeps its own copy
};

#endif  // CROSSPOINT_MESH
