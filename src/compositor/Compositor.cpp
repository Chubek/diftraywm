#include "compositor/Compositor.hpp"

#include "nterm/NTerm.hpp"
#include "plugin/PluginManager.hpp"
#include "theme/ThemeEngine.hpp"
#include "lua/LuaEngine.hpp"
#include "views/Cell.hpp"
#include "views/GCursorView.hpp"
#include "views/NCursorView.hpp"
#include "views/TCursorView.hpp"
#include "views/View.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <sstream>

Compositor::Compositor() = default;
Compositor::~Compositor() = default;

bool Compositor::init() {
  ncursor_view_ = std::make_unique<NCursorView>();
  cells_.push_back(std::make_unique<Cell>());
  ncursor_view_->insert_cell(cells_.back().get(), false);
  ncursor_view_->set_cell_select_mode(false);
  theme_engine_ = new ThemeEngine();
  plugin_manager_ = new PluginManager();
  lua_engine_ = new LuaEngine();
  command_context_.compositor = this;
  command_context_.active_view = ncursor_view_.get();
  command_context_.ncursor_view = ncursor_view_.get();
  command_context_.active_cell = active_cell();
  command_context_.theme_engine = theme_engine_;
  command_context_.plugin_manager = plugin_manager_;
  command_context_.lua_engine = lua_engine_;
  command_context_.status_line = &status_line_;
  command_bar_.set_context(&command_context_);
  active_view_ = ncursor_view_.get();
  if (lua_engine_) {
    lua_engine_->init();
    lua_engine_->scan_extensions();
  }
  if (plugin_manager_) {
    plugin_manager_->discover();
  }
  return true;
}

int Compositor::run() {
  if (active_view_) {
    active_view_->layout();
  }
  return 0;
}

void Compositor::set_active_view(View *view) {
  active_view_ = view;
  command_context_.active_view = view;
  command_context_.active_cell = active_cell();
  command_context_.active_gcursor = active_gcursor();
  if (view && view->type() == ViewType::NCURSOR) {
    command_context_.ncursor_view = ncursor_view();
  }
}

NCursorView *Compositor::ncursor_view() const { return ncursor_view_.get(); }

Cell *Compositor::active_cell() const {
  return ncursor_view_ ? ncursor_view_->active_cell() : nullptr;
}

GCursorView *Compositor::active_gcursor() const {
  if (auto *cursor = dynamic_cast<GCursorView *>(active_view_)) {
    return cursor;
  }
  return nullptr;
}

void Compositor::rebuild_command_context() {
  command_context_.compositor = this;
  command_context_.active_view = active_view_;
  command_context_.ncursor_view = ncursor_view();
  command_context_.active_cell = active_cell();
  command_context_.active_gcursor = active_gcursor();
  command_context_.theme_engine = theme_engine_;
  command_context_.plugin_manager = plugin_manager_;
  command_context_.lua_engine = lua_engine_;
  command_context_.status_line = &status_line_;
  command_bar_.set_context(&command_context_);
}

std::string Compositor::spawn_cell(bool above) {
  if (!ncursor_view_) {
    return "spawn failed: no ncursor view";
  }
  cells_.push_back(std::make_unique<Cell>());
  auto *cell = cells_.back().get();
  if (!ncursor_view_->insert_cell(cell, above)) {
    cells_.pop_back();
    return "spawn failed: could not insert cell";
  }
  ncursor_view_->layout();
  rebuild_command_context();
  return std::string("spawned cell ") + cell->id();
}

std::string Compositor::erase_cell(Cell *cell) {
  if (!cell) {
    return "kill failed: no active cell";
  }
  if (ncursor_view_) {
    ncursor_view_->remove_cell(cell);
  }
  if (tcursor_view_) {
    tcursor_view_.reset();
    active_view_ = ncursor_view_.get();
  }
  const auto it = std::remove_if(cells_.begin(), cells_.end(),
                                 [cell](const std::unique_ptr<Cell> &owned) { return owned.get() == cell; });
  if (it != cells_.end()) {
    const auto id = cell->id();
    cells_.erase(it, cells_.end());
    rebuild_command_context();
    return "removed cell " + id;
  }
  return "kill failed: cell not owned";
}

std::string Compositor::kill_selected_cell() { return erase_cell(active_cell()); }

std::string Compositor::move_selected_cell(int delta) {
  if (!ncursor_view_) {
    return "move failed: no ncursor view";
  }
  if (ncursor_view_->move_selected_cell(delta)) {
    ncursor_view_->layout();
    rebuild_command_context();
    return "moved selected cell";
  }
  return "move failed";
}

