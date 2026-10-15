#include "MeshMessageRow.h"

#if CROSSPOINT_MESH

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>
#include <ctime>

#include "CrossPointSettings.h"
#include "components/UITheme.h"

namespace meshrow {

namespace {

// 2020-01-01. A radio that never had its clock set stamps messages near 0.
constexpr uint32_t MIN_PLAUSIBLE_EPOCH = 1577836800;

// "14:32" for today, "10-09 14:32" for another day, empty when the sender's
// clock was not set.
void formatWhen(char* out, size_t size, uint32_t timestamp, uint32_t nowEpoch) {
  out[0] = 0;
  if (timestamp < MIN_PLAUSIBLE_EPOCH) return;
  const time_t sent = static_cast<time_t>(timestamp);
  const time_t now = static_cast<time_t>(nowEpoch);
  struct tm sentTm{};
  struct tm nowTm{};
  if (localtime_r(&sent, &sentTm) == nullptr || localtime_r(&now, &nowTm) == nullptr) return;

  char clock[12];
  if (SETTINGS.clockFormat == 1) {
    const int hour12 = sentTm.tm_hour % 12 == 0 ? 12 : sentTm.tm_hour % 12;
    snprintf(clock, sizeof(clock), "%d:%02d %s", hour12, sentTm.tm_min, sentTm.tm_hour >= 12 ? "PM" : "AM");
  } else {
    snprintf(clock, sizeof(clock), "%02d:%02d", sentTm.tm_hour, sentTm.tm_min);
  }

  const bool today = sentTm.tm_year == nowTm.tm_year && sentTm.tm_yday == nowTm.tm_yday;
  if (today) {
    snprintf(out, size, "%s", clock);
  } else {
    snprintf(out, size, "%02d-%02d %s", sentTm.tm_mon + 1, sentTm.tm_mday, clock);
  }
}

const char* deliveryText(const meshc::StoredMessage& message, bool direct) {
  if (!message.outgoing) return "";
  switch (message.delivery) {
    case meshc::Delivery::Sending:
      return tr(STR_MESH_SENDING);
    case meshc::Delivery::Failed:
      return tr(STR_MESH_NOT_SENT);
    case meshc::Delivery::Sent:
      // A channel message has no receipt to wait for, so "sent" says nothing.
      return direct ? tr(STR_MESH_SENT) : "";
    case meshc::Delivery::Delivered:
      return tr(STR_MESH_DELIVERED);
    case meshc::Delivery::Unconfirmed:
      return tr(STR_MESH_NOT_CONFIRMED);
    case meshc::Delivery::None:
      break;
  }
  return "";
}

}  // namespace

void formatHeader(const meshc::StoredMessage& message, const char* peerName, uint32_t nowEpoch, char* out,
                  size_t size) {
  char when[24];
  formatWhen(when, sizeof(when), message.timestamp, nowEpoch);

  const bool direct = peerName != nullptr;
  // A room server's posts carry their author; other direct messages are the peer's.
  const char* who =
      message.outgoing ? tr(STR_MESH_YOU) : (message.sender[0] != 0 ? message.sender : (direct ? peerName : ""));

  int used = snprintf(out, size, "%s", who);
  for (const char* part : {static_cast<const char*>(when), deliveryText(message, direct)}) {
    if (part[0] == 0 || used < 0 || static_cast<size_t>(used) >= size) continue;
    used += snprintf(out + used, size - static_cast<size_t>(used), "%s%s", used > 0 ? "  " : "", part);
  }
}

void formatDetails(const meshc::StoredMessage& message, const char* peerName, char* out, const size_t size) {
  size_t used = 0;
  const auto line = [&](const char* format, auto... args) {
    if (used >= size) return;
    const int n = snprintf(out + used, size - used, format, args...);
    if (n > 0) used += static_cast<size_t>(n);
    if (used < size) out[used++] = '\n';
    out[used < size ? used : size - 1] = 0;
  };
  out[0] = 0;

  const char* who = message.outgoing ? tr(STR_MESH_YOU) : (message.sender[0] != 0 ? message.sender : peerName);
  if (who && who[0] != 0) line("%s: %s", tr(STR_MESH_FROM), who);

  if (message.timestamp >= MIN_PLAUSIBLE_EPOCH) {
    const time_t sent = static_cast<time_t>(message.timestamp);
    struct tm sentTm{};
    if (localtime_r(&sent, &sentTm) != nullptr) {
      line("%s: %04d-%02d-%02d %02d:%02d:%02d", tr(STR_MESH_TIME), sentTm.tm_year + 1900, sentTm.tm_mon + 1,
           sentTm.tm_mday, sentTm.tm_hour, sentTm.tm_min, sentTm.tm_sec);
    }
  }

  if (message.outgoing) {
    const char* state = deliveryText(message, true);
    if (state[0] != 0) line("%s: %s", tr(STR_MESH_STATE), state);
  } else {
    if (message.pathLen == meshc::PATH_LEN_DIRECT) {
      line("%s: %s", tr(STR_MESH_ROUTE), tr(STR_MESH_ROUTE_DIRECT_PATH));
    } else if ((message.pathLen & 0x3F) == 0) {
      line("%s: %s", tr(STR_MESH_ROUTE), tr(STR_MESH_ROUTE_HEARD_DIRECTLY));
    } else {
      char hops[24];
      const unsigned hopCount = static_cast<unsigned>(message.pathLen & 0x3F);
      if (hopCount == 1) {
        snprintf(hops, sizeof(hops), "%s", tr(STR_MESH_ONE_HOP));
      } else {
        snprintf(hops, sizeof(hops), tr(STR_MESH_HOPS_FORMAT), hopCount);
      }
      line("%s: %s", tr(STR_MESH_ROUTE), hops);
    }
    if (message.hasSnr) {
      // Quarter dB steps; print with two decimals without float formatting.
      const int quarters = message.snrQuarterDb;
      const int whole = quarters / 4;
      const int frac = (quarters < 0 ? -quarters : quarters) % 4 * 25;
      line("SNR: %s%d.%02d dB", quarters < 0 && whole == 0 ? "-" : "", whole, frac);
    }
  }
  // Drop the last line break.
  if (used > 0 && used <= size && out[used - 1] == '\n') out[used - 1] = 0;
}

void setListMargins(UiAppHost::UiScreen& screen, const GfxRenderer& renderer) {
  namespace fui = freeink::ui;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.buttonHintsHeight),
                  static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

void setMessageRowStyle(UiAppHost::UiScreen& screen, freeink::ui::ListProps& props) {
  // Bold also marks the small style as set on purpose (see textStyleUnset).
  props.labelText = screen.theme().smallText;
  props.labelText.bold = true;
  props.subtitleText = screen.theme().bodyText;
  props.subtitleText.bold = false;
  props.subtitleText.maxLines = MAX_TEXT_LINES;
}

}  // namespace meshrow

#endif  // CROSSPOINT_MESH
