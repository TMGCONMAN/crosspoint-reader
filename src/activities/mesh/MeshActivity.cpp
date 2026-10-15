#include "MeshActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "MappedInputManager.h"
#include "MeshChatActivity.h"
#include "MeshContactsActivity.h"
#include "MeshMessageRow.h"
#include "MeshNearbyActivity.h"
#include "MeshQrActivity.h"
#include "MeshRadioSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "mesh/MeshQr.h"
#include "mesh/MeshRadioStore.h"

namespace fui = freeink::ui;
using Phase = MeshService::Phase;

namespace {

// E-ink repaints are slow; contact and message events arrive in bursts.
constexpr uint32_t REPAINT_INTERVAL_MS = 1500;
constexpr uint32_t START_RETRY_MS = 1000;
constexpr uint8_t MAX_START_ATTEMPTS = 5;
constexpr size_t PIN_DIGITS = 6;
// MeshCore's PIN for radios without a screen; radios with one show their own.
constexpr char DEFAULT_PIN[] = "123456";
constexpr unsigned long ACTIONS_HOLD_MS = 1000;

bool parseHexSecret(const std::string& text, uint8_t* out) {
  size_t digits = 0;
  uint8_t value = 0;
  for (const char c : text) {
    int nibble = -1;
    if (c >= '0' && c <= '9') nibble = c - '0';
    if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
    if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
    if (c == ' ' || c == '-' || c == ':') continue;
    if (nibble < 0 || digits >= 2 * meshc::CHANNEL_SECRET_SIZE) return false;
    value = static_cast<uint8_t>((value << 4) | nibble);
    if (digits % 2 == 1) out[digits / 2] = value;
    digits++;
  }
  return digits == 2 * meshc::CHANNEL_SECRET_SIZE;
}

bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

bool parsePin(const std::string& text, uint32_t& pin) {
  if (text.size() != PIN_DIGITS) return false;
  pin = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    pin = pin * 10 + static_cast<uint32_t>(c - '0');
  }
  return true;
}

const char* problemText(MeshService::Problem problem) {
  switch (problem) {
    case MeshService::Problem::ConnectFailed:
      return tr(STR_MESH_CONNECT_FAILED);
    case MeshService::Problem::PairingFailed:
      return tr(STR_MESH_PAIRING_FAILED);
    case MeshService::Problem::NotCompanion:
      return tr(STR_MESH_NOT_COMPANION);
    case MeshService::Problem::LinkLost:
      return tr(STR_MESH_LINK_LOST);
    case MeshService::Problem::RadioSilent:
      return tr(STR_MESH_RADIO_SILENT);
    case MeshService::Problem::None:
      break;
  }
  return tr(STR_MESH_NOT_CONNECTED);
}

// 62500 -> "62.5", 31250 -> "31.25", 250000 -> "250"
void formatKilo(char* out, size_t size, uint32_t value) {
  const unsigned whole = value / 1000;
  const unsigned frac = value % 1000;
  if (frac == 0) {
    snprintf(out, size, "%u", whole);
  } else if (frac % 100 == 0) {
    snprintf(out, size, "%u.%u", whole, frac / 100);
  } else if (frac % 10 == 0) {
    snprintf(out, size, "%u.%02u", whole, frac / 10);
  } else {
    snprintf(out, size, "%u.%03u", whole, frac);
  }
}

}  // namespace

MeshActivity::MeshActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Mesh", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

const char* MeshActivity::headerTitle() const { return tr(STR_MESH); }

void MeshActivity::onEnter() {
  UiListActivity::onEnter();
  startFailed = !MESH_SERVICE.acquire();
  startAttempts = 1;
  nextStartAttemptMs = millis() + START_RETRY_MS;
  pinPrompted = false;
  advertSent = false;
  rowsBuilt = false;
  syncFromService();
}

void MeshActivity::onExit() {
  // The link stays up for a moment, so Home keeps it.
  MESH_SERVICE.release();
  UiListActivity::onExit();
}

bool MeshActivity::preventAutoSleep() {
  const Phase phase = MESH_SERVICE.phase();
  return phase == Phase::Scanning || phase == Phase::Connecting || phase == Phase::NeedPin || phase == Phase::Handshake;
}

