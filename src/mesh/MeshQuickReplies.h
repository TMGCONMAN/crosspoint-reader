#pragma once

#include "MeshSupport.h"

#if CROSSPOINT_MESH

#include <cstddef>

// Canned answers for the chat's Quick Replies row. They live in a text file
// on the card, one per line, so they can be edited on a computer; the file
// is written with a default set the first time it is missing.
namespace meshquick {

constexpr size_t MAX_REPLIES = 8;
constexpr size_t REPLY_SIZE = 96;  // bytes with the terminator; longer lines are cut
constexpr char FILE_PATH[] = "/.crosspoint/mesh/quick_replies.txt";

// Fills out with up to MAX_REPLIES replies and returns how many.
size_t load(char (*out)[REPLY_SIZE], size_t max);

}  // namespace meshquick

#endif  // CROSSPOINT_MESH