std::string Compositor::toggle_cell_select_mode() {
  if (!ncursor_view_) {
    return "cell select unavailable";
  }
  ncursor_view_->set_cell_select_mode(!ncursor_view_->cell_select_mode());
  return ncursor_view_->cell_select_mode() ? "cell select enabled" : "cell select disabled";
}

std::string Compositor::focus_active_cell() {
  if (auto *cell = active_cell()) {
    if (auto *term = cell->nterm()) {
      term->on_readable();
    }
    return "focused cell " + cell->id();
  }
  return "focus failed: no active cell";
}

std::string Compositor::promote_active_cell_to_tcursor() {
  if (tcursor_view_) {
    return "tcursor already active";
  }
  auto *cell = active_cell();
  if (!cell) {
    return "promote failed: no active cell";
  }
  tcursor_view_ = std::make_unique<TCursorView>(cell);
  active_view_ = tcursor_view_.get();
  rebuild_command_context();
  return "promoted cell " + cell->id();
}

std::string Compositor::restore_tcursor() {
  if (!tcursor_view_) {
    return "restore failed: no tcursor";
  }
  tcursor_view_.reset();
  active_view_ = ncursor_view_.get();
  rebuild_command_context();
  return "restored tcursor";
}

std::string Compositor::dock_cursor(const std::string &id) {
  if (!ncursor_view_) {
    return "dock failed: no ncursor view";
  }
  auto existing = std::find_if(gcursors_.begin(), gcursors_.end(), [&](const auto &cursor) {
    return cursor && cursor->word_id() == id;
  });
  if (existing == gcursors_.end()) {
    gcursors_.push_back(std::make_unique<GCursorView>(this, id));
    existing = std::prev(gcursors_.end());
  }
  ncursor_view_->cursor_area().dock(existing->get());
  rebuild_command_context();
  return "docked cursor " + (*existing)->word_id();
}

std::string Compositor::restore_cursor(const std::string &id) {
  if (!ncursor_view_) {
    return "restore failed: no ncursor view";
  }
  auto *cursor = ncursor_view_->cursor_area().find_by_id(id);
  if (!cursor) {
    return "restore failed: cursor not found";
  }
  ncursor_view_->cursor_area().restore(cursor);
  set_active_view(cursor);
  return "restored cursor " + cursor->word_id();
}

std::string Compositor::list_cursor_ids() const {
  if (gcursors_.empty()) {
    return "no cursors";
  }
  std::ostringstream out;
  for (const auto &cursor : gcursors_) {
    if (!cursor) {
      continue;
    }
    if (out.tellp() > 0) {
      out << ' ';
    }
    out << cursor->word_id();
  }
  return out.str();
}

std::string Compositor::set_shell_override(const std::string &shell, CommandScope scope) {
  if (scope == CommandScope::CELL) {
    if (auto *cell = active_cell()) {
      cell->set_shell_override(shell);
      return "set cell shell to " + shell;
    }
    return "set shell failed: no active cell";
  }
  for (auto &cell : cells_) {
    if (cell) {
      cell->set_shell_override(shell);
    }
  }
  return "set global shell to " + shell;
}

std::string Compositor::apply_theme_css(const std::string &css) {
  if (!theme_engine_) {
    return "theme engine unavailable";
  }
  ThemeProperties parsed;
  if (!theme_engine_->parse_css(css, parsed)) {
    return theme_engine_->last_error();
  }
  theme_engine_->swap_active(std::move(parsed));
  return "theme applied";
}

void WlDisplayDeleter::operator()(wl_display *ptr) const noexcept {
  (void)ptr;
}
void WlrBackendDeleter::operator()(wlr_backend *ptr) const noexcept {
  (void)ptr;
}
void WlrRendererDeleter::operator()(wlr_renderer *ptr) const noexcept {
  (void)ptr;
}
void WlrAllocatorDeleter::operator()(wlr_allocator *ptr) const noexcept {
  (void)ptr;
}
void WlrOutputLayoutDeleter::operator()(wlr_output_layout *ptr) const noexcept {
  (void)ptr;
}
void WlrSceneDeleter::operator()(wlr_scene *ptr) const noexcept {
  (void)ptr;
}
void WlrSeatDeleter::operator()(wlr_seat *ptr) const noexcept {
  (void)ptr;
}