void MeshActivity::onBackButton() { activityManager.goHome(HomeMenuItem::MESH); }

void MeshActivity::loop() {
  if (!optionPopup.isActive()) runPending();
  if (watched != MeshService::Request::None && !optionPopup.isActive()) {
    const auto state = MESH_SERVICE.requestState(watched);
    if (state == MeshService::RequestState::Done || state == MeshService::RequestState::Failed) {
      showMessage(I18N.get(state == MeshService::RequestState::Done ? watchedDone : watchedFailed));
      MESH_SERVICE.clearRequest();
      watched = MeshService::Request::None;
    } else if (state == MeshService::RequestState::Idle) {
      watched = MeshService::Request::None;
    }
  }
  // Not under an open popup: it is drawn over the rows as they were.
  if (!optionPopup.isActive()) syncFromService();
  UiListActivity::loop();
}

void MeshActivity::syncFromService() {
  const uint32_t nowMs = millis();
  Phase phase = MESH_SERVICE.phase();

  // start() fails while an earlier teardown is still finishing; retry.
  if (phase == Phase::Off && startAttempts < MAX_START_ATTEMPTS && reached(nowMs, nextStartAttemptMs)) {
    startFailed = !MESH_SERVICE.start();
    startAttempts++;
    nextStartAttemptMs = nowMs + START_RETRY_MS;
    phase = MESH_SERVICE.phase();
    rowsBuilt = false;
  }

  if (phase == Phase::NeedPin) {
    if (!pinPrompted) {
      pinPrompted = true;
      promptForPin();
      return;
    }
  } else if (phase != Phase::Connecting) {
    pinPrompted = false;
  }

  const uint32_t revision = MESH_SERVICE.revision();
  const uint8_t devices = MESH_SERVICE.deviceCount();
  const bool phaseChanged = phase != shownPhase;
  if (rowsBuilt && !phaseChanged && revision == shownRevision && devices == shownDevices) return;
  if (rowsBuilt && !phaseChanged && nowMs - lastRebuildMs < REPAINT_INTERVAL_MS) return;

  if (phase != Phase::Ready) advertSent = false;
  RowsDigest digest;
  {
    RenderLock lock(*this);
    rebuildRows(phase);
    digest = digestRows();
    if (digest.actions != shownDigest.actions) rowsDrawn = false;
  }
  // An e-ink repaint takes a second; skip it when the rows came out the same.
  const bool repaint = !rowsBuilt || digest.content != shownDigest.content || digest.actions != shownDigest.actions;
  shownDigest = digest;
  shownPhase = phase;
  shownRevision = revision;
  shownDevices = devices;
  lastRebuildMs = nowMs;
  rowsBuilt = true;
  if (repaint) requestUpdate();
}

MeshActivity::RowsDigest MeshActivity::digestRows() const {
  // FNV-1a over the strings and fields, with a separator after each.
  const auto mix = [](uint32_t hash, const char* text) {
    for (; text != nullptr && *text != 0; text++) hash = (hash ^ static_cast<uint8_t>(*text)) * 16777619u;
    return (hash ^ 0xFFu) * 16777619u;
  };
  const auto mixByte = [](uint32_t hash, uint8_t value) { return (hash ^ value) * 16777619u; };

  RowsDigest digest{2166136261u, 2166136261u};
  for (int row = 0; row < rowCount; row++) {
    const fui::ListItem& item = rowItems[row];
    digest.content = mix(digest.content, item.label);
    digest.content = mix(digest.content, item.value);
    digest.content = mix(digest.content, item.subtitle);
    digest.content = mix(digest.content, item.sectionHeading);

    const RowAction action = rowActions[row];
    const uint8_t target = rowTargets[row];
    digest.actions = mixByte(mixByte(digest.actions, static_cast<uint8_t>(action)), target);
    // A chat row is the conversation it opens, not its position in a list.
    if (action == RowAction::OpenChannel && target < MeshService::MAX_CHANNELS) {
      digest.actions = mix(digest.actions, channelKeys[target]);
    } else if (action == RowAction::OpenConversation && target < MeshService::MAX_CONVERSATIONS) {
      digest.actions = mix(digest.actions, conversationKeys[target]);
    }
  }
  return digest;
}

