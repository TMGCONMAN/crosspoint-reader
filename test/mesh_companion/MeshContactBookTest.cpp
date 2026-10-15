#include <gtest/gtest.h>

#include <cstring>

#include "MeshContactBook.h"

using namespace meshc;
using Upsert = ContactBook::Upsert;

namespace {

Contact contact(const char* name, uint8_t keyFill, uint8_t type = adv::CHAT, int8_t pathLen = -1) {
  Contact c;
  memset(c.publicKey, keyFill, sizeof(c.publicKey));
  strncpy(c.name, name, NAME_FIELD_SIZE);
  c.type = type;
  c.outPathLen = pathLen;
  c.lastAdvert = 1000u + keyFill;
  return c;
}

struct Book {
  ContactEntry storage[3];
  ContactBook book;
  Book() { book.setStorage(storage, 3); }
};

}  // namespace

TEST(MeshContactBook, AddsContactsInOrder) {
  Book b;
  EXPECT_EQ(b.book.upsert(contact("Ana", 0x11)), Upsert::Added);
  EXPECT_EQ(b.book.upsert(contact("Relay 7", 0x22, adv::REPEATER, 2)), Upsert::Added);
  ASSERT_EQ(b.book.count(), 2u);
  EXPECT_STREQ(b.book.at(0).name, "Ana");
  EXPECT_STREQ(b.book.at(1).name, "Relay 7");
  EXPECT_EQ(b.book.at(1).type, adv::REPEATER);
  EXPECT_EQ(b.book.at(1).routeHops(), 2);
  EXPECT_EQ(b.book.at(1).lastAdvert, 1000u + 0x22);
}

TEST(MeshContactBook, SameKeyUpdatesInPlace) {
  Book b;
  b.book.upsert(contact("Ana", 0x11));
  b.book.upsert(contact("Ben", 0x22));
  EXPECT_EQ(b.book.upsert(contact("Ana (new radio)", 0x11, adv::CHAT, 0)), Upsert::Changed);
  ASSERT_EQ(b.book.count(), 2u);
  EXPECT_STREQ(b.book.at(0).name, "Ana (new radio)");
  EXPECT_EQ(b.book.at(0).routeHops(), 0);
}

TEST(MeshContactBook, AnAdvertThatChangesNothingShownIsReportedAsSame) {
  Book b;
  b.book.upsert(contact("Ana", 0x11, adv::CHAT, 2));
  Contact heardAgain = contact("Ana", 0x11, adv::CHAT, 2);
  heardAgain.lastAdvert = 999999;
  EXPECT_EQ(b.book.upsert(heardAgain), Upsert::Same);
  EXPECT_EQ(b.book.at(0).lastAdvert, 999999u);

  Contact rerouted = contact("Ana", 0x11, adv::CHAT, 3);
  EXPECT_EQ(b.book.upsert(rerouted), Upsert::Changed);
}

TEST(MeshContactBook, RouteIsReadFromTheRadiosPathLength) {
  Book b;
  b.book.upsert(contact("No route", 1, adv::CHAT, -1));  // 0xFF on the radio
  b.book.upsert(contact("Direct", 2, adv::CHAT, 0));
  b.book.upsert(contact("Two hops, 2-byte entries", 3, adv::CHAT, 0x42));
  EXPECT_FALSE(b.book.at(0).hasRoute());
  EXPECT_TRUE(b.book.at(1).hasRoute());
  EXPECT_EQ(b.book.at(1).routeHops(), 0);
  EXPECT_TRUE(b.book.at(2).hasRoute());
  EXPECT_EQ(b.book.at(2).routeHops(), 2);
}

TEST(MeshContactBook, FullTableRefusesNewButUpdatesKnown) {
  Book b;
  b.book.upsert(contact("A", 1));
  b.book.upsert(contact("B", 2));
  b.book.upsert(contact("C", 3));
  EXPECT_EQ(b.book.upsert(contact("D", 4)), Upsert::Full);
  EXPECT_EQ(b.book.upsert(contact("B2", 2)), Upsert::Changed);
  EXPECT_EQ(b.book.count(), 3u);
  EXPECT_STREQ(b.book.at(1).name, "B2");
}

