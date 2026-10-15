// MeshService's requests to the radio and to other nodes: radio settings,
// channel slots, logins and status from servers, discovery, contact details.
#include "MeshService.h"

#if CROSSPOINT_MESH

#include <Arduino.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

namespace {

bool reached(uint32_t nowMs, uint32_t deadlineMs) { return static_cast<int32_t>(nowMs - deadlineMs) >= 0; }

constexpr uint32_t SETTINGS_TIMEOUT_MS = 15000;
constexpr uint32_t CHANNEL_TIMEOUT_MS = 15000;
// Until the radio has sent the request and estimated the round trip.
constexpr uint32_t SERVER_TIMEOUT_MS = 30000;
// Added to the radio's estimate: a server that answers takes its time.
constexpr uint32_t SERVER_MARGIN_MS = 10000;
// Repeaters wait a random time before answering, so answers spread out.
constexpr uint32_t DISCOVER_WINDOW_MS = 12000;
constexpr uint32_t DETAILS_TIMEOUT_MS = 10000;
constexpr uint8_t FALLBACK_CHANNEL_SLOTS = 8;

}  // namespace

MeshService::RequestState MeshService::requestState(const Request kind) const {
  if (!session || session->request != kind) return RequestState::Idle;
  return session->requestStatus;
}

void MeshService::clearRequest() {
  if (!session || session->requestStatus == RequestState::Busy) return;
  session->request = Request::None;
  session->requestStatus = RequestState::Idle;
}

bool MeshService::beginRequest(const Request kind, const uint32_t tag, const uint8_t* keyPrefix,
                               const uint32_t timeoutMs) {
  if (!session) return false;
  session->request = kind;
  session->requestStatus = RequestState::Busy;
  session->requestTag = tag;
  session->requestDeadlineMs = millis() + timeoutMs;
  if (keyPrefix) {
    memcpy(session->requestKey, keyPrefix, meshc::KEY_PREFIX_SIZE);
  } else {
    memset(session->requestKey, 0, meshc::KEY_PREFIX_SIZE);
  }
  touch();
  return true;
}

void MeshService::finishRequest(const bool ok) {
  if (!session || session->requestStatus != RequestState::Busy) return;
  session->requestStatus = ok ? RequestState::Done : RequestState::Failed;
  session->requestTag = 0;
  touch();
}

namespace {
bool requestFree(const MeshService& service) { return !service.requestBusy(); }
}  // namespace

// ---- Radio settings ----

bool MeshService::setNodeName(const char* name) {
  if (!session || !requestFree(*this) || name == nullptr || name[0] == 0) return false;
  const uint32_t tag = takeTag();
  return client.setAdvertName(name, tag) && beginRequest(Request::Settings, tag, nullptr, SETTINGS_TIMEOUT_MS);
}

bool MeshService::setTxPower(const int8_t dbm) {
  if (!session || !requestFree(*this)) return false;
  const uint32_t tag = takeTag();
  return client.setTxPower(dbm, tag) && beginRequest(Request::Settings, tag, nullptr, SETTINGS_TIMEOUT_MS);
}

bool MeshService::setRadioParams(const uint32_t freqKhz, const uint32_t bwHz, const uint8_t spreadingFactor,
                                 const uint8_t codingRate) {
  if (!session || !requestFree(*this)) return false;
  const uint32_t tag = takeTag();
  return client.setRadioParams(freqKhz, bwHz, spreadingFactor, codingRate, tag) &&
         beginRequest(Request::Settings, tag, nullptr, SETTINGS_TIMEOUT_MS);
}

bool MeshService::setPosition(const int32_t latE6, const int32_t lonE6) {
  if (!session || !requestFree(*this)) return false;
  const uint32_t tag = takeTag();
  return client.setAdvertPosition(latE6, lonE6, tag) &&
         beginRequest(Request::Settings, tag, nullptr, SETTINGS_TIMEOUT_MS);
}

