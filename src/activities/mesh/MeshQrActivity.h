#pragma once

#include "mesh/MeshSupport.h"

#if CROSSPOINT_MESH

#include "activities/Activity.h"
#include "mesh/MeshQr.h"

// A QR code for the MeshCore apps to scan, with a title and a line saying
// what it is. Any button or tap closes it.
class MeshQrActivity final : public Activity {
 public:
  MeshQrActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* title, const char* payload,
                 const char* caption);
  void loop() override;
  void render(RenderLock&&) override;

 private:
  char title[40] = {0};
  char payload[meshqr::URL_SIZE] = {0};
  char caption[96] = {0};
};

#endif  // CROSSPOINT_MESH
