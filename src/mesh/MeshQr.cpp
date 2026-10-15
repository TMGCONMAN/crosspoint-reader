#include "MeshQr.h"

#if CROSSPOINT_MESH

#include <MeshFrames.h>

#include <cstdio>

namespace meshqr {

namespace {

// Appends text to out at used, percent-encoding all but unreserved characters.
size_t appendEncoded(char* out, size_t size, size_t used, const char* text) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  for (const char* p = text; p && *p && used + 4 < size; p++) {
    const auto c = static_cast<unsigned char>(*p);
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '_' || c == '.' || c == '~';
    if (plain) {
      out[used++] = static_cast<char>(c);
    } else {
      out[used++] = '%';
      out[used++] = HEX[c >> 4];
      out[used++] = HEX[c & 0x0F];
    }
  }
  out[used] = 0;
  return used;
}

size_t appendHex(char* out, size_t size, size_t used, const uint8_t* bytes, size_t count) {
  static constexpr char HEX[] = "0123456789abcdef";
  for (size_t i = 0; i < count && used + 3 < size; i++) {
    out[used++] = HEX[bytes[i] >> 4];
    out[used++] = HEX[bytes[i] & 0x0F];
  }
  out[used] = 0;
  return used;
}

size_t appendText(char* out, size_t size, size_t used, const char* text) {
  const int n = snprintf(out + used, size - used, "%s", text);
  if (n < 0) return used;
  used += static_cast<size_t>(n);
  return used < size ? used : size - 1;
}

}  // namespace

bool contactUrl(const char* name, const uint8_t* publicKey, const uint8_t type, char* out, const size_t size) {
  if (size == 0) return false;
  out[0] = 0;
  size_t used = appendText(out, size, 0, "meshcore://contact/add?name=");
  used = appendEncoded(out, size, used, name);
  used = appendText(out, size, used, "&public_key=");
  used = appendHex(out, size, used, publicKey, meshc::PUB_KEY_SIZE);
  char tail[12];
  snprintf(tail, sizeof(tail), "&type=%u", static_cast<unsigned>(type));
  used = appendText(out, size, used, tail);
  // A full buffer means something was cut.
  if (used + 1 >= size) {
    out[0] = 0;
    return false;
  }
  return true;
}

bool channelUrl(const char* name, const uint8_t* secret, char* out, const size_t size) {
  if (size == 0) return false;
  out[0] = 0;
  size_t used = appendText(out, size, 0, "meshcore://channel/add?name=");
  used = appendEncoded(out, size, used, name);
  used = appendText(out, size, used, "&secret=");
  used = appendHex(out, size, used, secret, meshc::CHANNEL_SECRET_SIZE);
  if (used + 1 >= size) {
    out[0] = 0;
    return false;
  }
  return true;
}

}  // namespace meshqr

#endif  // CROSSPOINT_MESH