bool MeshService::setShareLocation(const bool share) {
  if (!session || !requestFree(*this)) return false;
  meshc::SelfInfo self = client.selfInfo();
  self.advertLocPolicy = share ? 1 : 0;
  const uint32_t tag = takeTag();
  return client.setOtherParams(self, tag) && beginRequest(Request::Settings, tag, nullptr, SETTINGS_TIMEOUT_MS);
}

// ---- Channel slots ----

uint8_t MeshService::channelSlots() const {
  const uint8_t reported = client.deviceInfo().maxChannels;
  const uint8_t slots = reported ? reported : FALLBACK_CHANNEL_SLOTS;
  return slots < MAX_CHANNELS ? slots : MAX_CHANNELS;
}

const uint8_t MeshService::PUBLIC_CHANNEL_SECRET[meshc::CHANNEL_SECRET_SIZE] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

void MeshService::hashtagSecret(const char* name, uint8_t* out) {
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(name), strlen(name), digest, 0);
  memcpy(out, digest, meshc::CHANNEL_SECRET_SIZE);
}

bool MeshService::hasChannel(const uint8_t* secret) const {
  if (!session) return false;
  for (uint8_t i = 0; i < session->channelCount; i++) {
    const ChannelEntry& entry = session->channels[i];
    if (entry.live && memcmp(entry.secret, secret, meshc::CHANNEL_SECRET_SIZE) == 0) return true;
  }
  return false;
}

int MeshService::freeChannelSlot() const {
  if (!session) return -1;
  for (uint8_t slot = 0; slot < channelSlots(); slot++) {
    bool used = false;
    for (uint8_t i = 0; i < session->channelCount && !used; i++) {
      used = session->channels[i].live && session->channels[i].slot == slot;
    }
    if (!used) return slot;
  }
  return -1;
}

bool MeshService::joinChannel(const char* name, const uint8_t* secret) {
  if (!session || !requestFree(*this) || name == nullptr || name[0] == 0 || secret == nullptr) return false;
  const int slot = freeChannelSlot();
  if (slot < 0) return false;
  meshc::Channel channel;
  channel.index = static_cast<uint8_t>(slot);
  snprintf(channel.name, sizeof(channel.name), "%s", name);
  memcpy(channel.secret, secret, sizeof(channel.secret));
  const uint32_t tag = takeTag();
  if (!client.setChannel(channel, tag) || !beginRequest(Request::JoinChannel, tag, nullptr, CHANNEL_TIMEOUT_MS)) {
    return false;
  }
  session->requestSlot = channel.index;
  return true;
}

bool MeshService::leaveChannel(const uint8_t slot) {
  if (!session || !requestFree(*this)) return false;
  meshc::Channel channel;  // empty name, zero secret: a free slot
  channel.index = slot;
  const uint32_t tag = takeTag();
  if (!client.setChannel(channel, tag) || !beginRequest(Request::LeaveChannel, tag, nullptr, CHANNEL_TIMEOUT_MS)) {
    return false;
  }
  session->requestSlot = slot;
  return true;
}

void MeshService::onChannelEmpty(const uint8_t index) {
  if (!session) return;
  for (uint8_t i = 0; i < session->channelCount; i++) {
    if (session->channels[i].live && session->channels[i].slot == index) session->channels[i].live = false;
  }
  dropStaleChannels();
  requestChannelRead(index, false);
  touch();
}

void MeshService::requestChannelRead(const uint8_t slot, const bool present) {
  if (!session || session->requestStatus != RequestState::Busy || session->requestTag != 0) return;
  if (slot != session->requestSlot) return;
  if (session->request == Request::JoinChannel) finishRequest(present);
  if (session->request == Request::LeaveChannel) finishRequest(!present);
}

// ---- Servers ----

bool MeshService::login(const uint8_t* keyPrefix, const char* password) {
  if (!session || !requestFree(*this)) return false;
  const meshc::ContactEntry* entry = session->contacts.findByPrefix(keyPrefix);
  if (!entry) return false;
  const uint32_t tag = takeTag();
  return client.login(entry->publicKey, password ? password : "", tag) &&
         beginRequest(Request::Login, tag, keyPrefix, SERVER_TIMEOUT_MS);
}

