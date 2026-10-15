#include "MeshServerActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "MeshChatActivity.h"
#include "MeshMessageRow.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "mesh/MeshHistory.h"

namespace fui = freeink::ui;

namespace {

void formatDuration(char* out, size_t size, uint32_t seconds) {
  const unsigned days = seconds / 86400;
  const unsigned hours = (seconds % 86400) / 3600;
  const unsigned minutes = (seconds % 3600) / 60;
  if (days > 0) {
    snprintf(out, size, "%ud %uh", days, hours);
  } else if (hours > 0) {
    snprintf(out, size, "%uh %um", hours, minutes);
  } else {
    snprintf(out, size, "%um %us", minutes, static_cast<unsigned>(seconds % 60));
  }
}

}  // namespace

MeshServerActivity::MeshServerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const uint8_t* publicKey,
                                       const char* name, const uint8_t type)
    : UiListActivity("MeshServer", renderer, mappedInput), type(type) {
  memcpy(this->publicKey, publicKey, sizeof(this->publicKey));
  snprintf(this->name, sizeof(this->name), "%s", name);
}

void MeshServerActivity::onEnter() {
  {
    RenderLock lock(*this);
    refreshRows();
  }
  UiListActivity::onEnter();
}

char* MeshServerActivity::addRow(const char* label, const RowAction action) {
  if (rowCount >= MAX_ROWS) return nullptr;
  fui::ListItem& item = items[rowCount];
  item = fui::ListItem{};
  item.label = label;
  item.actionValue = static_cast<int16_t>(rowCount);
  values[rowCount][0] = 0;
  item.value = values[rowCount];
  actions[rowCount] = action;
  return values[rowCount++];
}

void MeshServerActivity::refreshRows() {
  shownRevision = MESH_SERVICE.revision();
  rowCount = 0;
  bool admin = false;
  const bool loggedIn = MESH_SERVICE.isLoggedIn(publicKey, &admin);
  const bool loginBusy = MESH_SERVICE.requestState(MeshService::Request::Login) == MeshService::RequestState::Busy;
  const bool statusBusy = MESH_SERVICE.requestState(MeshService::Request::Status) == MeshService::RequestState::Busy;

  if (char* value = addRow(loggedIn ? tr(STR_MESH_LOG_IN_AGAIN) : tr(STR_MESH_LOG_IN), RowAction::Login)) {
    snprintf(value, VALUE_SIZE, "%s",
             loginBusy ? tr(STR_MESH_WAITING) : (loggedIn ? (admin ? tr(STR_MESH_ADMIN) : tr(STR_MESH_GUEST)) : ""));
  }
  if (type == meshc::adv::ROOM) addRow(tr(STR_MESH_OPEN_ROOM), RowAction::OpenRoom);
  if (char* value = addRow(tr(STR_MESH_GET_STATUS), RowAction::Status)) {
    if (statusBusy) snprintf(value, VALUE_SIZE, "%s", tr(STR_MESH_WAITING));
  }

  meshc::ServerStatus status;
  if (!MESH_SERVICE.lastStatus(publicKey, status)) return;
  const int first = rowCount;
  if (char* value = addRow(tr(STR_MESH_BATTERY))) {
    snprintf(value, VALUE_SIZE, "%u.%02u V", status.battMilliVolts / 1000, (status.battMilliVolts % 1000) / 10);
  }
  if (char* value = addRow(tr(STR_MESH_UPTIME))) formatDuration(value, VALUE_SIZE, status.upTimeSecs);
  if (char* value = addRow(tr(STR_MESH_NOISE_FLOOR))) snprintf(value, VALUE_SIZE, "%d dBm", status.noiseFloor);
  if (char* value = addRow(tr(STR_MESH_LAST_SIGNAL))) {
    const int q = status.lastSnrQuarterDb;
    snprintf(value, VALUE_SIZE, "%d dBm  SNR %s%d.%02d", status.lastRssi, q < 0 && q / 4 == 0 ? "-" : "", q / 4,
             (q < 0 ? -q : q) % 4 * 25);
  }
  if (char* value = addRow(tr(STR_MESH_PACKETS))) {
    snprintf(value, VALUE_SIZE, "%lu / %lu", static_cast<unsigned long>(status.packetsReceived),
             static_cast<unsigned long>(status.packetsSent));
  }
  if (char* value = addRow(tr(STR_MESH_AIR_TIME))) formatDuration(value, VALUE_SIZE, status.airTimeSecs);
  if (char* value = addRow(tr(STR_MESH_QUEUE))) snprintf(value, VALUE_SIZE, "%u", status.txQueueLen);
  if (first < rowCount) items[first].sectionHeading = tr(STR_MESH_STATUS);
  if (nav.selected >= rowCount) nav.selected = rowCount - 1;
}

