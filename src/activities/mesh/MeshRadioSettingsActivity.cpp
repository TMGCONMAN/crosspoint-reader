#include "MeshRadioSettingsActivity.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "MappedInputManager.h"
#include "MeshMessageRow.h"
#include "activities/util/KeyboardEntryActivity.h"

namespace fui = freeink::ui;

namespace {

// The LoRa bandwidths MeshCore radios accept, in Hz.
constexpr uint32_t BANDWIDTHS_HZ[] = {7800, 10400, 15600, 20800, 31250, 41700, 62500, 125000, 250000, 500000};

// 869525 -> "869.525"; trailing zeros dropped.
void formatThousandths(char* out, size_t size, uint32_t value) {
  const unsigned whole = value / 1000;
  unsigned frac = value % 1000;
  if (frac == 0) {
    snprintf(out, size, "%u", whole);
    return;
  }
  int digits = 3;
  while (frac % 10 == 0) {
    frac /= 10;
    digits--;
  }
  snprintf(out, size, "%u.%0*u", whole, digits, frac);
}

// "12.3456" with up to `places` decimals as an integer scaled by 10^places.
bool parseDecimal(const char* text, int places, int64_t& out) {
  while (*text == ' ') text++;
  bool negative = false;
  if (*text == '-' || *text == '+') negative = *text++ == '-';
  int64_t whole = 0;
  int digits = 0;
  while (*text >= '0' && *text <= '9') {
    whole = whole * 10 + (*text++ - '0');
    if (++digits > 10) return false;
  }
  int64_t frac = 0;
  int fracDigits = 0;
  if (*text == '.') {
    text++;
    while (*text >= '0' && *text <= '9') {
      if (fracDigits < places) {
        frac = frac * 10 + (*text - '0');
        fracDigits++;
      }
      text++;
    }
  }
  if (digits == 0 && fracDigits == 0) return false;
  while (*text == ' ') text++;
  if (*text != 0) return false;
  for (int i = fracDigits; i < places; i++) frac *= 10;
  int64_t scale = 1;
  for (int i = 0; i < places; i++) scale *= 10;
  out = (whole * scale + frac) * (negative ? -1 : 1);
  return true;
}

void formatCoordinate(char* out, size_t size, int32_t valueE6) {
  const bool negative = valueE6 < 0;
  const uint32_t magnitude = negative ? static_cast<uint32_t>(-static_cast<int64_t>(valueE6)) : valueE6;
  snprintf(out, size, "%s%u.%06u", negative ? "-" : "", static_cast<unsigned>(magnitude / 1000000),
           static_cast<unsigned>(magnitude % 1000000));
}

}  // namespace

MeshRadioSettingsActivity::MeshRadioSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("MeshRadioSettings", renderer, mappedInput) {}

const char* MeshRadioSettingsActivity::headerTitle() const { return tr(STR_MESH_RADIO_SETTINGS); }

void MeshRadioSettingsActivity::onEnter() {
  {
    RenderLock lock(*this);
    refreshRows();
  }
  UiListActivity::onEnter();
  shownRevision = MESH_SERVICE.revision();
}