// The render task holds the render lock until this returns, so a rebuild
// cannot slip in between the panel refresh and the flag.
void MeshActivity::render(RenderLock&& lock) {
  // The popup is drawn over the rows already on the panel.
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
  rowsDrawn = true;
}

char* MeshActivity::addRow(const char* label, RowAction action, uint8_t target) {
  if (rowCount >= MAX_ROWS) return nullptr;
  fui::ListItem& item = rowItems[rowCount];
  item = fui::ListItem{};
  item.label = label;
  item.actionValue = static_cast<int16_t>(rowCount);
  rowValues[rowCount][0] = 0;
  rowSubtitles[rowCount][0] = 0;
  item.value = rowValues[rowCount];
  rowActions[rowCount] = action;
  rowTargets[rowCount] = target;
  return rowValues[rowCount++];
}

char* MeshActivity::subtitleFor(const int row) {
  rowItems[row].subtitle = rowSubtitles[row];
  return rowSubtitles[row];
}

void MeshActivity::addStatusRow(Phase phase) {
  const char* text = tr(STR_MESH_STARTING);
  switch (phase) {
    case Phase::Off:
      if (startFailed) text = tr(STR_MESH_BT_FAILED);
      break;
    case Phase::Stopping:
      break;
    case Phase::Idle:
      text = problemText(MESH_SERVICE.problem());
      break;
    case Phase::Scanning:
      text = tr(STR_SCANNING);
      break;
    case Phase::Connecting:
      text = tr(STR_CONNECTING);
      break;
    case Phase::NeedPin:
      text = tr(STR_MESH_PIN_NEEDED);
      break;
    case Phase::Handshake:
      text = tr(STR_MESH_READING_RADIO);
      break;
    case Phase::Ready:
      text = advertSent ? tr(STR_MESH_ADVERT_SENT) : tr(STR_CONNECTED);
      break;
  }
  addRow(text);
}

void MeshActivity::addDeviceRows() {
  const uint8_t count = MESH_SERVICE.deviceCount();
  for (uint8_t i = 0; i < count && i < MeshLink::MAX_DEVICES; i++) {
    MeshLinkDevice device;
    if (!MESH_SERVICE.device(i, device)) break;
    strncpy(deviceLabels[i], device.name, sizeof(deviceLabels[i]) - 1);
    deviceLabels[i][sizeof(deviceLabels[i]) - 1] = 0;
    if (char* value = addRow(deviceLabels[i], RowAction::ConnectDevice, i)) {
      snprintf(value, TEXT_SIZE, "%d dBm", static_cast<int>(device.rssi));
    }
  }
}

void MeshActivity::addChannelRows() {
  const uint8_t count = MESH_SERVICE.channelCount();
  if (MESH_SERVICE.unreadTotal() > 0) addRow(tr(STR_MESH_MARK_ALL_READ), RowAction::MarkAllRead);
  for (uint8_t i = 0; i < count && i < MeshService::MAX_CHANNELS; i++) {
    MeshService::ChannelView channel;
    if (!MESH_SERVICE.channel(i, channel)) break;
    snprintf(channelLabels[i], sizeof(channelLabels[i]), "%s", channel.name);
    snprintf(channelKeys[i], sizeof(channelKeys[i]), "%s", channel.threadKey);
    char* value = addRow(channelLabels[i], RowAction::OpenChannel, i);
    if (!value) break;
    const int row = rowCount - 1;
    if (i == 0) rowItems[row].sectionHeading = tr(STR_MESH_CHANNELS);
    if (channel.unread > 0) snprintf(value, TEXT_SIZE, "%u", static_cast<unsigned>(channel.unread));
    if (channel.preview[0] != 0) snprintf(subtitleFor(row), SUBTITLE_SIZE, "%s", channel.preview);
  }
  if (MESH_SERVICE.freeChannelSlot() >= 0) {
    addRow(tr(STR_MESH_JOIN_CHANNEL), RowAction::JoinChannel);
    if (count == 0) rowItems[rowCount - 1].sectionHeading = tr(STR_MESH_CHANNELS);
  }
}

