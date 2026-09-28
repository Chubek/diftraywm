#pragma once
// The supplied Kaguya predates Lua 5.5. Keep the compatibility layer in
// DiftrayWM so neither vendored tree needs patching.
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#if LUA_VERSION_NUM >= 505
#include <chrono>
inline lua_State *diftray_lua_newstate(lua_Alloc allocator, void *userdata) {
  return lua_newstate(allocator, userdata,
      static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count()));
}
#define lua_newstate diftray_lua_newstate
#define LUA_GCSETPAUSE LUA_GCPARAM, LUA_GCPPAUSE
#define LUA_GCSETSTEPMUL LUA_GCPARAM, LUA_GCPSTEPMUL
#endif
#include <kaguya/kaguya.hpp>
#if LUA_VERSION_NUM >= 505
#undef lua_newstate
#undef LUA_GCSETPAUSE
#undef LUA_GCSETSTEPMUL
#endif
