#pragma once

#include <string>

class Cell;
class Compositor;
class GCursorView;
class LuaEngine;
class NCursorView;
class PluginManager;
class ThemeEngine;
class View;

struct CommandContext {
  std::string raw_input;
  Compositor *compositor = nullptr;
  View *active_view = nullptr;
  NCursorView *ncursor_view = nullptr;
  Cell *active_cell = nullptr;
  GCursorView *active_gcursor = nullptr;
  ThemeEngine *theme_engine = nullptr;
  PluginManager *plugin_manager = nullptr;
  LuaEngine *lua_engine = nullptr;
  std::string *status_line = nullptr;
};
