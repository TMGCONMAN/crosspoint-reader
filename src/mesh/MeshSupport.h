#pragma once

// MeshCore companion support is compiled only into builds that set
// CROSSPOINT_MESH=1 (the x4pro-mesh environment). Shared code tests this
// macro so every other build is unchanged.
#ifndef CROSSPOINT_MESH
#define CROSSPOINT_MESH 0
#endif