TEST(MeshContactBook, RemoveClosesTheGap) {
  Book b;
  const Contact a = contact("A", 1);
  const Contact mid = contact("B", 2);
  b.book.upsert(a);
  b.book.upsert(mid);
  b.book.upsert(contact("C", 3));
  EXPECT_TRUE(b.book.remove(mid.publicKey));
  EXPECT_FALSE(b.book.remove(mid.publicKey));
  ASSERT_EQ(b.book.count(), 2u);
  EXPECT_STREQ(b.book.at(1).name, "C");
  EXPECT_EQ(b.book.upsert(contact("D", 4)), Upsert::Added);
}

TEST(MeshContactBook, FindsByKeyPrefix) {
  Book b;
  Contact mixed = contact("Mixed", 0x55);
  mixed.publicKey[KEY_PREFIX_SIZE] = 0x99;  // differs after the prefix only
  b.book.upsert(contact("Ana", 0x11));
  b.book.upsert(mixed);

  const uint8_t prefix[KEY_PREFIX_SIZE] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x55};
  const ContactEntry* found = b.book.findByPrefix(prefix);
  ASSERT_NE(found, nullptr);
  EXPECT_STREQ(found->name, "Mixed");

  const uint8_t unknown[KEY_PREFIX_SIZE] = {0x55, 0x55, 0x55, 0x55, 0x55, 0x56};
  EXPECT_EQ(b.book.findByPrefix(unknown), nullptr);
}

TEST(MeshContactBook, LongNameIsCutAndTerminated) {
  Book b;
  Contact c = contact("", 1);
  memset(c.name, 'x', NAME_FIELD_SIZE);
  c.name[NAME_FIELD_SIZE] = 0;
  b.book.upsert(c);
  EXPECT_EQ(strlen(b.book.at(0).name), NAME_FIELD_SIZE);
}

TEST(MeshContactBook, NoStorageHoldsNothing) {
  ContactBook book;
  EXPECT_EQ(book.upsert(contact("A", 1)), Upsert::Full);
  EXPECT_EQ(book.count(), 0u);
  const uint8_t prefix[KEY_PREFIX_SIZE] = {1, 1, 1, 1, 1, 1};
  EXPECT_EQ(book.findByPrefix(prefix), nullptr);
}

TEST(MeshContactBook, ClearKeepsTheStorage) {
  Book b;
  b.book.upsert(contact("A", 1));
  b.book.clear();
  EXPECT_EQ(b.book.count(), 0u);
  EXPECT_EQ(b.book.capacity(), 3u);
  EXPECT_EQ(b.book.upsert(contact("B", 2)), Upsert::Added);
}

TEST(MeshContactBook, FavouriteMarkIsReadAndCountsAsAChange) {
  Book b;
  Contact ana = contact("Ana", 0x11);
  b.book.upsert(ana);
  EXPECT_FALSE(b.book.at(0).isFavorite());

  ana.flags = 0x4B;  // favourite plus bits this code does not use
  EXPECT_EQ(b.book.upsert(ana), Upsert::Changed);
  EXPECT_TRUE(b.book.at(0).isFavorite());
  EXPECT_EQ(b.book.upsert(ana), Upsert::Same);

  ana.flags = 0x01;  // only bits no screen shows changed
  EXPECT_EQ(b.book.upsert(ana), Upsert::Same);
  EXPECT_EQ(b.book.at(0).flags, 0x01);
}

TEST(MeshContactBook, SetFlagsChangesOnlyTheNamedContact) {
  Book b;
  const Contact ana = contact("Ana", 0x11);
  const Contact ben = contact("Ben", 0x22);
  b.book.upsert(ana);
  b.book.upsert(ben);
  EXPECT_TRUE(b.book.setFlags(ben.publicKey, CONTACT_FLAG_FAVORITE));
  EXPECT_FALSE(b.book.at(0).isFavorite());
  EXPECT_TRUE(b.book.at(1).isFavorite());

  const Contact stranger = contact("?", 0x33);
  EXPECT_FALSE(b.book.setFlags(stranger.publicKey, CONTACT_FLAG_FAVORITE));
}
