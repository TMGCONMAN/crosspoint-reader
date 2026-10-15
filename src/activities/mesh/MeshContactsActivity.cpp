#include "MeshContactsActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "MappedInputManager.h"
#include "MeshChatActivity.h"
#include "MeshMessageRow.h"
#include "MeshQrActivity.h"
#include "MeshServerActivity.h"
#include "mesh/MeshHistory.h"
#include "mesh/MeshService.h"

namespace fui = freeink::ui;

namespace {

// Adverts change routes all the time on a busy mesh, and every refresh is an
// e-ink repaint.
constexpr uint32_t REFRESH_INTERVAL_MS = 5000;
constexpr unsigned long ACTIONS_HOLD_MS = 1000;

bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

constexpr double PI_RAD = 3.14159265358979323846;
constexpr double EARTH_RADIUS_KM = 6371.0;

// Great-circle distance and initial bearing between two points in degrees E6.
void distanceAndBearing(int32_t fromLat, int32_t fromLon, int32_t toLat, int32_t toLon, double& km, int& bearing) {
  const double lat1 = fromLat / 1e6 * PI_RAD / 180;
  const double lat2 = toLat / 1e6 * PI_RAD / 180;
  const double dLat = lat2 - lat1;
  const double dLon = (toLon - fromLon) / 1e6 * PI_RAD / 180;
  const double a = sin(dLat / 2) * sin(dLat / 2) + cos(lat1) * cos(lat2) * sin(dLon / 2) * sin(dLon / 2);
  km = 2 * EARTH_RADIUS_KM * atan2(sqrt(a), sqrt(1 - a));
  const double y = sin(dLon) * cos(lat2);
  const double x = cos(lat1) * sin(lat2) - sin(lat1) * cos(lat2) * cos(dLon);
  bearing = static_cast<int>(atan2(y, x) * 180 / PI_RAD + 360.5) % 360;
}

// Five decimals (about a metre), as "-33.86880".
void formatCoordinate(char* out, size_t size, int32_t valueE6) {
  const bool negative = valueE6 < 0;
  const auto magnitude = static_cast<uint32_t>(negative ? -static_cast<int64_t>(valueE6) : valueE6);
  const uint32_t rounded = (magnitude + 5) / 10;  // to five decimals
  snprintf(out, size, "%s%u.%05u", negative ? "-" : "", static_cast<unsigned>(rounded / 100000),
           static_cast<unsigned>(rounded % 100000));
}

const char* compassPoint(int bearing) {
  static constexpr const char* POINTS[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return POINTS[((bearing + 22) % 360) / 45];
}

const char* typeName(uint8_t type) {
  switch (type) {
    case meshc::adv::CHAT:
      return tr(STR_MESH_PERSON);
    case meshc::adv::REPEATER:
      return tr(STR_MESH_REPEATER);
    case meshc::adv::ROOM:
      return tr(STR_MESH_ROOM);
    default:
      return tr(STR_MESH_SENSOR);
  }
}

// People, then rooms, then repeaters, then anything else.
int typeRank(uint8_t type) {
  switch (type) {
    case meshc::adv::CHAT:
      return 0;
    case meshc::adv::ROOM:
      return 1;
    case meshc::adv::REPEATER:
      return 2;
    default:
      return 3;
  }
}

}  // namespace

MeshContactsActivity::MeshContactsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("MeshContacts", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

const char* MeshContactsActivity::headerTitle() const { return tr(STR_MESH_CONTACTS); }

bool MeshContactsActivity::before(const Row& a, const Row& b) {
  if (a.favorite != b.favorite) return a.favorite;
  const int rankA = typeRank(a.type);
  const int rankB = typeRank(b.type);
  if (rankA != rankB) return rankA < rankB;
  const int byName = strcasecmp(a.name, b.name);
  if (byName != 0) return byName < 0;
  return memcmp(a.keyPrefix, b.keyPrefix, sizeof(a.keyPrefix)) < 0;
}

void MeshContactsActivity::onEnter() {
  {
    // A render requested by the screen below may still be running.
    RenderLock lock(*this);
    rowCount = 0;
    // One block for as long as the screen is open: 43 bytes for each
    // contact the radio can hold.
    rowCapacity = MESH_SERVICE.contactCapacity();
    rows = makeUniqueNoThrow<Row[]>(rowCapacity > 0 ? rowCapacity : 1);
  }
  UiListActivity::onEnter();
  if (!rows) {
    LOG_ERR("MCONT", "OOM: contact list");
    finish();
    return;
  }
  // A failure from an earlier visit is no longer news.
  MESH_SERVICE.takeFavoriteFailure();
  refresh();
}

void MeshContactsActivity::onExit() {
  // ActivityManager holds the render lock around onExit().
  rowCount = 0;
  rows.reset();
  UiListActivity::onExit();
}

void MeshContactsActivity::loop() {
  if (awaitingDetails && !optionPopup.isActive()) {
    const auto state = MESH_SERVICE.requestState(MeshService::Request::Details);
    if (state != MeshService::RequestState::Busy) {
      awaitingDetails = false;
      if (state == MeshService::RequestState::Done) showDetails();
      MESH_SERVICE.clearRequest();
    }
  }
  if (favoriteRefused || MESH_SERVICE.takeFavoriteFailure()) {
    favoriteRefused = false;
    awaitingFavorite = false;
    showFavoriteFailure();
  }
  // The radio's answer to a favourite change skips the wait that spaces out
  // ordinary refreshes.
  const bool answered = awaitingFavorite && !MESH_SERVICE.favoriteBusy();
  if (answered) awaitingFavorite = false;
  // Not under an open popup: it is drawn over the rows as they were.
  if (!optionPopup.isActive() && MESH_SERVICE.contactsRevision() != shownRevision &&
      (answered || reached(millis(), nextRefreshMs))) {
    refresh();
  }
  UiListActivity::loop();
}

void MeshContactsActivity::refresh() {
  {
    RenderLock lock(*this);
    rowsDrawn = false;

    uint8_t selectedKey[meshc::KEY_PREFIX_SIZE] = {0};
    const int selected = nav.selected;
    const bool hadSelection = selected >= 0 && static_cast<size_t>(selected) < rowCount;
    if (hadSelection) memcpy(selectedKey, rows[selected].keyPrefix, sizeof(selectedKey));

    // The table grows when the radio lists its contacts after this screen
    // opened, or another radio holds more. Without memory the old rows stay.
    const size_t wanted = MESH_SERVICE.contactCapacity();
    if (wanted > rowCapacity) {
      auto larger = makeUniqueNoThrow<Row[]>(wanted);
      if (larger) {
        rows = std::move(larger);
        rowCapacity = wanted;
      } else {
        LOG_ERR("MCONT", "OOM: %u contacts", static_cast<unsigned>(wanted));
      }
    }

    rowCount = 0;
    meshc::ContactEntry entry;
    const size_t total = MESH_SERVICE.contactCount();
    for (size_t i = 0; i < total && rowCount < rowCapacity; i++) {
      if (!MESH_SERVICE.contact(i, entry)) break;
      Row& row = rows[rowCount++];
      snprintf(row.name, sizeof(row.name), "%s", entry.name);
      memcpy(row.keyPrefix, entry.publicKey, sizeof(row.keyPrefix));
      row.type = entry.type;
      row.favorite = entry.isFavorite();
      row.hasRoute = entry.hasRoute();
      row.routeHops = entry.routeHops();
    }
    std::sort(rows.get(), rows.get() + rowCount, &MeshContactsActivity::before);

    if (hadSelection) {
      for (size_t i = 0; i < rowCount; i++) {
        if (memcmp(rows[i].keyPrefix, selectedKey, sizeof(selectedKey)) != 0) continue;
        if (static_cast<int>(i) != selected) nav.requestSelection(static_cast<int>(i));
        break;
      }
    }
    if (nav.selected >= static_cast<int>(rowCount)) nav.selected = rowCount > 0 ? static_cast<int>(rowCount) - 1 : 0;
  }
  shownRevision = MESH_SERVICE.contactsRevision();
  nextRefreshMs = millis() + REFRESH_INTERVAL_MS;
  requestUpdate();
}

// fui::ListProps::rowProvider. Runs on the render task, which holds the
// render lock, so the rows cannot change under it.
void MeshContactsActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<MeshContactsActivity*>(ctx);
  item.actionValue = static_cast<int16_t>(index);
  if (!self->rows || index >= self->rowCount) return;
  const Row& row = self->rows[index];
  item.label = row.name;
  // The sort puts every favourite first, so each group starts once.
  if (row.favorite && index == 0) item.sectionHeading = tr(STR_MESH_FAVORITES);
  if (!row.favorite && index > 0 && self->rows[index - 1].favorite) {
    item.sectionHeading = tr(STR_MESH_OTHER_CONTACTS);
  }
  switch (row.type) {
    case meshc::adv::CHAT:
      // How a message to this person travels.
      if (!row.hasRoute) {
        item.subtitle = tr(STR_MESH_ROUTE_FLOOD);
      } else if (row.routeHops == 0) {
        item.subtitle = tr(STR_MESH_ROUTE_DIRECT);
      } else {
        snprintf(self->rowSubtitle, sizeof(self->rowSubtitle), tr(STR_MESH_ROUTE_HOPS_FORMAT),
                 static_cast<unsigned>(row.routeHops));
        item.subtitle = self->rowSubtitle;
      }
      break;
    case meshc::adv::REPEATER:
      item.subtitle = tr(STR_MESH_REPEATER);
      break;
    case meshc::adv::ROOM:
      item.subtitle = tr(STR_MESH_ROOM);
      break;
    default:
      break;
  }
}

// The render task holds the render lock until this returns, so a refresh
// cannot slip in between the panel refresh and the flag.
void MeshContactsActivity::render(RenderLock&& lock) {
  // The popup is drawn over the rows already on the panel.
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
  rowsDrawn = true;
}

// Touch only. A tap on rows the panel does not show yet must not move the
// selection either.
void MeshContactsActivity::onRowAction(const fui::ActionEvent& event) {
  if (!rowsDrawn) {
    app.clearTapFlash();
    return;
  }
  UiListActivity::onRowAction(event);
}

void MeshContactsActivity::activateIndex(const int index) {
  // The list changes under a tap that opens a chat or does nothing.
  app.clearTapFlash();
  if (!rowsDrawn || index < 0 || static_cast<size_t>(index) >= rowCount) return;
  openRow(rows[index]);
}

void MeshContactsActivity::openRow(const Row& row) {
  if (row.type == meshc::adv::CHAT) {
    openChat(row);
  } else if (row.type == meshc::adv::REPEATER || row.type == meshc::adv::ROOM) {
    openServer(row);
  }
}

void MeshContactsActivity::openServer(const Row& row) {
  meshc::ContactEntry contact;
  if (!MESH_SERVICE.findContact(row.keyPrefix, contact)) return;
  auto server =
      makeUniqueNoThrow<MeshServerActivity>(renderer, mappedInput, contact.publicKey, contact.name, contact.type);
  if (!server) {
    LOG_ERR("MCONT", "OOM: server screen");
    return;
  }
  startActivityForResult(std::move(server), [this](const ActivityResult&) {
    rowsDrawn = false;
    if (MESH_SERVICE.contactsRevision() != shownRevision) refresh();
  });
}

void MeshContactsActivity::showQr(const Row& row) {
  meshc::ContactEntry contact;
  if (!MESH_SERVICE.findContact(row.keyPrefix, contact)) return;
  char url[meshqr::URL_SIZE];
  if (!meshqr::contactUrl(contact.name, contact.publicKey, contact.type, url, sizeof(url))) return;
  auto qr = makeUniqueNoThrow<MeshQrActivity>(renderer, mappedInput, contact.name, url, tr(STR_MESH_CONTACT_QR_HINT));
  if (!qr) return;
  startActivityForResult(std::move(qr), [this](const ActivityResult&) { rowsDrawn = false; });
}

void MeshContactsActivity::showDetails() {
  meshc::Contact contact;
  if (!MESH_SERVICE.details(contact)) return;
  snprintf(detailsName, sizeof(detailsName), "%s", contact.name);
  size_t used = 0;
  const auto line = [&](const char* format, auto... args) {
    if (used >= sizeof(detailsText)) return;
    const int n = snprintf(detailsText + used, sizeof(detailsText) - used, format, args...);
    if (n > 0) used += static_cast<size_t>(n);
    if (used + 1 < sizeof(detailsText)) {
      detailsText[used++] = '\n';
      detailsText[used] = 0;
    }
  };
  detailsText[0] = 0;
  line("%s: %s", tr(STR_MESH_TYPE), typeName(contact.type));
  if (contact.lastAdvert > 0) {
    const time_t heard = static_cast<time_t>(contact.lastAdvert);
    struct tm heardTm{};
    if (localtime_r(&heard, &heardTm) != nullptr && heardTm.tm_year >= 120) {
      line("%s: %04d-%02d-%02d %02d:%02d", tr(STR_MESH_LAST_ADVERT), heardTm.tm_year + 1900, heardTm.tm_mon + 1,
           heardTm.tm_mday, heardTm.tm_hour, heardTm.tm_min);
    }
  }
  const auto pathLen = static_cast<uint8_t>(contact.outPathLen);
  if (pathLen == meshc::OUT_PATH_UNKNOWN) {
    line("%s: %s", tr(STR_MESH_ROUTE), tr(STR_MESH_ROUTE_FLOOD));
  } else if ((pathLen & 0x3F) == 0) {
    line("%s: %s", tr(STR_MESH_ROUTE), tr(STR_MESH_ROUTE_DIRECT));
  } else {
    char hops[24];
    const unsigned hopCount = static_cast<unsigned>(pathLen & 0x3F);
    if (hopCount == 1) {
      snprintf(hops, sizeof(hops), "%s", tr(STR_MESH_ONE_HOP));
    } else {
      snprintf(hops, sizeof(hops), tr(STR_MESH_HOPS_FORMAT), hopCount);
    }
    line("%s: %s", tr(STR_MESH_ROUTE), hops);
  }
  const bool hasPosition = contact.hasPosition && (contact.latE6 != 0 || contact.lonE6 != 0);
  if (hasPosition) {
    char lat[16];
    char lon[16];
    formatCoordinate(lat, sizeof(lat), contact.latE6);
    formatCoordinate(lon, sizeof(lon), contact.lonE6);
    line("%s: %s, %s", tr(STR_MESH_POSITION), lat, lon);
    const meshc::SelfInfo& self = MESH_SERVICE.selfInfo();
    if (self.latE6 != 0 || self.lonE6 != 0) {
      double km = 0;
      int bearing = 0;
      distanceAndBearing(self.latE6, self.lonE6, contact.latE6, contact.lonE6, km, bearing);
      // Without float printf: hundredths of a km below 10 km, whole km above.
      const auto hundredths = static_cast<unsigned long>(km * 100 + 0.5);
      char distance[24];
      if (hundredths < 1000) {
        snprintf(distance, sizeof(distance), "%lu.%02lu km", hundredths / 100, hundredths % 100);
      } else {
        snprintf(distance, sizeof(distance), "%lu km", (hundredths + 50) / 100);
      }
      line("%s: %s, %d\xc2\xb0 %s", tr(STR_MESH_DISTANCE), distance, bearing, compassPoint(bearing));
    } else {
      line("%s", tr(STR_MESH_SET_OWN_POSITION));
    }
  } else {
    line("%s: %s", tr(STR_MESH_POSITION), tr(STR_MESH_NOT_SHARED));
  }
  if (used > 0 && detailsText[used - 1] == '\n') detailsText[used - 1] = 0;

  const char* options[1] = {tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    rowsDrawn = false;
    optionPopup.showMessage(detailsName, detailsText, options, 1, 0, [](int) {});
  }
  requestUpdate();
}

void MeshContactsActivity::openChat(const Row& row) {
  if (row.type != meshc::adv::CHAT && row.type != meshc::adv::ROOM) return;

  char threadKey[meshhistory::THREAD_KEY_SIZE];
  meshhistory::contactThreadKey(row.keyPrefix, threadKey);
  auto chat = makeUniqueNoThrow<MeshChatActivity>(renderer, mappedInput, threadKey, row.name);
  if (!chat) {
    LOG_ERR("MCONT", "OOM: chat screen");
    return;
  }
  // Contacts may have changed under the chat; show them before the repaint
  // that follows this handler.
  startActivityForResult(std::move(chat), [this](const ActivityResult&) {
    rowsDrawn = false;  // the panel shows the chat until the repaint that follows
    if (MESH_SERVICE.contactsRevision() != shownRevision) refresh();
  });
}

int MeshContactsActivity::findRow(const uint8_t* keyPrefix) const {
  for (size_t i = 0; i < rowCount; i++) {
    if (memcmp(rows[i].keyPrefix, keyPrefix, meshc::KEY_PREFIX_SIZE) == 0) return static_cast<int>(i);
  }
  return -1;
}

// Touch long press; the base class has moved the selection to the row.
void MeshContactsActivity::onRowLongPress(const int index) { showActions(index); }

bool MeshContactsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

bool MeshContactsActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ACTIONS_HOLD_MS)) {
    showActions(nav.selected);
    return true;
  }
  return UiListActivity::handleButtons();
}

