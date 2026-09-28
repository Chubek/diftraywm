#include "compositor/Compositor.hpp"
#include "views/Cell.hpp"
#include "views/NCursorView.hpp"
#include "views/GCursorView.hpp"
#include "views/TCursorView.hpp"
#include "nterm/NTerm.hpp"

#include <sstream>

std::string Compositor::list_views() const {
  std::ostringstream out;
  for (const auto &view : ncursor_views_) {
    out << view->id() << " output=" << view->output_name() << " workspace=" << view->workspace();
    for (const auto &stack : view->cell_stacks())
      for (auto *cell : stack.cells) out << " cell=" << cell->id();
    out << '\n';
  }
  return out.str();
}

std::string Compositor::move_cell(const std::string &id, const std::string &destination) {
  Cell *cell = nullptr;
  NCursorView *target = nullptr;
  for (auto &candidate : cells_) if (candidate->id() == id) cell = candidate.get();
  for (auto &candidate : ncursor_views_) if (candidate->id() == destination) target = candidate.get();
  if (!cell || !target) return "cell move failed: unknown cell or ncursor";
  auto *source = owner_ncursor(cell);
  if (!source) return "cell move failed: cell has no owner";
  if (source == target) return "cell already in ncursor " + destination;
  if (cell->state() == CellState::TCURSOR) return "restore the tcursor before moving its cell";
  // Every NCursor remains usable after its final cell is moved.
  size_t count = 0;
  for (const auto &stack : source->cell_stacks()) count += stack.cells.size();
  if (count == 1) {
    auto replacement = std::make_unique<Cell>(source->default_shell().empty() ? config_.shell : source->default_shell());
    auto *raw = replacement.get();
    source->insert_cell(raw, false);
    cells_.push_back(std::move(replacement));
    attach_cell_surface(raw);
    if (display_) { raw->nterm()->start(); watch_cell_pty(raw); }
  }
  source->remove_cell(cell);
  target->insert_cell(cell, false);
  for (auto &cursor : gcursors_) {
    if (cursor->owner_cell() != cell) continue;
    cursor->set_owner_ncursor(target);
    cursor->set_workspace(target->workspace());
    cursor->set_output_name(target->output_name());
  }
  request_notelet(cell, "", "move");
  relayout();
  return "moved cell " + id + " to " + destination;
}

std::string Compositor::move_cursor(const std::string &id, const std::string &kind,
                                    const std::string &destination) {
  auto *cursor = find_gcursor(id);
  if (!cursor) return "cursor move failed: unknown cursor";
  Cell *cell = nullptr;
  NCursorView *target = nullptr;
  if (kind == "cell") {
    for (auto &candidate : cells_) if (candidate->id() == destination) cell = candidate.get();
    target = owner_ncursor(cell);
  } else if (kind == "ncursor") {
    for (auto &candidate : ncursor_views_) if (candidate->id() == destination) target = candidate.get();
  }
  if (!target) return "cursor move failed: unknown destination";
  for (auto &candidate : cells_) candidate->cursor_area().forget(cursor);
  for (auto &view : ncursor_views_) view->cursor_area().forget(cursor);
  cursor->set_owner_cell(cell);
  cursor->set_owner_ncursor(target);
  cursor->set_workspace(target->workspace());
  cursor->set_output_name(target->output_name());
  if (cursor->docked()) {
    if (cell) cell->cursor_area().dock(cursor);
    else target->cursor_area().dock(cursor);
  }
  if (active_view_ == cursor) active_view_ = active_ncursor_;
  relayout();
  return "moved cursor " + id + " to " + destination;
}
