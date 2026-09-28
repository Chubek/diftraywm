#pragma once

#include <array>
#include <string_view>
#include <vector>

class GCursorView;

class CursorArea {
public:
  void dock(GCursorView *view);
  void restore(GCursorView *view);
  void forget(GCursorView *view);
  GCursorView *find_by_id(std::string_view id) const;
  GCursorView *find_by_slot(int slot) const;
  bool assign_slot(GCursorView *view, int slot);
  std::vector<GCursorView *> docked() const;

private:
  std::vector<GCursorView *> docked_;
  std::array<GCursorView *, 4> quick_restore_slots_{};
};