void MeshActivity::addConversationRows() {
  const uint8_t count = MESH_SERVICE.conversationCount();
  bool headed = false;
  for (uint8_t i = 0; i < count && i < MeshService::MAX_CONVERSATIONS; i++) {
    MeshService::ConversationView conversation;
    if (!MESH_SERVICE.conversation(i, conversation)) break;
    snprintf(conversationLabels[i], sizeof(conversationLabels[i]), "%s", conversation.name);
    snprintf(conversationKeys[i], sizeof(conversationKeys[i]), "%s", conversation.threadKey);
    char* value = addRow(conversationLabels[i], RowAction::OpenConversation, i);
    if (!value) break;
    const int row = rowCount - 1;
    if (!headed) rowItems[row].sectionHeading = tr(STR_MESH_DIRECT_MESSAGES);
    headed = true;
    if (conversation.unread > 0) snprintf(value, TEXT_SIZE, "%u", static_cast<unsigned>(conversation.unread));
    if (conversation.preview[0] != 0) snprintf(subtitleFor(row), SUBTITLE_SIZE, "%s", conversation.preview);
  }
  // Starts a conversation with someone not listed above.
  if (char* value = addRow(tr(STR_MESH_CONTACTS), RowAction::OpenContacts)) {
    snprintf(value, TEXT_SIZE, "%u", static_cast<unsigned>(MESH_SERVICE.contactCount()));
    if (!headed) rowItems[rowCount - 1].sectionHeading = tr(STR_MESH_DIRECT_MESSAGES);
  }
}

void MeshActivity::addRadioRows() {
  const meshc::SelfInfo& self = MESH_SERVICE.selfInfo();
  const meshc::DeviceInfo& info = MESH_SERVICE.deviceInfo();
  char number[16];

  if (char* value = addRow(tr(STR_MESH_NAME))) {
    snprintf(value, TEXT_SIZE, "%s", self.name);
    const int row = rowCount - 1;
    rowItems[row].sectionHeading = tr(STR_MESH_RADIO);
    snprintf(subtitleFor(row), SUBTITLE_SIZE, "%s %s", info.model, info.version);
  }
  if (char* value = addRow(tr(STR_MESH_FREQUENCY))) {
    formatKilo(number, sizeof(number), self.freqKhz);
    snprintf(value, TEXT_SIZE, "%s MHz", number);
    formatKilo(number, sizeof(number), self.bwHz);
    // LoRa parameter names are the same in every language.
    snprintf(subtitleFor(rowCount - 1), SUBTITLE_SIZE, "BW %s kHz  SF%u  CR%u", number,
             static_cast<unsigned>(self.spreadingFactor), static_cast<unsigned>(self.codingRate));
  }
  if (char* value = addRow(tr(STR_MESH_TX_POWER))) {
    snprintf(value, TEXT_SIZE, "%d dBm", static_cast<int>(self.txPowerDbm));
  }
  if (char* value = addRow(tr(STR_MESH_RADIO_BATTERY))) {
    const unsigned mv = MESH_SERVICE.batteryMilliVolts();
    if (mv > 0) {
      snprintf(value, TEXT_SIZE, "%u.%02u V", mv / 1000, (mv % 1000) / 10);
    } else {
      snprintf(value, TEXT_SIZE, "-");
    }
  }
}

