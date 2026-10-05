#pragma once

#include <string>
#include <utility>

enum class ViewType {
  NCURSOR,
  GCURSOR,
  TCURSOR,
};

class View {
public:
  // Views own layout and lifecycle state. The compositor routes input/focus,
  // and WaylandRuntime renders their scene nodes; there are no alternate
  // rendering or keyboard hooks on the view model.
  const std::string &output_name() const { return output_name_; }
  void set_output_name(std::string name) { output_name_ = std::move(name); }
  virtual ~View() = default;
  virtual ViewType type() const = 0;

private:
  std::string output_name_ = "default";
};
