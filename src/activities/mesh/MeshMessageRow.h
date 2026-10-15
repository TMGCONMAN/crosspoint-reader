#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include <MeshThreadLog.h>

#include <cstddef>
#include <cstdint>

#include "components/UiAppHost.h"

class GfxRenderer;

// Text and layout shared by the Mesh screens.
namespace meshrow {

// Enough lines for the longest message at the narrowest row width, with
// headroom for larger UI fonts.
constexpr uint8_t MAX_TEXT_LINES = 10;
constexpr size_t HEADER_SIZE = 112;

// The line above a message: who, when, and for an outgoing message its
// delivery state when that is worth showing. peerName is the other side of a
// direct conversation, or nullptr in a channel, where each message carries
// its sender. nowEpoch decides whether the date is shown.
void formatHeader(const meshc::StoredMessage& message, const char* peerName, uint32_t nowEpoch, char* out, size_t size);

// The details of one message, one fact a line: who, the full date and time,
// the route and the signal of an incoming message, the state of an outgoing one.
void formatDetails(const meshc::StoredMessage& message, const char* peerName, char* out, size_t size);

// Places the list between the header band and the button hints.
void setListMargins(UiAppHost::UiScreen& screen, const GfxRenderer& renderer);
// Sender and time in the small font above the message in the body font.
void setMessageRowStyle(UiAppHost::UiScreen& screen, freeink::ui::ListProps& props);

}  // namespace meshrow

#endif  // CROSSPOINT_MESH
