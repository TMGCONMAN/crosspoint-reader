#pragma once

#include <cstddef>
#include <cstdint>

#include "MeshFrames.h"

namespace meshc {

// The radio's out_path_len for a contact it has no route to; it floods then.
constexpr uint8_t OUT_PATH_UNKNOWN = 0xFF;

// What the screens need of one contact: 72 bytes against the 150 of a Contact.
struct ContactEntry {
  uint8_t publicKey[PUB_KEY_SIZE];
  char name[NAME_FIELD_SIZE + 1];
  uint8_t type;         // one of adv::*
  uint8_t flags;        // as the radio stores them; see isFavorite()
  uint8_t outPathLen;   // as the radio stores it; read it with hasRoute() and routeHops()
  uint32_t lastAdvert;  // epoch seconds

  bool isFavorite() const { return (flags & CONTACT_FLAG_FAVORITE) != 0; }
  bool hasRoute() const { return outPathLen != OUT_PATH_UNKNOWN; }
  // Repeaters on the route. The top two bits of the radio's value give the
  // size of each path entry, not a count.
  uint8_t routeHops() const { return outPathLen & 0x3F; }
};

// The radio's contacts, in the order it reported them. The caller owns the
// storage: a heap block sized for the radio on the device, a plain array in
// host tests.
class ContactBook {
 public:
  ContactBook() = default;
  // entries must stay valid until the next setStorage() call.
  void setStorage(ContactEntry* entries, size_t capacity);
  void clear() { used = 0; }

  size_t count() const { return used; }
  size_t capacity() const { return limit; }
  const ContactEntry& at(size_t index) const { return table[index]; }

  enum class Upsert : uint8_t {
    Full,  // new, and the table has no room
    Added,
    Changed,  // name, type, favourite mark or route differ from before
    Same,     // nothing a screen shows changed
  };
  // Adds the contact, or updates the one with the same public key.
  Upsert upsert(const Contact& contact);
  bool remove(const uint8_t* publicKey);
  // The contact whose public key starts with these KEY_PREFIX_SIZE bytes.
  const ContactEntry* findByPrefix(const uint8_t* keyPrefix) const;
  // The first contact whose key starts with these 4 bytes, the length room
  // servers name an author by. Another contact may share them.
  const ContactEntry* findByShortPrefix(const uint8_t* prefix4) const;
  // Records flags the radio has confirmed. False when the contact is unknown.
  bool setFlags(const uint8_t* publicKey, uint8_t flags);

 private:
  ContactEntry* table = nullptr;
  size_t limit = 0;
  size_t used = 0;
};

}  // namespace meshc
