#pragma once

#include "views/CursorArea.hpp"

#include <memory>
#include <string>

#include <wlr/util/box.h>

struct diftray_cell_surface;

class NTerm;

enum class CellState {
  NORMAL,
  SELECTED,
  TCURSOR,
};

class Cell {
public:
  explicit Cell(std::string shell_override = {});
  ~Cell();

  const std::string &id() const;
  const wlr_box &box() const;
  void set_box(const wlr_box &box);
  CellState state() const;
  void set_state(CellState state);
  NTerm *nterm() const;
  CursorArea &cursor_area();
  const CursorArea &cursor_area() const;
  void set_shell_override(std::string shell_override, bool individual = true);
  bool has_shell_override() const { return individual_shell_; }
  const std::string &shell_override() const;
  void set_surface(diftray_cell_surface *surface);
  diftray_cell_surface *surface() const;

private:
  std::string id_;
  wlr_box box_{};
  CellState state_ = CellState::NORMAL;
  std::unique_ptr<NTerm> nterm_;
  CursorArea cursor_area_;
  std::string shell_override_;
  bool individual_shell_ = false;
  diftray_cell_surface *surface_ = nullptr;
};
