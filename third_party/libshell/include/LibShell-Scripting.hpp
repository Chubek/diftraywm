#pragma once

// QaMRpp Lua adapter.
//
// The embedded-Language dependency is confined to this header and its single
// translation unit; the core runtime and the platform layer neither know nor
// care which engine backs `$(lua ...)` / `` `lua:...` ``.

#include "lsh/Scripting.hpp"