void MeshActivity::rebuildRows(Phase phase) {
  rowCount = 0;
  addStatusRow(phase);

  const bool hasSaved = MESH_RADIO_STORE.hasRadio();
  switch (phase) {
    case Phase::Off:
    case Phase::Stopping:
      break;
    case Phase::Idle:
      if (hasSaved) {
        if (char* value = addRow(tr(STR_CONNECT), RowAction::ConnectSaved)) {
          snprintf(value, TEXT_SIZE, "%s", MESH_RADIO_STORE.radioName());
        }
      }
      addRow(tr(STR_MESH_SCAN), RowAction::Scan);
      addDeviceRows();
      if (hasSaved) addRow(tr(STR_MESH_FORGET_RADIO), RowAction::ForgetSaved);
      break;
    case Phase::Scanning:
      addDeviceRows();
      addRow(tr(STR_CANCEL), RowAction::Disconnect);
      break;
    case Phase::Connecting:
      addRow(tr(STR_CANCEL), RowAction::Disconnect);
      break;
    case Phase::NeedPin:
      addRow(tr(STR_MESH_ENTER_PIN), RowAction::EnterPin);
      addRow(tr(STR_CANCEL), RowAction::Disconnect);
      break;
    case Phase::Handshake:
      addRow(tr(STR_MESH_DISCONNECT), RowAction::Disconnect);
      break;
    case Phase::Ready:
      addChannelRows();
      addConversationRows();
      addRadioRows();
      addRow(tr(STR_MESH_RADIO_SETTINGS), RowAction::OpenSettings);
      addRow(tr(STR_MESH_MY_QR), RowAction::ShowMyQr);
      if (MESH_SERVICE.canDiscover()) addRow(tr(STR_MESH_NEARBY), RowAction::OpenNearby);
      addRow(tr(STR_MESH_SEND_ADVERT), RowAction::SendAdvert);
      addRow(tr(STR_MESH_DISCONNECT), RowAction::Disconnect);
      break;
  }

  if (nav.selected >= rowCount) nav.selected = rowCount - 1;
}

// Touch only. A tap on rows the panel does not show yet must not move the
// selection either.
void MeshActivity::onRowAction(const fui::ActionEvent& event) {
  if (!rowsDrawn) {
    app.clearTapFlash();
    return;
  }
  UiListActivity::onRowAction(event);
}

void MeshActivity::resumeFromChild() {
  rowsDrawn = false;
  rowsBuilt = false;
  syncFromService();
}

void MeshActivity::activateIndex(const int index) {
  // The rows change after most actions; a lingering flash would gray an
  // unrelated row.
  app.clearTapFlash();
  if (index < 0 || index >= rowCount || !rowsDrawn) return;
  switch (rowActions[index]) {
    case RowAction::None:
      return;
    case RowAction::Scan:
      MESH_SERVICE.startScan();
      break;
    case RowAction::ConnectSaved:
      MESH_SERVICE.connectSaved();
      break;
    case RowAction::ForgetSaved:
      MESH_SERVICE.forgetSavedRadio();
      break;
    case RowAction::ConnectDevice: {
      MeshLinkDevice device;
      if (MESH_SERVICE.device(rowTargets[index], device)) MESH_SERVICE.connectTo(device);
      break;
    }
    case RowAction::OpenChannel:
      if (rowTargets[index] < MeshService::MAX_CHANNELS) {
        openChat(channelKeys[rowTargets[index]], channelLabels[rowTargets[index]]);
      }
      return;
    case RowAction::OpenConversation:
      if (rowTargets[index] < MeshService::MAX_CONVERSATIONS) {
        openChat(conversationKeys[rowTargets[index]], conversationLabels[rowTargets[index]]);
      }
      return;
    case RowAction::OpenContacts:
      openContacts();
      return;
    case RowAction::EnterPin:
      promptForPin();
      return;
    case RowAction::SendAdvert:
      advertSent = MESH_SERVICE.sendAdvert();
      break;
    case RowAction::Disconnect:
      MESH_SERVICE.disconnect();
      break;
    case RowAction::MarkAllRead:
      MESH_SERVICE.markAllRead();
      break;
    case RowAction::JoinChannel:
      showJoinChoices();
      return;
    case RowAction::OpenSettings: {
      auto settings = makeUniqueNoThrow<MeshRadioSettingsActivity>(renderer, mappedInput);
      if (settings) startActivityForResult(std::move(settings), [this](const ActivityResult&) { resumeFromChild(); });
      return;
    }
    case RowAction::OpenNearby: {
      auto nearby = makeUniqueNoThrow<MeshNearbyActivity>(renderer, mappedInput);
      if (nearby) startActivityForResult(std::move(nearby), [this](const ActivityResult&) { resumeFromChild(); });
      return;
    }
    case RowAction::ShowMyQr: {
      const meshc::SelfInfo& self = MESH_SERVICE.selfInfo();
      char url[meshqr::URL_SIZE];
      if (meshqr::contactUrl(self.name, self.publicKey, meshc::adv::CHAT, url, sizeof(url))) {
        openQr(self.name, url, tr(STR_MESH_MY_QR_HINT));
      }
      return;
    }
  }
  // Repaint on the next pass without waiting out the repaint interval.
  rowsBuilt = false;
}

