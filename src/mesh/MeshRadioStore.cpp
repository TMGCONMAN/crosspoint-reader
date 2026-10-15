#include "MeshRadioStore.h"

#if CROSSPOINT_MESH

#include <cstring>

namespace {

void copyText(char* dst, size_t dstSize, const char* src) {
  strncpy(dst, src ? src : "", dstSize - 1);
  dst[dstSize - 1] = 0;
}

}  // namespace

void MeshRadioStore::toJson(JsonDocument& doc) const {
  doc["addr"] = addr;
  doc["addrType"] = addrType;
  doc["name"] = name;
}

bool MeshRadioStore::fromJson(JsonVariantConst doc) {
  copyText(addr, sizeof(addr), doc["addr"] | "");
  addrType = doc["addrType"] | 0;
  copyText(name, sizeof(name), doc["name"] | "");
  return true;
}

void MeshRadioStore::setRadio(const char* newAddr, uint8_t newAddrType, const char* newName) {
  if (newAddr == nullptr || newAddr[0] == 0) return;
  const char* wantedName = newName ? newName : "";
  if (strncmp(addr, newAddr, sizeof(addr)) == 0 && addrType == newAddrType &&
      strncmp(name, wantedName, sizeof(name) - 1) == 0) {
    return;
  }
  copyText(addr, sizeof(addr), newAddr);
  addrType = newAddrType;
  copyText(name, sizeof(name), wantedName);
  saveToFile();
}

void MeshRadioStore::clearRadio() {
  if (!hasRadio()) return;
  addr[0] = 0;
  addrType = 0;
  name[0] = 0;
  saveToFile();
}

#endif  // CROSSPOINT_MESH
