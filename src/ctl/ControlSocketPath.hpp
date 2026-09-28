#pragma once
// Location of the control socket shared by the compositor and diftrayctl.
//
// Both processes derive the same path, so diftrayctl needs no discovery file.
// DIFTRAYWM_CONTROL_SOCKET overrides it, which is what the headless tests use
// to keep concurrent compositor instances from colliding.
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace diftray {
inline std::string control_socket_path() {
  if (const char *configured = std::getenv("DIFTRAYWM_CONTROL_SOCKET")) {
    if (*configured) return configured;
  }
  const char *runtime = std::getenv("XDG_RUNTIME_DIR");
  const std::filesystem::path base =
      runtime && *runtime ? std::filesystem::path(runtime) : std::filesystem::path("/tmp");
  return (base / ("diftraywm-ctl-" + std::to_string(static_cast<long long>(::getuid())) +
                  ".sock")).string();
}
}
