#include <iostream>
#include <mutex>

void diftraywm_log(const char *message) {
  static std::mutex mutex;
  std::lock_guard<std::mutex> lock(mutex);
  if (message && *message) {
    std::clog << "[diftraywm] " << message << '\n';
  }
}
