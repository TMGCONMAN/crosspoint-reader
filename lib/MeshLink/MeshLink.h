#pragma once

#include <cstddef>
#include <cstdint>

// BLE transport to a MeshCore companion radio: a NimBLE central that pairs
// with one radio and moves whole protocol frames over its Nordic UART service.
//
// Only built when CROSSPOINT_MESH is set (x4pro-mesh environment); every
// other build compiles this library to nothing. This header is NimBLE-free so
// callers do not pull the BLE stack into their translation units.
//
// Threading: NimBLE callbacks and a worker task do the blocking BLE work. The
// public methods are for the main loop and never block on the radio, except
// end(), which waits up to its timeout.

enum class MeshLinkState : uint8_t {
  Off,         // BLE stack not running
  Idle,        // stack up, no link
  Scanning,    // collecting companion radios in range
  Connecting,  // connect, pairing or service discovery in progress
  NeedPin,     // the radio asked for its six-digit PIN; call submitPin()
  Ready,       // frames can be sent and received
};

// Why the last connection attempt or link ended.
enum class MeshLinkError : uint8_t { None, ConnectFailed, PairingFailed, NotCompanion, LinkLost };

struct MeshLinkDevice {
  char addr[18];  // "aa:bb:cc:dd:ee:ff"
  uint8_t addrType;
  char name[32];  // falls back to the address when the radio sent no name
  int8_t rssi;
};

class MeshLink {
 public:
  static constexpr uint8_t MAX_DEVICES = 8;
  static constexpr size_t MAX_FRAME_SIZE = 176;

  static MeshLink& getInstance();

  // Start the BLE stack as a central. Must run at normal CPU frequency: hold
  // a HalPowerManager::Lock from before begin() until end() returns true.
  bool begin(const char* hostName = "CrossPoint");
  // Drop the link and free the BLE stack's RAM. Returns false when a worker
  // is still inside a BLE wait after timeoutMs; call again on a later pass.
  // begin() is refused until end() has returned true.
  bool end(uint32_t timeoutMs = 1000);
  bool isRunning() const;
  bool isStopping() const;

  MeshLinkState state() const;
  MeshLinkError lastError() const;

  // Scan results are companion radios only: devices advertising the Nordic
  // UART service or a known MeshCore name prefix.
  bool startScan(uint32_t durationMs = 8000);
  void stopScan();
  uint8_t deviceCount() const;
  bool device(uint8_t index, MeshLinkDevice& out) const;

  // Asynchronous: watch state() for NeedPin, Ready, or a return to Idle with
  // lastError() set. A bonded radio reconnects without asking for the PIN.
  bool connect(const char* addr, uint8_t addrType);
  void disconnect();
  bool submitPin(uint32_t pin);
  // Delete the stored bond so the next connect() pairs from scratch.
  bool forget(const char* addr, uint8_t addrType);

  // Queue one frame for the radio. False when the link is not Ready or the
  // queue is full; the caller retries later.
  bool send(const uint8_t* frame, size_t len);
  // Asks the radio for a short connection interval (fast transfers) or a
  // longer one (less power). Ready links only.
  void setFastConnection(bool fast);
  // Pop one received frame. Call from the main loop until it returns false.
  bool receive(uint8_t* frame, size_t capacity, size_t& len);

 private:
  MeshLink() = default;
};

#define MESH_LINK MeshLink::getInstance()
