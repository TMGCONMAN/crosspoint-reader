#include "MeshLink.h"

#if defined(CROSSPOINT_MESH) && CROSSPOINT_MESH

#include <Arduino.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <string>

namespace {

constexpr char NUS_SERVICE_UUID[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
// Named from the radio's side: the app writes to RX and is notified on TX.
constexpr char NUS_RX_UUID[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char NUS_TX_UUID[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char MESHCORE_NAME_PREFIX[] = "MeshCore-";

constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t WORKER_TICK_MS = 50;
constexpr uint32_t TEARDOWN_POLL_MS = 20;
constexpr uint32_t MAX_TEARDOWN_MS = 2000;
constexpr uint32_t WORKER_STACK_BYTES = 4096;
// The radio streams its contact list without pauses, and the main loop can
// be busy for a while with the SD card: 32 frames are about 5.7 KB.
constexpr UBaseType_t RX_QUEUE_DEPTH = 32;
// Connection intervals in 1.25 ms units. Fast while the handshake reads the
// radio's contacts and channels, slower once linked to save power; the
// supervision timeout (10 ms units) stays 8 s so a slow e-ink render does not
// look like a dead link.
constexpr uint16_t FAST_INTERVAL_MIN = 12;
constexpr uint16_t FAST_INTERVAL_MAX = 24;
constexpr uint16_t IDLE_INTERVAL_MIN = 24;
constexpr uint16_t IDLE_INTERVAL_MAX = 40;
constexpr uint16_t SUPERVISION_TIMEOUT = 800;
constexpr UBaseType_t TX_QUEUE_DEPTH = 4;
// Frames are at most 176 bytes; ATT adds a 3-byte header.
constexpr uint16_t PREFERRED_MTU = MeshLink::MAX_FRAME_SIZE + 3;

struct Frame {
  uint8_t len;
  uint8_t data[MeshLink::MAX_FRAME_SIZE];
};

portMUX_TYPE devicesMux = portMUX_INITIALIZER_UNLOCKED;
MeshLinkDevice devices[MeshLink::MAX_DEVICES];
uint8_t deviceTotal = 0;

NimBLEClient* client = nullptr;
// Valid only while state is Ready; cleared by the disconnect callback.
std::atomic<NimBLERemoteCharacteristic*> writeChar{nullptr};
TaskHandle_t workerTask = nullptr;
QueueHandle_t txQueue = nullptr;
QueueHandle_t rxQueue = nullptr;

std::atomic<MeshLinkState> linkState{MeshLinkState::Off};
std::atomic<MeshLinkError> linkError{MeshLinkError::None};
std::atomic<bool> begun{false};
std::atomic<bool> teardownPending{false};
std::atomic<bool> connectRequested{false};
// True while the worker may be inside a blocking NimBLE call.
std::atomic<bool> workerBusy{false};
std::atomic<bool> userDisconnect{false};
std::atomic<bool> disconnectObserved{true};
std::atomic<bool> pinPending{false};
std::atomic<uint32_t> droppedRxFrames{0};

char targetAddr[sizeof(MeshLinkDevice::addr)] = {0};
uint8_t targetAddrType = 0;

void failConnect(MeshLinkError error) {
  linkError.store(userDisconnect.load() ? MeshLinkError::None : error);
  linkState.store(MeshLinkState::Idle);
}

void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  if (len == 0 || len > MeshLink::MAX_FRAME_SIZE || rxQueue == nullptr) return;
  Frame frame;
  frame.len = static_cast<uint8_t>(len);
  memcpy(frame.data, data, len);
  if (xQueueSend(rxQueue, &frame, 0) != pdTRUE) droppedRxFrames.fetch_add(1);
}

bool isCompanion(const NimBLEAdvertisedDevice* dev) {
  if (dev->isAdvertisingService(NimBLEUUID(NUS_SERVICE_UUID))) return true;
  return dev->haveName() && dev->getName().rfind(MESHCORE_NAME_PREFIX, 0) == 0;
}

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (dev == nullptr || !dev->isConnectable() || !isCompanion(dev)) return;
    const std::string addr = dev->getAddress().toString();
    const std::string name = dev->haveName() ? dev->getName() : addr;

    portENTER_CRITICAL(&devicesMux);
    uint8_t slot = 0;
    while (slot < deviceTotal && strncmp(devices[slot].addr, addr.c_str(), sizeof(devices[slot].addr)) != 0) slot++;
    if (slot < MeshLink::MAX_DEVICES) {
      MeshLinkDevice& entry = devices[slot];
      const bool isNew = slot == deviceTotal;
      // A later report without a name must not erase one already stored.
      if (isNew || dev->haveName()) {
        strncpy(entry.name, name.c_str(), sizeof(entry.name) - 1);
        entry.name[sizeof(entry.name) - 1] = 0;
      }
      strncpy(entry.addr, addr.c_str(), sizeof(entry.addr) - 1);
      entry.addr[sizeof(entry.addr) - 1] = 0;
      entry.addrType = dev->getAddress().getType();
      entry.rssi = dev->getRSSI();
      if (isNew) deviceTotal++;
    }
    portEXIT_CRITICAL(&devicesMux);
  }

  void onScanEnd(const NimBLEScanResults&, int) override {
    MeshLinkState expected = MeshLinkState::Scanning;
    linkState.compare_exchange_strong(expected, MeshLinkState::Idle);
  }
};

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int reason) override {
    disconnectObserved.store(true);
    writeChar.store(nullptr);
    pinPending.store(false);
    // While connecting, the worker reports the failure itself.
    MeshLinkState expected = MeshLinkState::Ready;
    if (linkState.compare_exchange_strong(expected, MeshLinkState::Idle)) {
      linkError.store(userDisconnect.load() ? MeshLinkError::None : MeshLinkError::LinkLost);
      LOG_INF("MLINK", "Link down, reason %d", reason);
    }
  }

  // The radio shows or knows the PIN; this side types it in.
  void onPassKeyEntry(NimBLEConnInfo&) override {
    pinPending.store(true);
    linkState.store(MeshLinkState::NeedPin);
  }

  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t) override {
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }
};

