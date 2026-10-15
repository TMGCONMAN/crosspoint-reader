#pragma once

#include "MeshSupport.h"

#if CROSSPOINT_MESH

#include <PersistableStore.h>

#include <cstdint>

// The companion radio this device last paired with, kept on the SD card so
// the Mesh screen can reconnect without a scan. The BLE bond itself lives in
// NVS; forgetting a radio here does not remove that bond.
class MeshRadioStore : public PersistableStore<MeshRadioStore> {
 public:
  static constexpr size_t ADDR_SIZE = 18;
  static constexpr size_t NAME_SIZE = 33;

  static const char* getFilePath() { return "/.crosspoint/mesh.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool hasRadio() const { return addr[0] != 0; }
  const char* radioAddr() const { return addr; }
  uint8_t radioAddrType() const { return addrType; }
  const char* radioName() const { return name[0] ? name : addr; }

  // Both write the file only when the stored value changes.
  void setRadio(const char* newAddr, uint8_t newAddrType, const char* newName);
  void clearRadio();

 private:
  MeshRadioStore() = default;
  friend class PersistableStore<MeshRadioStore>;

  char addr[ADDR_SIZE] = {0};
  uint8_t addrType = 0;
  char name[NAME_SIZE] = {0};
};

#define MESH_RADIO_STORE MeshRadioStore::getInstance()

#endif  // CROSSPOINT_MESH
