#pragma once

#include <string>

// Appends why a home-screen cover failed to /.crosspoint/cover-report.txt:
// the last log lines (the converter's error) and the free memory. For
// devices without a serial cable. At most a few reports a boot.
void reportCoverFailure(const std::string& bookPath, int height, const char* what = "Cover not made");