ScanCallbacks scanCallbacks;
ClientCallbacks clientCallbacks;

// The disconnect callback ran and NimBLE reports the client DISCONNECTED, so
// deleteClient() frees it now. On a connected client the delete is deferred,
// and a deinit() after that leaks NimBLE's client slot.
bool clientReleased() { return disconnectObserved.load() && NimBLEDevice::getDisconnectedClient() == client; }

// cancelConnect() stops only the GAP connect. Once the link is up the worker
// waits inside pairing or discovery, and only a disconnect ends that wait.
void cancelPendingConnect() {
  if (client == nullptr) return;
  client->cancelConnect();
  if (client->isConnected()) client->disconnect();
}

void doConnect() {
  NimBLEDevice::getScan()->stop();
  xQueueReset(txQueue);
  xQueueReset(rxQueue);
  pinPending.store(false);

  const NimBLEAddress addr(std::string(targetAddr), targetAddrType);
  disconnectObserved.store(false);
  if (!client->connect(addr)) {
    disconnectObserved.store(true);
    LOG_ERR("MLINK", "Connect to %s failed: %d", targetAddr, client->getLastError());
    failConnect(MeshLinkError::ConnectFailed);
    return;
  }

  // The radio only accepts frames on an encrypted, authenticated link.
  const bool secured = client->secureConnection();
  pinPending.store(false);
  if (!secured) {
    LOG_ERR("MLINK", "Pairing with %s failed: %d", targetAddr, client->getLastError());
    // A bond the radio no longer holds (it was reset or re-flashed) fails
    // here every time. Dropping it lets the next attempt pair from scratch.
    if (NimBLEDevice::isBonded(addr)) NimBLEDevice::deleteBond(addr);
    client->disconnect();
    failConnect(MeshLinkError::PairingFailed);
    return;
  }
  linkState.store(MeshLinkState::Connecting);

  NimBLERemoteService* service = client->getService(NimBLEUUID(NUS_SERVICE_UUID));
  NimBLERemoteCharacteristic* notifyChar = service ? service->getCharacteristic(NimBLEUUID(NUS_TX_UUID)) : nullptr;
  NimBLERemoteCharacteristic* rxChar = service ? service->getCharacteristic(NimBLEUUID(NUS_RX_UUID)) : nullptr;
  if (notifyChar == nullptr || rxChar == nullptr || !notifyChar->canNotify() ||
      !notifyChar->subscribe(true, onNotify)) {
    LOG_ERR("MLINK", "%s has no usable companion service", targetAddr);
    client->disconnect();
    failConnect(MeshLinkError::NotCompanion);
    return;
  }

  writeChar.store(rxChar);
  linkError.store(MeshLinkError::None);
  linkState.store(MeshLinkState::Ready);
  // A disconnect that landed before the store above found the state still
  // Connecting and left it alone, so it must be caught here.
  if (!client->isConnected()) {
    writeChar.store(nullptr);
    MeshLinkState expected = MeshLinkState::Ready;
    if (linkState.compare_exchange_strong(expected, MeshLinkState::Idle)) {
      linkError.store(userDisconnect.load() ? MeshLinkError::None : MeshLinkError::LinkLost);
    }
    return;
  }
  LOG_INF("MLINK", "Link ready: %s mtu=%u", targetAddr, client->getMTU());
}

