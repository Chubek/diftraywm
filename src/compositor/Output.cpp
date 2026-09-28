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
  bool changed = false;
  std::set<std::string> connected;
  for (const auto &item : geometry) {
    if (item.name.empty() || item.width <= 0 || item.height <= 0) continue;
    const auto old = outputs_.find(item.name);
    if (old == outputs_.end()) changed = true;
    else {
      const auto &g = old->second.geometry;
      changed |= g.x != item.x || g.y != item.y || g.width != item.width || g.height != item.height ||
                 g.rotation != item.rotation || g.scale != item.scale;
    }
    connected.insert(item.name);
    outputs_[item.name].geometry = item;
  }
  // Keep the desktop alive while the last monitor is disconnected. Its views
  // will be migrated when another monitor arrives.
  if (connected.empty()) return;
  const std::string fallback = connected.count(active_output_) ? active_output_ : *connected.begin();
  for (auto it = outputs_.begin(); it != outputs_.end();) {
    if (connected.count(it->first)) { ++it; continue; }
    changed = true;
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
  if (changed) for (const auto &[cell, notelet] : notelet_cells_) request_notelet(cell, "", "outputs");
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
  notify_extensions("view");
}

std::string Compositor::list_outputs() const {
  std::ostringstream result;
  for (const auto &[name, output] : outputs_) {
    if (result.tellp() > 0) result << '\n';
    const auto &box = output.geometry;
    result << (name == active_output_ ? "* " : "  ") << name << ' '
           << box.width << 'x' << box.height << " at " << box.x << ',' << box.y
           << " rotation=" << box.rotation << " scale=" << box.scale;
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

MonitorConfig Compositor::monitor_config(const std::string &name) const {
  MonitorConfig result;
  for (const auto &monitor : config_.monitors) {
    if (monitor.name == name) return monitor;
    if (monitor.name == "*") result = monitor;
  }
  result.name = name;
  return result;
}

std::string Compositor::configure_output(const std::string &name, const std::string &setting,
                                         const std::string &value, const std::string &extra) {
  if (!outputs_.count(name)) return "output not found: " + name;
  auto monitor = monitor_config(name);
  std::map<std::string, std::string> values;
  if (setting == "rotate") values["rotation"] = value;
  else if (setting == "scale") values["scale"] = value;
  else if (setting == "position") {
    if (value == "auto" && extra.empty()) monitor.positioned = false;
    else { values["x"] = value; values["y"] = extra; }
  } else return "unknown output setting: " + setting;
  std::string error;
  if (!parse_monitor_settings(values, monitor, error)) return error;
  if (!wayland_runtime_) return "output configuration requires a running Wayland backend";
  const diftray_output_config state{monitor.rotation, monitor.scale, monitor.positioned, monitor.x, monitor.y};
  if (!diftray_wayland_runtime_configure_output(wayland_runtime_, name.c_str(), &state))
    return "output configuration rejected: " + name;
  // Retain successful overrides for reconnection, without rewriting the file.
  auto it = std::find_if(config_.monitors.begin(), config_.monitors.end(), [&](const auto &m) { return m.name == name; });
  if (it == config_.monitors.end()) config_.monitors.push_back(monitor);
  else *it = monitor;
  return "configured output " + name;
}