void MeshActivity::promptForPin() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_MESH_ENTER_PIN), DEFAULT_PIN,
                                                           PIN_DIGITS, InputType::Text);
  if (!keyboard) {
    LOG_ERR("MESH", "OOM: PIN keyboard");
    MESH_SERVICE.disconnect();
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    rowsDrawn = false;  // the panel shows the keyboard until the repaint that follows
    rowsBuilt = false;
    if (result.isCancelled) {
      MESH_SERVICE.disconnect();
      return;
    }
    uint32_t pin = 0;
    if (!parsePin(std::get<KeyboardResult>(result.data).text, pin)) {
      // Not six digits: ask again while the radio is still waiting.
      pinPrompted = false;
      return;
    }
    // A false return means pairing already timed out; the status row shows it.
    MESH_SERVICE.submitPin(pin);
  });
}

void MeshActivity::openChat(const char* threadKey, const char* title) {
  auto chat = makeUniqueNoThrow<MeshChatActivity>(renderer, mappedInput, threadKey, title);
  if (!chat) {
    LOG_ERR("MESH", "OOM: chat screen");
    return;
  }
  // The chat cleared the unread count; the rebuild in the handler shows that
  // in the repaint that follows it.
  startActivityForResult(std::move(chat), [this](const ActivityResult&) { resumeFromChild(); });
}

void MeshActivity::openContacts() {
  auto contacts = makeUniqueNoThrow<MeshContactsActivity>(renderer, mappedInput);
  if (!contacts) {
    LOG_ERR("MESH", "OOM: contact list");
    return;
  }
  // A conversation may have started there.
  startActivityForResult(std::move(contacts), [this](const ActivityResult&) { resumeFromChild(); });
}

void MeshActivity::onRowLongPress(const int index) { showRowActions(index); }

bool MeshActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool MeshActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ACTIONS_HOLD_MS)) {
    showRowActions(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void MeshActivity::showRowActions(const int index) {
  app.clearTapFlash();
  if (!rowsDrawn || optionPopup.isActive() || index < 0 || index >= rowCount) return;
  const RowAction action = rowActions[index];
  const uint8_t target = rowTargets[index];
  if (action == RowAction::OpenChannel && target < MeshService::MAX_CHANNELS) {
    // The service's list can have changed since the rows were drawn: find
    // the channel by the history key the row shows.
    MeshService::ChannelView channel;
    bool found = false;
    for (uint8_t i = 0; i < MESH_SERVICE.channelCount() && !found; i++) {
      found = MESH_SERVICE.channel(i, channel) && strcmp(channel.threadKey, channelKeys[target]) == 0;
    }
    if (!found) return;
    actionIsChannel = true;
    actionSlot = channel.slot;
    memcpy(actionSecret, channel.secret, sizeof(actionSecret));
    snprintf(actionKey, sizeof(actionKey), "%s", channelKeys[target]);
    snprintf(actionName, sizeof(actionName), "%s", channelLabels[target]);
  } else if (action == RowAction::OpenConversation && target < MeshService::MAX_CONVERSATIONS) {
    actionIsChannel = false;
    snprintf(actionKey, sizeof(actionKey), "%s", conversationKeys[target]);
    snprintf(actionName, sizeof(actionName), "%s", conversationLabels[target]);
  } else {
    return;
  }

  // Open, Mark as Read, Clear History, and for a channel Share and Leave.
  const char* options[5] = {tr(STR_OPEN), tr(STR_MESH_MARK_READ), tr(STR_MESH_CLEAR_HISTORY),
                            tr(STR_MESH_SHARE_CHANNEL), tr(STR_MESH_LEAVE_CHANNEL)};
  const int count = actionIsChannel ? 5 : 3;
  {
    RenderLock lock(*this);
    rowsDrawn = false;
    optionPopup.show(actionName, options, count, 0, [this](const int choice) {
      switch (choice) {
        case 0:
          openChat(actionKey, actionName);
          break;
        case 1:
          MESH_SERVICE.markRead(actionKey);
          rowsBuilt = false;
          break;
        case 2:
          pending = Pending::ConfirmClear;
          break;
        case 3:
          pending = Pending::ShowChannelQr;
          break;
        case 4:
          pending = Pending::ConfirmLeave;
          break;
        default:
          break;
      }
    });
  }
  requestUpdate();
}

void MeshActivity::showJoinChoices() {
  if (optionPopup.isActive()) return;
  const char* options[3] = {tr(STR_MESH_PUBLIC_CHANNEL), tr(STR_MESH_HASHTAG_CHANNEL), tr(STR_MESH_PRIVATE_CHANNEL)};
  {
    RenderLock lock(*this);
    rowsDrawn = false;
    optionPopup.show(tr(STR_MESH_JOIN_CHANNEL), options, 3, 0, [this](const int choice) {
      if (choice == 0) {
        if (MESH_SERVICE.hasChannel(MeshService::PUBLIC_CHANNEL_SECRET)) {
          snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_CHANNEL_ALREADY));
          pending = Pending::RequestResult;
        } else if (MESH_SERVICE.joinChannel("Public", MeshService::PUBLIC_CHANNEL_SECRET)) {
          watchRequest(MeshService::Request::JoinChannel, StrId::STR_MESH_CHANNEL_JOINED,
                       StrId::STR_MESH_CHANNEL_NOT_JOINED);
        }
      } else if (choice == 1) {
        pending = Pending::HashtagName;
      } else if (choice == 2) {
        // The secret first: it is what someone joining has in hand.
        pending = Pending::PrivateSecret;
      }
    });
  }
  requestUpdate();
}