void MeshServerActivity::loop() {
  if (watched != MeshService::Request::None && !optionPopup.isActive()) {
    const auto state = MESH_SERVICE.requestState(watched);
    if (state == MeshService::RequestState::Done || state == MeshService::RequestState::Failed) {
      const bool ok = state == MeshService::RequestState::Done;
      const MeshService::Request finished = watched;
      watched = MeshService::Request::None;
      MESH_SERVICE.clearRequest();
      if (finished == MeshService::Request::Login) {
        showMessage(ok ? tr(STR_MESH_LOGGED_IN) : tr(STR_MESH_LOGIN_FAILED));
      } else if (!ok) {
        showMessage(tr(STR_MESH_NO_ANSWER));
      }
    } else if (state == MeshService::RequestState::Idle) {
      watched = MeshService::Request::None;
    }
  }
  if (!optionPopup.isActive() && MESH_SERVICE.revision() != shownRevision) {
    {
      RenderLock lock(*this);
      refreshRows();
    }
    requestUpdate();
  }
  UiListActivity::loop();
}

void MeshServerActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

bool MeshServerActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void MeshServerActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (optionPopup.isActive() || index < 0 || index >= rowCount) return;
  const bool ready = MESH_SERVICE.phase() == MeshService::Phase::Ready;
  switch (actions[index]) {
    case RowAction::Login: {
      if (!ready || MESH_SERVICE.requestBusy()) return;
      auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_MESH_PASSWORD_GUEST), "",
                                                               15, InputType::Password);
      if (!keyboard) return;
      startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
        if (!result.isCancelled) login(std::get<KeyboardResult>(result.data).text);
      });
      return;
    }
    case RowAction::Status:
      // Servers answer only clients they know, a guest included.
      if (!MESH_SERVICE.isLoggedIn(publicKey)) {
        showMessage(tr(STR_MESH_LOG_IN_FIRST));
        return;
      }
      if (ready && MESH_SERVICE.requestStatus(publicKey)) watched = MeshService::Request::Status;
      break;
    case RowAction::OpenRoom:
      openRoom();
      return;
    case RowAction::None:
      return;
  }
  {
    RenderLock lock(*this);
    refreshRows();
  }
  requestUpdate();
}

void MeshServerActivity::login(const std::string& password) {
  if (MESH_SERVICE.login(publicKey, password.c_str())) {
    watched = MeshService::Request::Login;
  } else {
    showMessage(tr(STR_MESH_LOGIN_FAILED));
  }
}

void MeshServerActivity::openRoom() {
  char threadKey[meshhistory::THREAD_KEY_SIZE];
  meshhistory::contactThreadKey(publicKey, threadKey);
  auto chat = makeUniqueNoThrow<MeshChatActivity>(renderer, mappedInput, threadKey, name);
  if (!chat) {
    LOG_ERR("MSRV", "OOM: room chat");
    return;
  }
  startActivityForResult(std::move(chat), [this](const ActivityResult&) {
    RenderLock lock(*this);
    refreshRows();
  });
}

void MeshServerActivity::showMessage(const char* text) {
  snprintf(messageText, sizeof(messageText), "%s", text);
  const char* options[1] = {tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 1, 0, [](int) {});
  }
  requestUpdate();
}

void MeshServerActivity::buildScreen(UiScreen& screen) {
  meshrow::setListMargins(screen, renderer);
  fui::ListProps props;
  props.items = items;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_MESH
