#include <atomic>

namespace {
std::atomic<unsigned long> dirty_regions{0};
}

void diftraywm_damage_tracker_mark_dirty() { ++dirty_regions; }
unsigned long diftraywm_damage_tracker_count() { return dirty_regions.load(); }