void MeshActivity::runPending() {
  const Pending next = pending;
  pending = Pending::None;
  switch (next) {
    case Pending::None:
      return;
    case Pending::ConfirmClear: {
      const char* options[2] = {tr(STR_CANCEL), tr(STR_MESH_CLEAR_HISTORY)};
      RenderLock lock(*this);
      rowsDrawn = false;
      optionPopup.showMessage(actionName, tr(STR_MESH_CLEAR_CONFIRM), options, 2, 0, [this](const int choice) {
        if (choice != 1) return;
        if (!MESH_SERVICE.clearHistory(actionKey)) {
          snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_CLEAR_FAILED));
          pending = Pending::RequestResult;
        }
        rowsBuilt = false;
      });
      break;
    }
    case Pending::ConfirmLeave: {
      const char* options[2] = {tr(STR_CANCEL), tr(STR_MESH_LEAVE_CHANNEL)};
      RenderLock lock(*this);
      rowsDrawn = false;
      optionPopup.showMessage(actionName, tr(STR_MESH_LEAVE_CONFIRM), options, 2, 0, [this](const int choice) {
        if (choice == 1 && MESH_SERVICE.leaveChannel(actionSlot)) {
          watchRequest(MeshService::Request::LeaveChannel, StrId::STR_MESH_CHANNEL_LEFT,
                       StrId::STR_MESH_CHANNEL_NOT_LEFT);
        }
      });
      break;
    }
    case Pending::HashtagName:
      askText(tr(STR_MESH_HASHTAG_CHANNEL), "#", meshc::NAME_FIELD_SIZE - 1, &MeshActivity::joinHashtag);
      return;
    case Pending::PrivateName:
      askText(tr(STR_MESH_CHANNEL_NAME_LIMIT), "", meshc::NAME_FIELD_SIZE - 1, &MeshActivity::choosePrivateName);
      return;
    case Pending::PrivateSecret:
      // Room for the digits with a separator between each pair.
      askText(tr(STR_MESH_CHANNEL_SECRET), "", 3 * meshc::CHANNEL_SECRET_SIZE, &MeshActivity::joinPrivate);
      return;
    case Pending::ShowChannelQr:
      openChannelQr();
      return;
    case Pending::RequestResult:
      showMessage(messageText);
      return;
  }
  requestUpdate();
}