void workerFn(void*) {
  Frame frame;
  for (;;) {
    const bool haveFrame = xQueueReceive(txQueue, &frame, pdMS_TO_TICKS(WORKER_TICK_MS)) == pdTRUE;
    // Set before reading teardownPending: end() then either sees the worker
    // busy and waits, or the worker sees the teardown and does nothing.
    workerBusy.store(true);
    if (!teardownPending.load()) {
      if (connectRequested.exchange(false)) {
        doConnect();
      } else if (haveFrame) {
        NimBLERemoteCharacteristic* chr = writeChar.load();
        if (chr == nullptr || !chr->writeValue(frame.data, frame.len, true)) {
          LOG_ERR("MLINK", "Frame 0x%02x not written", frame.data[0]);
        }
      }
    }
    workerBusy.store(false);
  }
}

void deleteQueues() {
  if (txQueue) vQueueDelete(txQueue);
  if (rxQueue) vQueueDelete(rxQueue);
  txQueue = nullptr;
  rxQueue = nullptr;
}

}  // namespace

MeshLink& MeshLink::getInstance() {
  static MeshLink instance;
  return instance;
}

bool MeshLink::begin(const char* hostName) {
  if (begun.load()) return true;
  // An unfinished end() still owns the worker, the client or the stack.
  if (teardownPending.load()) return false;

  // A deinit that did not complete would make init() return a dead stack.
  if (NimBLEDevice::isInitialized()) {
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (!NimBLEDevice::init(hostName ? hostName : "CrossPoint")) {
    LOG_ERR("MLINK", "NimBLE init failed");
    return false;
  }

  NimBLEDevice::setMTU(PREFERRED_MTU);
  // The companion radio requires bonding with a passkey (MITM protection).
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*sc=*/true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);

  NimBLEScan* scan = NimBLEDevice::getScan();
  // Duplicates stay on: the name often arrives in a later scan response.
  scan->setScanCallbacks(&scanCallbacks, true);
  // onResult() copies what the UI needs, so NimBLE keeps no result list.
  scan->setMaxResults(0);
  scan->setActiveScan(true);
  scan->setInterval(160);
  scan->setWindow(160);

  txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(Frame));
  rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(Frame));
  client = NimBLEDevice::createClient();
  if (txQueue == nullptr || rxQueue == nullptr || client == nullptr) {
    LOG_ERR("MLINK", "Out of memory starting the link");
    if (client) NimBLEDevice::deleteClient(client);
    client = nullptr;
    NimBLEDevice::deinit(true);
    deleteQueues();
    return false;
  }
  client->setConnectTimeout(CONNECT_TIMEOUT_MS);
  client->setConnectionParams(FAST_INTERVAL_MIN, FAST_INTERVAL_MAX, 0, SUPERVISION_TIMEOUT);
  client->setClientCallbacks(&clientCallbacks, false);

  disconnectObserved.store(true);
  connectRequested.store(false);
  userDisconnect.store(false);
  droppedRxFrames.store(0);
  if (xTaskCreate(workerFn, "mesh-link", WORKER_STACK_BYTES, nullptr, 3, &workerTask) != pdPASS) {
    LOG_ERR("MLINK", "Worker task not created");
    NimBLEDevice::deleteClient(client);
    client = nullptr;
    NimBLEDevice::deinit(true);
    deleteQueues();
    return false;
  }

  linkError.store(MeshLinkError::None);
  linkState.store(MeshLinkState::Idle);
  begun.store(true);
  return true;
}

bool MeshLink::end(uint32_t timeoutMs) {
  if (!begun.load() && !teardownPending.load()) return true;
  const uint32_t budgetMs = timeoutMs > MAX_TEARDOWN_MS ? MAX_TEARDOWN_MS : timeoutMs;
  const uint32_t startMs = millis();

  if (begun.exchange(false)) {
    teardownPending.store(true);
    userDisconnect.store(true);
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan && scan->isScanning()) scan->stop();
  }

  // Never delete the worker inside a NimBLE wait: NimBLE would keep a dead
  // task handle. Cancel on every pass, since the link can come up after the
  // first cancel.
  while (workerBusy.load()) {
    cancelPendingConnect();
    if (millis() - startMs >= budgetMs) return false;
    vTaskDelay(pdMS_TO_TICKS(TEARDOWN_POLL_MS));
  }
  if (workerTask) {
    vTaskDelete(workerTask);
    workerTask = nullptr;
  }

  // Close the link and delete the client before deinit; see clientReleased().
  if (client) {
    while (!clientReleased()) {
      if (client->isConnected()) client->disconnect();
      if (millis() - startMs >= budgetMs) return false;
      vTaskDelay(pdMS_TO_TICKS(TEARDOWN_POLL_MS));
    }
    NimBLEDevice::deleteClient(client);
    client = nullptr;
  }

  // Bonds live in NVS and survive this.
  NimBLEDevice::deinit(true);
  if (NimBLEDevice::isInitialized()) return false;

  deleteQueues();
  writeChar.store(nullptr);
  pinPending.store(false);
  portENTER_CRITICAL(&devicesMux);
  deviceTotal = 0;
  portEXIT_CRITICAL(&devicesMux);
  linkState.store(MeshLinkState::Off);
  teardownPending.store(false);
  return true;
}

