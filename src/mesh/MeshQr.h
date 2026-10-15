#pragma once

#include "MeshSupport.h"

#if CROSSPOINT_MESH

#include <cstddef>
#include <cstdint>

// The links the MeshCore apps read from QR codes (docs.meshcore.io/qr_codes).
namespace meshqr {

// Room for a 31-byte name with every byte percent-encoded.
constexpr size_t URL_SIZE = 240;

// meshcore://contact/add?name=...&public_key=<64 hex>&type=<n>. False, with
// out empty, when the link does not fit: a cut key would add the wrong contact.
bool contactUrl(const char* name, const uint8_t* publicKey, uint8_t type, char* out, size_t size);
// meshcore://channel/add?name=...&secret=<32 hex>
bool channelUrl(const char* name, const uint8_t* secret, char* out, size_t size);

}  // namespace meshqr

#endif  // CROSSPOINT_MESH