void MeshActivity::askText(const char* title, const char* initial, const size_t maxLen,
                           void (MeshActivity::*done)(const std::string&)) {
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, title, initial, maxLen, InputType::Text);
  if (!keyboard) {
    LOG_ERR("MESH", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, done](const ActivityResult& result) {
    resumeFromChild();
    if (!result.isCancelled) (this->*done)(std::get<KeyboardResult>(result.data).text);
  });
}

void MeshActivity::joinHashtag(const std::string& typed) {
  // "#" and nothing else, or nothing at all: no channel.
  std::string name = typed;
  while (!name.empty() && name.back() == ' ') name.pop_back();
  if (name.empty() || name == "#") return;
  if (name[0] != '#') name.insert(name.begin(), '#');
  uint8_t secret[meshc::CHANNEL_SECRET_SIZE];
  MeshService::hashtagSecret(name.c_str(), secret);
  if (MESH_SERVICE.hasChannel(secret)) {
    snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_CHANNEL_ALREADY));
    pending = Pending::RequestResult;
    return;
  }
  if (MESH_SERVICE.joinChannel(name.c_str(), secret)) {
    watchRequest(MeshService::Request::JoinChannel, StrId::STR_MESH_CHANNEL_JOINED, StrId::STR_MESH_CHANNEL_NOT_JOINED);
  }
}

void MeshActivity::joinPrivate(const std::string& secretHex) {
  // Step one: the secret, or empty for a new channel.
  if (secretHex.empty()) {
    // A new private channel: a random secret, to share by its QR code.
    for (size_t i = 0; i < sizeof(privateSecret); i += 4) {
      const uint32_t r = esp_random();
      memcpy(&privateSecret[i], &r, 4);
    }
  } else if (!parseHexSecret(secretHex, privateSecret)) {
    snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_SECRET_INVALID));
    pending = Pending::RequestResult;
    return;
  }
  if (MESH_SERVICE.hasChannel(privateSecret)) {
    snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_CHANNEL_ALREADY));
    pending = Pending::RequestResult;
    return;
  }
  pending = Pending::PrivateName;
}

void MeshActivity::choosePrivateName(const std::string& name) {
  // Step two: the name; empty keeps the default.
  snprintf(privateName, sizeof(privateName), "%s", name.empty() ? tr(STR_MESH_PRIVATE_CHANNEL) : name.c_str());
  if (MESH_SERVICE.joinChannel(privateName, privateSecret)) {
    watchRequest(MeshService::Request::JoinChannel, StrId::STR_MESH_CHANNEL_JOINED, StrId::STR_MESH_CHANNEL_NOT_JOINED);
  }
}

void MeshActivity::watchRequest(const MeshService::Request kind, const StrId doneText, const StrId failedText) {
  watched = kind;
  watchedDone = doneText;
  watchedFailed = failedText;
  rowsBuilt = false;
}

void MeshActivity::showMessage(const char* text) {
  if (text != messageText) snprintf(messageText, sizeof(messageText), "%s", text);
  const char* options[1] = {tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    rowsDrawn = false;
    optionPopup.showMessage("", messageText, options, 1, 0, [this](int) { rowsBuilt = false; });
  }
  requestUpdate();
}

void MeshActivity::openQr(const char* title, const char* payload, const char* caption) {
  auto qr = makeUniqueNoThrow<MeshQrActivity>(renderer, mappedInput, title, payload, caption);
  if (!qr) {
    LOG_ERR("MESH", "OOM: QR screen");
    return;
  }
  startActivityForResult(std::move(qr), [this](const ActivityResult&) { resumeFromChild(); });
}

void MeshActivity::openChannelQr() {
  char url[meshqr::URL_SIZE];
  if (meshqr::channelUrl(actionName, actionSecret, url, sizeof(url))) {
    openQr(actionName, url, tr(STR_MESH_CHANNEL_QR_HINT));
  }
}

void MeshActivity::buildScreen(UiScreen& screen) {
  meshrow::setListMargins(screen, renderer);

  fui::ListProps props;
  props.items = rowItems;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  // Tap acts, long press offers the row's actions; buttons stay in loop().
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_MESH
