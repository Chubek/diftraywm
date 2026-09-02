#include <atomic>

namespace {
std::atomic<bool> shutdown_requested{false};
}

void diftraywm_request_shutdown() { shutdown_requested.store(true); }
bool diftraywm_shutdown_requested() { return shutdown_requested.load(); }