bool MeshService::isLoggedIn(const uint8_t* keyPrefix, bool* isAdmin) const {
  if (!session) return false;
  for (uint8_t i = 0; i < session->loginCount; i++) {
    if (memcmp(session->loginKeys[i], keyPrefix, meshc::KEY_PREFIX_SIZE) != 0) continue;
    if (isAdmin) *isAdmin = session->loginAdmin[i];
    return true;
  }
  return false;
}

bool MeshService::requestStatus(const uint8_t* keyPrefix) {
  if (!session || !requestFree(*this)) return false;
  const meshc::ContactEntry* entry = session->contacts.findByPrefix(keyPrefix);
  if (!entry) return false;
  const uint32_t tag = takeTag();
  return client.requestStatus(entry->publicKey, tag) &&
         beginRequest(Request::Status, tag, keyPrefix, SERVER_TIMEOUT_MS);
}

bool MeshService::lastStatus(const uint8_t* keyPrefix, meshc::ServerStatus& out) const {
  if (!session || !session->hasStatus) return false;
  if (memcmp(session->status.keyPrefix, keyPrefix, meshc::KEY_PREFIX_SIZE) != 0) return false;
  out = session->status;
  return true;
}

void MeshService::onRequestSent(uint8_t, const uint32_t tag, const meshc::SentInfo& info) {
  if (!session || session->requestStatus != RequestState::Busy || tag != session->requestTag) return;
  // The answer comes as a push with no tag, matched by the server's key.
  if (info.timeoutMs > 0) session->requestDeadlineMs = millis() + info.timeoutMs + SERVER_MARGIN_MS;
}

void MeshService::onLoginResult(const meshc::LoginResult& result) {
  if (!session || session->request != Request::Login || session->requestStatus != RequestState::Busy) return;
  if (memcmp(result.keyPrefix, session->requestKey, meshc::KEY_PREFIX_SIZE) != 0) return;
  if (result.success) {
    uint8_t at = 0;
    while (at < session->loginCount && memcmp(session->loginKeys[at], result.keyPrefix, meshc::KEY_PREFIX_SIZE) != 0) {
      at++;
    }
    if (at == session->loginCount) {
      // Full: the oldest login gives way.
      if (session->loginCount == Session::MAX_LOGINS) {
        memmove(session->loginKeys[0], session->loginKeys[1], (Session::MAX_LOGINS - 1) * meshc::KEY_PREFIX_SIZE);
        memmove(&session->loginAdmin[0], &session->loginAdmin[1], Session::MAX_LOGINS - 1);
        at = Session::MAX_LOGINS - 1;
      } else {
        session->loginCount++;
      }
    }
    memcpy(session->loginKeys[at], result.keyPrefix, meshc::KEY_PREFIX_SIZE);
    session->loginAdmin[at] = result.isAdmin;
  }
  finishRequest(result.success);
}

void MeshService::onServerStatus(const meshc::ServerStatus& status) {
  if (!session || session->request != Request::Status || session->requestStatus != RequestState::Busy) return;
  if (memcmp(status.keyPrefix, session->requestKey, meshc::KEY_PREFIX_SIZE) != 0) return;
  session->status = status;
  session->hasStatus = true;
  finishRequest(true);
}

// ---- Discovery ----

bool MeshService::discoverNearby() {
  if (!session || !requestFree(*this) || !canDiscover()) return false;
  // The tag only needs to tell this round's answers from a late one's.
  const uint32_t tag = takeTag();
  if (!client.discover(meshc::DISCOVER_FILTER_REPEATER | meshc::DISCOVER_FILTER_ROOM, tag)) return false;
  session->nearbyCount = 0;
  return beginRequest(Request::Discover, tag, nullptr, DISCOVER_WINDOW_MS);
}

void MeshService::endDiscovery() {
  if (session && session->request == Request::Discover && session->requestStatus == RequestState::Busy) {
    finishRequest(true);
  }
}

uint8_t MeshService::nearbyCount() const { return session ? session->nearbyCount : 0; }

bool MeshService::nearby(const uint8_t index, NearbyNode& out) const {
  if (!session || index >= session->nearbyCount) return false;
  out = session->nearby[index];
  return true;
}