void MeshRadioSettingsActivity::refreshRows() {
  const meshc::SelfInfo& self = MESH_SERVICE.selfInfo();
  static constexpr StrId LABELS[ROW_COUNT] = {StrId::STR_MESH_NAME,      StrId::STR_MESH_TX_POWER,
                                              StrId::STR_MESH_FREQUENCY, StrId::STR_MESH_BANDWIDTH,
                                              StrId::STR_MESH_SPREADING, StrId::STR_MESH_CODING_RATE,
                                              StrId::STR_MESH_POSITION,  StrId::STR_MESH_SHARE_POSITION};
  char number[16];
  for (int row = 0; row < ROW_COUNT; row++) {
    items[row] = fui::ListItem{};
    items[row].label = I18N.get(LABELS[row]);
    items[row].value = values[row];
    items[row].actionValue = static_cast<int16_t>(row);
    values[row][0] = 0;
  }
  snprintf(values[Name], VALUE_SIZE, "%s", self.name);
  snprintf(values[TxPower], VALUE_SIZE, "%d dBm", static_cast<int>(self.txPowerDbm));
  formatThousandths(number, sizeof(number), self.freqKhz);
  snprintf(values[Frequency], VALUE_SIZE, "%s MHz", number);
  formatThousandths(number, sizeof(number), self.bwHz);
  snprintf(values[Bandwidth], VALUE_SIZE, "%s kHz", number);
  snprintf(values[Spreading], VALUE_SIZE, "SF%u", static_cast<unsigned>(self.spreadingFactor));
  snprintf(values[Coding], VALUE_SIZE, "4/%u", static_cast<unsigned>(self.codingRate));
  if (self.latE6 == 0 && self.lonE6 == 0) {
    snprintf(values[Position], VALUE_SIZE, "%s", tr(STR_MESH_NOT_SET));
  } else {
    char lat[16];
    char lon[16];
    formatCoordinate(lat, sizeof(lat), self.latE6);
    formatCoordinate(lon, sizeof(lon), self.lonE6);
    snprintf(values[Position], VALUE_SIZE, "%s, %s", lat, lon);
  }
  snprintf(values[SharePosition], VALUE_SIZE, "%s", self.advertLocPolicy != 0 ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
  items[Name].sectionHeading = tr(STR_MESH_RADIO);
  items[Frequency].sectionHeading = tr(STR_MESH_LORA);
  items[Position].sectionHeading = tr(STR_MESH_POSITION);
  // The frequency and the others below change who this radio can hear.
  items[Frequency].subtitle = tr(STR_MESH_LORA_HINT);
}

void MeshRadioSettingsActivity::loop() {
  if (!optionPopup.isActive()) {
    if (pending == Pending::ConfirmRadio) {
      pending = Pending::None;
      confirmRadio();
    } else if (pending == Pending::Result) {
      pending = Pending::None;
      showMessage(messageText);
    }
  }
  if (watching && !optionPopup.isActive()) {
    const auto state = MESH_SERVICE.requestState(MeshService::Request::Settings);
    if (state != MeshService::RequestState::Busy) {
      watching = false;
      MESH_SERVICE.clearRequest();
      showMessage(state == MeshService::RequestState::Done ? tr(STR_MESH_SETTING_SAVED) : tr(STR_MESH_SETTING_FAILED));
    }
  }
  if (!optionPopup.isActive() && MESH_SERVICE.revision() != shownRevision) {
    shownRevision = MESH_SERVICE.revision();
    {
      RenderLock lock(*this);
      refreshRows();
    }
    requestUpdate();
  }
  UiListActivity::loop();
}

void MeshRadioSettingsActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

bool MeshRadioSettingsActivity::handleCustomInput() {
  return optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
}

void MeshRadioSettingsActivity::activateIndex(const int index) {
  app.clearTapFlash();
  if (optionPopup.isActive() || MESH_SERVICE.phase() != MeshService::Phase::Ready || MESH_SERVICE.requestBusy()) {
    return;
  }
  const meshc::SelfInfo& self = MESH_SERVICE.selfInfo();
  newFreqKhz = self.freqKhz;
  newBwHz = self.bwHz;
  newSf = self.spreadingFactor;
  newCr = self.codingRate;
  char initial[40];
  switch (index) {
    case Name:
      askText(tr(STR_MESH_NAME), self.name, meshc::NAME_FIELD_SIZE - 1, &MeshRadioSettingsActivity::applyName);
      break;
    case TxPower:
      snprintf(initial, sizeof(initial), "%d", static_cast<int>(self.txPowerDbm));
      askText(tr(STR_MESH_TX_POWER), initial, 4, &MeshRadioSettingsActivity::applyTxPower);
      break;
    case Frequency:
      formatThousandths(initial, sizeof(initial), self.freqKhz);
      askText(tr(STR_MESH_FREQUENCY_MHZ), initial, 10, &MeshRadioSettingsActivity::applyFrequency);
      break;
    case Bandwidth:
      chooseBandwidth();
      break;
    case Spreading:
      chooseNumber(tr(STR_MESH_SPREADING), 5, 12, self.spreadingFactor, &newSf);
      break;
    case Coding:
      chooseNumber(tr(STR_MESH_CODING_RATE), 5, 8, self.codingRate, &newCr);
      break;
    case Position: {
      char lat[16];
      char lon[16];
      formatCoordinate(lat, sizeof(lat), self.latE6);
      formatCoordinate(lon, sizeof(lon), self.lonE6);
      snprintf(initial, sizeof(initial), "%s, %s", lat, lon);
      askText(tr(STR_MESH_POSITION_FORMAT), self.latE6 == 0 && self.lonE6 == 0 ? "" : initial, 30,
              &MeshRadioSettingsActivity::applyPosition);
      break;
    }
    case SharePosition:
      startedOrRefused(MESH_SERVICE.setShareLocation(self.advertLocPolicy == 0));
      break;
    default:
      break;
  }
}

void MeshRadioSettingsActivity::askText(const char* title, const char* initial, const size_t maxLen,
                                        void (MeshRadioSettingsActivity::*done)(const std::string&)) {
  auto keyboard =
      makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, title, initial, maxLen, InputType::Text);
  if (!keyboard) {
    LOG_ERR("MRSET", "OOM: keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this, done](const ActivityResult& result) {
    if (!result.isCancelled) (this->*done)(std::get<KeyboardResult>(result.data).text);
  });
}

void MeshRadioSettingsActivity::applyName(const std::string& text) {
  if (text.empty() || text == MESH_SERVICE.selfInfo().name) return;
  startedOrRefused(MESH_SERVICE.setNodeName(text.c_str()));
}

void MeshRadioSettingsActivity::applyTxPower(const std::string& text) {
  int64_t dbm = 0;
  const int maxDbm = MESH_SERVICE.selfInfo().maxTxPowerDbm;
  if (!parseDecimal(text.c_str(), 0, dbm) || dbm < -9 || (maxDbm > 0 && dbm > maxDbm)) {
    snprintf(messageText, sizeof(messageText), tr(STR_MESH_TX_POWER_RANGE), maxDbm);
    pending = Pending::Result;
    return;
  }
  startedOrRefused(MESH_SERVICE.setTxPower(static_cast<int8_t>(dbm)));
}