void MeshContactsActivity::showActions(const int index) {
  app.clearTapFlash();
  if (!rowsDrawn || optionPopup.isActive() || index < 0 || static_cast<size_t>(index) >= rowCount) return;
  const Row& row = rows[index];
  memcpy(actionKey, row.keyPrefix, sizeof(actionKey));

  // A person opens a chat, a repeater or room its server screen; any
  // contact has details, a QR code and a favourite mark.
  const bool canOpen = row.type == meshc::adv::CHAT || row.type == meshc::adv::REPEATER || row.type == meshc::adv::ROOM;
  const char* options[4];
  int count = 0;
  if (canOpen) options[count++] = row.type == meshc::adv::CHAT ? tr(STR_MESH_OPEN_CHAT) : tr(STR_OPEN);
  options[count++] = tr(STR_MESH_DETAILS);
  options[count++] = tr(STR_MESH_SHOW_QR);
  options[count++] = row.favorite ? tr(STR_MESH_REMOVE_FAVORITE) : tr(STR_MESH_ADD_FAVORITE);

  {
    // The render task reads the popup's text.
    RenderLock lock(*this);
    // The panel shows the popup until the repaint after it closes; a tap in
    // that time must not reach the rows underneath.
    rowsDrawn = false;
    optionPopup.show(row.name, options, count, 0, [this, canOpen](const int choice) {
      const int current = findRow(actionKey);
      if (current < 0) return;
      const Row& chosen = rows[current];
      const int option = canOpen ? choice : choice + 1;
      if (option == 0) {
        openRow(chosen);
      } else if (option == 1) {
        awaitingDetails = MESH_SERVICE.requestDetails(chosen.keyPrefix);
      } else if (option == 2) {
        showQr(chosen);
      } else if (MESH_SERVICE.setFavorite(chosen.keyPrefix, !chosen.favorite)) {
        awaitingFavorite = true;
      } else {
        favoriteRefused = true;
      }
    });
  }
  requestUpdate();
}

void MeshContactsActivity::showFavoriteFailure() {
  const char* options[1] = {tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    rowsDrawn = false;
    optionPopup.showMessage("", tr(STR_MESH_FAVORITE_FAILED), options, 1, 0, [](int) {});
  }
  requestUpdate();
}

void MeshContactsActivity::buildScreen(UiScreen& screen) {
  meshrow::setListMargins(screen, renderer);
  if (rowCount == 0) {
    screen.centeredText(tr(STR_MESH_NO_CONTACTS), screen.theme().bodyText);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &MeshContactsActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(rowCount);
  props.action = ACTION_ROW;
  // Tap opens the chat; long press offers the favourite mark. Physical
  // buttons stay in loop().
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_MESH