void MeshService::onDiscoverReply(const meshc::DiscoverReply& reply) {
  if (!session || session->request != Request::Discover || session->requestStatus != RequestState::Busy) return;
  if (reply.tag != session->requestTag) return;
  if (memcmp(reply.publicKey, client.selfInfo().publicKey, meshc::PUB_KEY_SIZE) == 0) return;
  uint8_t at = 0;
  while (at < session->nearbyCount && memcmp(session->nearby[at].keyPrefix, reply.publicKey, meshc::KEY_PREFIX_SIZE)) {
    at++;
  }
  if (at == session->nearbyCount) {
    if (session->nearbyCount == MAX_NEARBY) return;
    session->nearbyCount++;
  }
  NearbyNode& node = session->nearby[at];
  memcpy(node.keyPrefix, reply.publicKey, meshc::KEY_PREFIX_SIZE);
  node.type = reply.nodeType;
  node.snrHereQuarterDb = reply.snrHereQuarterDb;
  node.snrThereQuarterDb = reply.snrThereQuarterDb;
  node.rssiHere = reply.rssiHere;
  if (const meshc::ContactEntry* known = session->contacts.findByPrefix(reply.publicKey)) {
    snprintf(node.name, sizeof(node.name), "%s", known->name);
  } else {
    snprintf(node.name, sizeof(node.name), "%02x%02x%02x%02x%02x%02x", reply.publicKey[0], reply.publicKey[1],
             reply.publicKey[2], reply.publicKey[3], reply.publicKey[4], reply.publicKey[5]);
  }
  touch();
}

// ---- Contact details ----

bool MeshService::requestDetails(const uint8_t* keyPrefix) {
  if (!session || !requestFree(*this) || session->favoriteTag != 0) return false;
  const meshc::ContactEntry* entry = session->contacts.findByPrefix(keyPrefix);
  if (!entry) return false;
  session->hasDetails = false;
  const uint32_t tag = takeTag();
  return client.requestContact(entry->publicKey, tag) &&
         beginRequest(Request::Details, tag, keyPrefix, DETAILS_TIMEOUT_MS);
}

bool MeshService::details(meshc::Contact& out) const {
  if (!session || !session->hasDetails) return false;
  out = session->details;
  return true;
}

// ---- Bookkeeping ----

bool MeshService::requestContactFetched(const uint32_t tag, const meshc::Contact& contact) {
  if (!session || tag == 0 || tag != session->requestTag || session->request != Request::Details) return false;
  session->details = contact;
  session->hasDetails = true;
  finishRequest(true);
  return true;
}

bool MeshService::requestCommandDone(const uint8_t cmd, const uint32_t tag) {
  if (!session || tag == 0 || tag != session->requestTag || session->requestStatus != RequestState::Busy) {
    return false;
  }
  switch (session->request) {
    case Request::Settings:
      // Done once the radio has reported itself again (onSelfInfo).
      session->requestTag = 0;
      if (!client.refreshSelfInfo()) finishRequest(true);
      break;
    case Request::JoinChannel:
    case Request::LeaveChannel:
      // Read the slot back: the radio may refuse a slot without an error.
      session->requestTag = 0;
      if (!client.readChannel(session->requestSlot)) finishRequest(false);
      break;
    case Request::Discover:
      break;  // answers gather until the window closes
    default:
      LOG_ERR("MESH", "Unexpected answer to command %u", static_cast<unsigned>(cmd));
      break;
  }
  return true;
}

bool MeshService::requestCommandFailed(const uint32_t tag) {
  if (!session || tag == 0 || tag != session->requestTag || session->requestStatus != RequestState::Busy) {
    return false;
  }
  finishRequest(false);
  return true;
}

void MeshService::pollRequest(const uint32_t nowMs) {
  if (!session || session->requestStatus != RequestState::Busy || !reached(nowMs, session->requestDeadlineMs)) return;
  // A discovery round ends with whatever answered; anything else timed out.
  finishRequest(session->request == Request::Discover);
}

#endif  // CROSSPOINT_MESH