void MeshRadioSettingsActivity::applyFrequency(const std::string& text) {
  int64_t khz = 0;
  // The LoRa chips MeshCore runs on: sub-GHz (SX126x, SX127x) or 2.4 GHz
  // (SX128x). The radio saves any value, and one outside its chip's band
  // would leave it deaf, so keep to the band it is in now.
  const bool twoGhz = MESH_SERVICE.selfInfo().freqKhz >= 2000000;
  const int64_t low = twoGhz ? 2400000 : 137000;
  const int64_t high = twoGhz ? 2500000 : 1020000;
  if (!parseDecimal(text.c_str(), 3, khz) || khz < low || khz > high) {
    snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_FREQUENCY_RANGE));
    pending = Pending::Result;
    return;
  }
  newFreqKhz = static_cast<uint32_t>(khz);
  pending = Pending::ConfirmRadio;
}

void MeshRadioSettingsActivity::applyPosition(const std::string& text) {
  // "lat, lon", or empty to clear it.
  if (text.empty()) {
    startedOrRefused(MESH_SERVICE.setPosition(0, 0));
    return;
  }
  const size_t comma = text.find(',');
  int64_t lat = 0;
  int64_t lon = 0;
  if (comma == std::string::npos || !parseDecimal(text.substr(0, comma).c_str(), 6, lat) ||
      !parseDecimal(text.substr(comma + 1).c_str(), 6, lon) || lat < -90000000 || lat > 90000000 || lon < -180000000 ||
      lon > 180000000) {
    snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_POSITION_INVALID));
    pending = Pending::Result;
    return;
  }
  startedOrRefused(MESH_SERVICE.setPosition(static_cast<int32_t>(lat), static_cast<int32_t>(lon)));
}

void MeshRadioSettingsActivity::chooseBandwidth() {
  const char* options[std::size(BANDWIDTHS_HZ)];
  int current = 0;
  for (size_t i = 0; i < std::size(BANDWIDTHS_HZ); i++) {
    formatThousandths(optionLabels[i], sizeof(optionLabels[i]), BANDWIDTHS_HZ[i]);
    options[i] = optionLabels[i];
    if (BANDWIDTHS_HZ[i] == newBwHz) current = static_cast<int>(i);
  }
  {
    RenderLock lock(*this);
    optionPopup.show(tr(STR_MESH_BANDWIDTH), options, static_cast<int>(std::size(BANDWIDTHS_HZ)), current,
                     [this](const int choice) {
                       if (choice < 0 || choice >= static_cast<int>(std::size(BANDWIDTHS_HZ))) return;
                       if (BANDWIDTHS_HZ[choice] == newBwHz) return;
                       newBwHz = BANDWIDTHS_HZ[choice];
                       pending = Pending::ConfirmRadio;
                     });
  }
  requestUpdate();
}

void MeshRadioSettingsActivity::chooseNumber(const char* title, const int first, const int last, const int current,
                                             uint8_t* target) {
  const char* options[12];
  int count = 0;
  for (int value = first; value <= last && count < 12; value++, count++) {
    snprintf(optionLabels[count], sizeof(optionLabels[count]), "%d", value);
    options[count] = optionLabels[count];
  }
  {
    RenderLock lock(*this);
    optionPopup.show(title, options, count, current - first, [this, first, target](const int choice) {
      const int value = first + choice;
      if (value == *target) return;
      *target = static_cast<uint8_t>(value);
      pending = Pending::ConfirmRadio;
    });
  }
  requestUpdate();
}

void MeshRadioSettingsActivity::confirmRadio() {
  const char* options[2] = {tr(STR_CANCEL), tr(STR_MESH_CHANGE)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage(tr(STR_MESH_LORA), tr(STR_MESH_LORA_WARNING), options, 2, 0, [this](const int choice) {
      if (choice != 1) return;
      startedOrRefused(MESH_SERVICE.setRadioParams(newFreqKhz, newBwHz, newSf, newCr));
    });
  }
  requestUpdate();
}

void MeshRadioSettingsActivity::startedOrRefused(const bool started) {
  if (started) {
    watching = true;
    return;
  }
  snprintf(messageText, sizeof(messageText), "%s", tr(STR_MESH_SETTING_FAILED));
  pending = Pending::Result;
}

void MeshRadioSettingsActivity::showMessage(const char* text) {
  if (text != messageText) snprintf(messageText, sizeof(messageText), "%s", text);
  const char* options[1] = {tr(STR_OK_BUTTON)};
  {
    RenderLock lock(*this);
    optionPopup.showMessage("", messageText, options, 1, 0, [](int) {});
  }
  requestUpdate();
}

void MeshRadioSettingsActivity::buildScreen(UiScreen& screen) {
  meshrow::setListMargins(screen, renderer);
  fui::ListProps props;
  props.items = items;
  props.count = ROW_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // CROSSPOINT_MESH