bool MeshLink::isRunning() const { return begun.load(); }
bool MeshLink::isStopping() const { return teardownPending.load(); }
MeshLinkState MeshLink::state() const { return linkState.load(); }
MeshLinkError MeshLink::lastError() const { return linkError.load(); }

bool MeshLink::startScan(uint32_t durationMs) {
  if (!begun.load()) return false;
  const MeshLinkState current = linkState.load();
  if (current != MeshLinkState::Idle && current != MeshLinkState::Scanning) return false;

  portENTER_CRITICAL(&devicesMux);
  deviceTotal = 0;
  portEXIT_CRITICAL(&devicesMux);
  linkState.store(MeshLinkState::Scanning);
  if (!NimBLEDevice::getScan()->start(durationMs, false, true)) {
    linkState.store(MeshLinkState::Idle);
    return false;
  }
  return true;
}

void MeshLink::stopScan() {
  if (!begun.load()) return;
  NimBLEDevice::getScan()->stop();
  MeshLinkState expected = MeshLinkState::Scanning;
  linkState.compare_exchange_strong(expected, MeshLinkState::Idle);
}

uint8_t MeshLink::deviceCount() const {
  portENTER_CRITICAL(&devicesMux);
  const uint8_t count = deviceTotal;
  portEXIT_CRITICAL(&devicesMux);
  return count;
}

bool MeshLink::device(uint8_t index, MeshLinkDevice& out) const {
  bool found = false;
  portENTER_CRITICAL(&devicesMux);
  if (index < deviceTotal) {
    out = devices[index];
    found = true;
  }
  portEXIT_CRITICAL(&devicesMux);
  return found;
}

bool MeshLink::connect(const char* addr, uint8_t addrType) {
  if (!begun.load() || addr == nullptr) return false;
  const MeshLinkState current = linkState.load();
  if (current != MeshLinkState::Idle && current != MeshLinkState::Scanning) return false;

  strncpy(targetAddr, addr, sizeof(targetAddr) - 1);
  targetAddr[sizeof(targetAddr) - 1] = 0;
  targetAddrType = addrType;
  userDisconnect.store(false);
  linkError.store(MeshLinkError::None);
  linkState.store(MeshLinkState::Connecting);
  connectRequested.store(true);
  return true;
}

void MeshLink::disconnect() {
  if (!begun.load()) return;
  userDisconnect.store(true);
  connectRequested.store(false);
  const MeshLinkState current = linkState.load();
  if (current == MeshLinkState::Connecting || current == MeshLinkState::NeedPin) {
    cancelPendingConnect();
  } else if (client && client->isConnected()) {
    client->disconnect();
  }
}

bool MeshLink::submitPin(uint32_t pin) {
  if (!pinPending.exchange(false)) return false;
  // Pairing runs on the one client, so its connection is the one that asked.
  if (client == nullptr || !client->isConnected()) return false;
  linkState.store(MeshLinkState::Connecting);
  return NimBLEDevice::injectPassKey(client->getConnInfo(), pin);
}

bool MeshLink::forget(const char* addr, uint8_t addrType) {
  if (!begun.load() || addr == nullptr) return false;
  return NimBLEDevice::deleteBond(NimBLEAddress(std::string(addr), addrType));
}

void MeshLink::setFastConnection(const bool fast) {
  if (client == nullptr || linkState.load() != MeshLinkState::Ready) return;
  // A request: the radio may keep its own interval.
  client->updateConnParams(fast ? FAST_INTERVAL_MIN : IDLE_INTERVAL_MIN, fast ? FAST_INTERVAL_MAX : IDLE_INTERVAL_MAX,
                           0, SUPERVISION_TIMEOUT);
}

bool MeshLink::send(const uint8_t* frame, size_t len) {
  if (linkState.load() != MeshLinkState::Ready || txQueue == nullptr) return false;
  if (frame == nullptr || len == 0 || len > MAX_FRAME_SIZE) return false;
  Frame item;
  item.len = static_cast<uint8_t>(len);
  memcpy(item.data, frame, len);
  return xQueueSend(txQueue, &item, 0) == pdTRUE;
}

bool MeshLink::receive(uint8_t* frame, size_t capacity, size_t& len) {
  if (rxQueue == nullptr || frame == nullptr) return false;
  Frame item;
  if (xQueueReceive(rxQueue, &item, 0) != pdTRUE) return false;
  if (item.len > capacity) return false;
  memcpy(frame, item.data, item.len);
  len = item.len;
  return true;
}

#endif  // CROSSPOINT_MESH
