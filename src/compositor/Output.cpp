#include "compositor/Compositor.hpp"
#include "compositor/WaylandRuntime.h"
#include "views/NCursorView.hpp"
#include "views/GCursorView.hpp"
#include "views/TCursorView.hpp"

#include <algorithm>
#include <set>
#include <sstream>

void Compositor::synchronize_outputs(const std::vector<OutputGeometry> &geometry) {
  remember_workspace_focus();
  std::set<std::string> connected;
  for (const auto &item : geometry) {
    if (item.name.empty() || item.width <= 0 || item.height <= 0) continue;
    connected.insert(item.name);
    outputs_[item.name].geometry = item;
  }
  // Keep the desktop alive while the last monitor is disconnected. Its views
  // will be migrated when another monitor arrives.
  if (connected.empty()) return;
  const std::string fallback = connected.count(active_output_) ? active_output_ : *connected.begin();
  for (auto it = outputs_.begin(); it != outputs_.end();) {
    if (connected.count(it->first)) { ++it; continue; }
    for (auto &view : ncursor_views_)
      if (view->output_name() == it->first) view->set_output_name(fallback);
    for (auto &view : gcursors_)
      if (view->output_name() == it->first) view->set_output_name(fallback);
    if (tcursor_view_ && tcursor_view_->output_name() == it->first)
      tcursor_view_->set_output_name(fallback);
    auto &destination = outputs_[fallback];
    for (int ws = kMinWorkspace; ws <= kMaxWorkspace; ++ws) {
      if (!destination.ncursors[ws]) destination.ncursors[ws] = it->second.ncursors[ws];
      if (!destination.views[ws]) destination.views[ws] = it->second.views[ws];
    }
    it = outputs_.erase(it);
  }
  active_output_ = fallback;
  restore_workspace_focus();
  relayout();
}

void Compositor::relayout() {
  if (laying_out_) return;
  laying_out_ = true;
  remember_workspace_focus();
  const auto focused_output = active_output_;
  rendering_output_ = focused_output;
  bool any_graphical = false;
  for (const auto &cursor : gcursors_)
    any_graphical |= !cursor->docked() && cursor->workspace() == current_workspace_;
  diftray_wayland_runtime_set_gcursor_visible(wayland_runtime_, any_graphical);
  for (auto &[name, output] : outputs_) {
    active_output_ = name;
    restore_workspace_focus();
    if (!active_ncursor_) spawn_ncursor();
    const auto &box = output.geometry;
    output_x_ = box.x; output_y_ = box.y;
    output_width_ = box.width; output_height_ = box.height;
    layout_current_output();
    remember_workspace_focus();
  }
  active_output_ = focused_output;
  restore_workspace_focus();
  const auto &box = outputs_.at(active_output_).geometry;
  output_x_ = box.x; output_y_ = box.y;
  output_width_ = box.width; output_height_ = box.height;
  diftray_wayland_runtime_set_chrome_box(wayland_runtime_, box.x, box.y, box.width, box.height);
  // Keyboard focus belongs to the selected monitor, regardless of render order.
  apply_view_visibility();
  rebuild_command_context();
  update_chrome();
  laying_out_ = false;
}

std::string Compositor::list_outputs() const {
  std::ostringstream result;
  for (const auto &[name, output] : outputs_) {
    if (result.tellp() > 0) result << '\n';
    const auto &box = output.geometry;
    result << (name == active_output_ ? "* " : "  ") << name << ' '
           << box.width << 'x' << box.height << " at " << box.x << ',' << box.y;
  }
  return result.str();
}

std::string Compositor::focus_output(const std::string &requested) {
  std::string name = requested;
  if (name == "next" || name == "prev") {
    auto it = outputs_.find(active_output_);
    if (name == "next") {
      if (++it == outputs_.end()) it = outputs_.begin();
    } else {
      if (it == outputs_.begin()) it = outputs_.end();
      --it;
    }
    name = it->first;
  }
  if (!outputs_.count(name)) return "output not found: " + name;
  remember_workspace_focus();
  active_output_ = name;
  restore_workspace_focus();
  if (!active_ncursor_) spawn_ncursor();
  help_pager_active_ = false;
  help_search_open_ = false;
  relayout();
  return "output " + name;
}

std::string Compositor::move_to_output(const std::string &name) {
  if (!outputs_.count(name)) return "output not found: " + name;
  if (name == active_output_) return "already on output " + name;
  if (!active_ncursor_) return "no active ncursor";
  if (tcursor_view_ && tcursor_view_->output_name() == active_output_)
    restore_tcursor();
  auto *moving = active_ncursor_;
  moving->set_output_name(name);
  for (auto &cursor : gcursors_) {
    if (cursor->owner_ncursor() == moving ||
        (cursor->owner_cell() && owner_ncursor(cursor->owner_cell()) == moving))
      cursor->set_output_name(name);
  }
  outputs_[name].ncursors[current_workspace_] = moving;
  outputs_[name].views[current_workspace_] = moving;
  active_ncursor_ = nullptr;
  active_view_ = nullptr;
  // Relayout supplies a replacement cell on an output whose last tab moved.
  relayout();
  return "moved ncursor " + moving->id() + " to output " + name;
}
