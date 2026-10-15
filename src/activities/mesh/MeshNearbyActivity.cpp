#include "MeshNearbyActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "MeshMessageRow.h"
#include "MeshServerActivity.h"

namespace fui = freeink::ui;

namespace {

// Quarter dB as "5.25"; no float formatting needed.
void formatSnr(char* out, size_t size, int quarters) {
  const int whole = quarters / 4;
  const int frac = (quarters < 0 ? -quarters : quarters) % 4 * 25;
  snprintf(out, size, "%s%d.%02d", quarters < 0 && whole == 0 ? "-" : "", whole, frac);
}

}  // namespace

MeshNearbyActivity::MeshNearbyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("MeshNearby", renderer, mappedInput) {}

const char* MeshNearbyActivity::headerTitle() const { return tr(STR_MESH_NEARBY); }

void MeshNearbyActivity::onEnter() {
  MESH_SERVICE.discoverNearby();
  {
    RenderLock lock(*this);
    refreshRows();
  }
  UiListActivity::onEnter();
}

void MeshNearbyActivity::onExit() {
  MESH_SERVICE.endDiscovery();
  MESH_SERVICE.clearRequest();
  UiListActivity::onExit();
}

void MeshNearbyActivity::refreshRows() {
  const auto state = MESH_SERVICE.requestState(MeshService::Request::Discover);
  shownState = state;
  shownRevision = MESH_SERVICE.revision();
  rowCount = 0;
  fui::ListItem& search = items[rowCount];
  search = fui::ListItem{};
  search.label = state == MeshService::RequestState::Busy ? tr(STR_MESH_SEARCHING) : tr(STR_MESH_SEARCH_AGAIN);
  search.actionValue = 0;
  rowCount++;

  const uint8_t found = MESH_SERVICE.nearbyCount();
  for (uint8_t i = 0; i < found && rowCount < MAX_ROWS; i++) {
    if (!MESH_SERVICE.nearby(i, nodes[i])) break;
    const MeshService::NearbyNode& node = nodes[i];
    fui::ListItem& item = items[rowCount];
    item = fui::ListItem{};
    item.label = node.name;
    item.actionValue = static_cast<int16_t>(rowCount);
    char here[12];
    char there[12];
    formatSnr(here, sizeof(here), node.snrHereQuarterDb);
    formatSnr(there, sizeof(there), node.snrThereQuarterDb);
    // How this radio heard the node / how the node heard this radio.
    snprintf(values[rowCount], VALUE_SIZE, "%s / %s dB", here, there);
    item.value = values[rowCount];
    snprintf(subtitles[rowCount], VALUE_SIZE, "%s  RSSI %d",
             node.type == meshc::adv::ROOM ? tr(STR_MESH_ROOM) : tr(STR_MESH_REPEATER),
             static_cast<int>(node.rssiHere));
    item.subtitle = subtitles[rowCount];
    if (rowCount == 1) item.sectionHeading = tr(STR_MESH_NEARBY_SNR);
    rowCount++;
  }
  if (found == 0 && state != MeshService::RequestState::Busy) {
    fui::ListItem& none = items[rowCount];
    none = fui::ListItem{};
    none.label = tr(STR_MESH_NOTHING_NEARBY);
    none.actionValue = static_cast<int16_t>(rowCount);
    rowCount++;
  }
  if (nav.selected >= rowCount) nav.selected = rowCount - 1;
}

void MeshNearbyActivity::loop() {
  const auto state = MESH_SERVICE.requestState(MeshService::Request::Discover);
  if (state != shownState || MESH_SERVICE.revision() != shownRevision) {
    {
      RenderLock lock(*this);
      refreshRows();
    }
    requestUpdate();
  }
  UiListActivity::loop();
}

void MeshNearbyActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (index == 0) {
    if (MESH_SERVICE.discoverNearby()) {
      RenderLock lock(*this);
      refreshRows();
    }
    requestUpdate();
    return;
  }
  const int node = index - 1;
  if (node < 0 || node >= MESH_SERVICE.nearbyCount()) return;
  meshc::ContactEntry contact;
  // Only a contact can be logged in to; a stranger shows up once it adverts.
  if (!MESH_SERVICE.findContact(nodes[node].keyPrefix, contact)) return;
  // The server screen needs the request slot for its login and status.
  MESH_SERVICE.endDiscovery();
  auto server =
      makeUniqueNoThrow<MeshServerActivity>(renderer, mappedInput, contact.publicKey, contact.name, contact.type);
  if (!server) {
    LOG_ERR("MNEAR", "OOM: server screen");
    return;
  }
  startActivityForResult(std::move(server), [this](const ActivityResult&) {
    RenderLock lock(*this);
    refreshRows();
  });
}

void MeshNearbyActivity::buildScreen(UiScreen& screen) {
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
