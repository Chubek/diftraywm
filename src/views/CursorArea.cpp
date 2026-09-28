#include "views/CursorArea.hpp"

#include "views/GCursorView.hpp"

#include <algorithm>
#include <array>

void CursorArea::dock(GCursorView *view) {
  if (!view) {
    return;
  }
  if (std::find(docked_.begin(), docked_.end(), view) == docked_.end()) {
    docked_.push_back(view);
    view->dock();
  }
}

void CursorArea::forget(GCursorView *view) {
  if (!view) {
    return;
  }
  docked_.erase(std::remove(docked_.begin(), docked_.end(), view), docked_.end());
  for (auto &slot : quick_restore_slots_) {
    if (slot == view) {
      slot = nullptr;
    }
  }
}

void CursorArea::restore(GCursorView *view) {
  forget(view);
  if (view) view->restore();
}

GCursorView *CursorArea::find_by_id(std::string_view id) const {
  const auto it = std::find_if(docked_.begin(), docked_.end(),
                               [id](const GCursorView *view) { return view && view->word_id() == id; });
  return it == docked_.end() ? nullptr : *it;
}

GCursorView *CursorArea::find_by_slot(int slot) const {
  if (slot < 0 || slot >= static_cast<int>(quick_restore_slots_.size())) {
    return nullptr;
  }
  return quick_restore_slots_[static_cast<std::size_t>(slot)];
}

bool CursorArea::assign_slot(GCursorView *view, int slot) {
  if (!view || slot < 0 || slot >= static_cast<int>(quick_restore_slots_.size())) {
    return false;
  }
  for (auto &assigned : quick_restore_slots_) {
    if (assigned == view) {
      assigned = nullptr;
    }
  }
  if (quick_restore_slots_[static_cast<std::size_t>(slot)] &&
      quick_restore_slots_[static_cast<std::size_t>(slot)] != view) {
    quick_restore_slots_[static_cast<std::size_t>(slot)]->set_quick_restore_slot(std::nullopt);
  }
  quick_restore_slots_[static_cast<std::size_t>(slot)] = view;
  view->set_quick_restore_slot(slot);
  return true;
}

std::vector<GCursorView *> CursorArea::docked() const { return docked_; }
