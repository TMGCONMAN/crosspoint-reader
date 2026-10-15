#include "MeshContactBook.h"

#include <cstring>

namespace meshc {

void ContactBook::setStorage(ContactEntry* entries, size_t capacity) {
  table = entries;
  limit = entries ? capacity : 0;
  used = 0;
}

ContactBook::Upsert ContactBook::upsert(const Contact& contact) {
  ContactEntry* entry = nullptr;
  for (size_t i = 0; i < used && !entry; i++) {
    if (memcmp(table[i].publicKey, contact.publicKey, PUB_KEY_SIZE) == 0) entry = &table[i];
  }
  const auto outPathLen = static_cast<uint8_t>(contact.outPathLen);
  Upsert result = Upsert::Added;
  if (entry) {
    // The other flag bits are permissions no screen shows.
    const bool sameMark = ((entry->flags ^ contact.flags) & CONTACT_FLAG_FAVORITE) == 0;
    const bool same = strncmp(entry->name, contact.name, NAME_FIELD_SIZE) == 0 && entry->type == contact.type &&
                      sameMark && entry->outPathLen == outPathLen;
    result = same ? Upsert::Same : Upsert::Changed;
  } else {
    if (used >= limit) return Upsert::Full;
    entry = &table[used++];
    memcpy(entry->publicKey, contact.publicKey, PUB_KEY_SIZE);
  }
  strncpy(entry->name, contact.name, NAME_FIELD_SIZE);
  entry->name[NAME_FIELD_SIZE] = 0;
  entry->type = contact.type;
  entry->flags = contact.flags;
  entry->outPathLen = outPathLen;
  entry->lastAdvert = contact.lastAdvert;
  return result;
}

bool ContactBook::remove(const uint8_t* publicKey) {
  for (size_t i = 0; i < used; i++) {
    if (memcmp(table[i].publicKey, publicKey, PUB_KEY_SIZE) != 0) continue;
    memmove(&table[i], &table[i + 1], (used - i - 1) * sizeof(ContactEntry));
    used--;
    return true;
  }
  return false;
}

bool ContactBook::setFlags(const uint8_t* publicKey, uint8_t flags) {
  for (size_t i = 0; i < used; i++) {
    if (memcmp(table[i].publicKey, publicKey, PUB_KEY_SIZE) != 0) continue;
    table[i].flags = flags;
    return true;
  }
  return false;
}

const ContactEntry* ContactBook::findByShortPrefix(const uint8_t* prefix4) const {
  if (prefix4 == nullptr) return nullptr;
  for (size_t i = 0; i < used; i++) {
    if (memcmp(table[i].publicKey, prefix4, 4) == 0) return &table[i];
  }
  return nullptr;
}

const ContactEntry* ContactBook::findByPrefix(const uint8_t* keyPrefix) const {
  for (size_t i = 0; i < used; i++) {
    if (memcmp(table[i].publicKey, keyPrefix, KEY_PREFIX_SIZE) == 0) return &table[i];
  }
  return nullptr;
}

}  // namespace meshc
