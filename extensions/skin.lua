-- DiftrayWM extension: theme and display settings.
--
-- Install: cp extensions/theme.lua ~/.config/diftraywm/extensions/
-- Command: :skin
--
-- A small front end over `theme load` and `set theme`, for the adjustments you
-- make often enough to want a name for: a larger font, a different border, a
-- variant of your normal theme.
--
-- The compositor parses CSS and owns every visual value, so nothing here sets a
-- colour or a radius itself. What this does is assemble a CSS declaration and
-- hand it to `set theme`, which means a preset is still theme data on the
-- compositor's side and reloads with the rest of the theme.
--
--   :skin                    the preset list
--   :skin keys               the properties the compositor reads
--   :skin show               list the active theme properties
--   :skin add plain <css>    a preset from a raw block
--   :skin set big border-size 1px     a preset from one declaration
--   :skin apply big          apply it
--   :skin edit big border-size 2px   change it
--   :skin rm big
--   :skin find border        search presets
--   :skin clear
--
-- Examples:
--
--   :skin set big border-size 1px
--   :skin set calm border-color #6f8ba6
--   :skin add plain :root { background-color: #101418; border-color: #6f8ba6; }
--   :skin apply big
--
-- Theme property names are hyphenated, matching themes/default.css: it is
-- `border-size`, not `border_size`. The latter parses into the token map and is
-- then read by nothing, which looks like a working preset that does nothing, so
-- `skin keys` lists the names that are actually read.

local MAX_PRESETS = 32
local MAX_CSS = 512
local PAGE = 8

local presets = {}   -- [name] = {name, css = ":root { ... }"}
local order = {}
local page = {}

local function say(text)
  diftray.status(string.sub(text, 1, 4000))
end

local function plural(count, singular, many)
  return tostring(count) .. " " .. (count == 1 and singular or (many or singular .. "s"))
end

local function rest(tokens, first)
  local parts = {}
  for i = first, #tokens do
    parts[#parts + 1] = tokens[i]
  end
  return table.concat(parts, " ")
end

local function index_of(name)
  for i = 1, #order do
    if order[i] == name then
      return i
    end
  end
  return nil
end

local function get(name)
  local at = index_of(name)
  return at and presets[name] or nil
end

local function valid_name(name)
  if name == nil or name == "" then
    return false, "a preset needs a name"
  end
  if #name > 48 then
    return false, "a preset name longer than 48 characters is not worth typing"
  end
  if string.find(name, "[%s\"]") then
    return false, "a preset name cannot contain spaces or quotes"
  end
  local reserved = {add = true, set = true, apply = true, edit = true,
                    show = true, keys = true, find = true, rm = true,
                    clear = true, stats = true}
  if reserved[string.lower(name)] then
    return false, "that name is a preset subcommand: " .. name
  end
  return true
end

-- The theme keys the compositor reads, with the note each one wants. Listed
-- here so `skin keys` can tell the user what is available instead of leaving a
-- mistyped property to fail as "incomplete CSS declaration" or, worse, to parse
-- into a token that nothing reads.
local KEYS = {
  ["border-size"] = "the border around the active cell",
  ["border-color"] = "the border and cursor colour",
  ["background-color"] = "the desktop background",
  ["command-bar-height"] = "the command bar's height",
  ["status-bar-height"] = "the status line's height",
  ["launcher-bar-height"] = "the launcher bar's height",
  ["command-bar-color"] = "the command bar's background",
  ["launcher-bar-color"] = "the launcher bar's background",
  ["terminal-background-color"] = "the terminal cell's background",
  ["terminal-foreground-color"] = "the terminal cell's text",
  ["terminal-cursor-color"] = "the terminal cell's cursor",
  ["terminal-cursor-thickness"] = "the terminal cursor's thickness",
  ["highlight-color"] = "the colour used for highlights",
  ["animation"] = "a CSS animation, e.g. cell-appear 140ms ease-out",
}

local function sorted_keys()
  local names = {}
  for name in pairs(KEYS) do
    names[#names + 1] = name
  end
  table.sort(names)
  return names
end

-- One declaration, wrapped the way the theme engine expects. The `name: value`
-- form and the trailing semicolon are both required: the parser splits on the
-- colon and commits a declaration only when both halves are non-empty, so
-- `:root { border-size }` is a parse failure rather than a property with no
-- value.
local function wrap(key, value)
  if key == "" then
    return nil, "a declaration needs a property name"
  end
  if string.find(key, "[%s{};:]") then
    return nil, "a property name cannot contain spaces, a colon, braces or a semicolon"
  end
  if value == "" then
    return nil, key .. " needs a value"
  end
  if string.find(value, "[{}]") then
    return nil, "a single value cannot contain braces; use skin add for a whole block"
  end
  if string.find(value, ";") then
    return nil, "one property per preset; use skin add for a whole block"
  end
  local note = KEYS[string.lower(key)]
  if note == nil then
    -- An unknown property parses cleanly into the token map and then does
    -- nothing, which is worse than a rejection: warn rather than refuse, since
    -- the theme engine is the authority on which properties it reads.
    return ":root { " .. key .. ": " .. value .. "; }",
           "the compositor does not read \"" .. key .. "\"; run skin keys"
  end
  return ":root { " .. key .. ": " .. value .. "; }", note
end

local function render(entries, title)
  local lines = {title}
  for i = 1, #page do
    lines[#lines + 1] = string.format("  %d) %-12s %s", i, page[i].name, page[i].css)
  end
  if #page < #entries then
    lines[#lines + 1] = string.format("  ... %d more", #entries - #page)
  end
  return table.concat(lines, "\n")
end

local function in_order()
  local list = {}
  for i = 1, #order do
    list[#list + 1] = presets[order[i]]
  end
  return list
end

local function show_list(entries, title)
  page = {}
  for i = 1, math.min(PAGE, #entries) do
    page[#page + 1] = entries[i]
  end
  if #page == 0 then
    say(title .. " (none)")
    return
  end
  say(render(entries, title))
end

diftray.register_command("skin", function(tokens, scope)
  local action = tokens[2]

  if action == nil then
    -- The active properties come from the theme engine, so ask rather than
    -- cache: a preset applied outside this extension would otherwise make the
    -- listing lie.
    diftray.command("theme show")
    say("skin: " .. plural(#order, "preset") .. ". The properties in effect " ..
        "are above.")
    return
  end

  if action == "show" then
    diftray.command("theme show")
    say("skin show: the active theme properties are above")
    return
  end

  if action == "add" then
    local name = tokens[3]
    local ok, why = valid_name(name)
    if not ok then
      say("skin add: " .. why)
      return
    end
    if get(name) ~= nil then
      say("skin add: " .. name .. " already exists; use skin edit to change it")
      return
    end
    local css = rest(tokens, 4)
    if css == "" then
      say("skin add needs a declaration, for example: skin add plain " ..
          ":root { background_color #101418 }")
      return
    end
    if #css > MAX_CSS then
      say("skin add: " .. #css .. " characters is over the " .. MAX_CSS ..
          " limit for a preset")
      return
    end
    if #order >= MAX_PRESETS then
      say("skin holds at most " .. MAX_PRESETS .. " presets")
      return
    end
    presets[name] = {name = name, css = css, note = "a raw declaration, as written"}
    order[#order + 1] = name
    say("skin " .. name .. " = " .. css)
    return
  end

  if action == "set" then
    local name = tokens[3]
    local ok, why = valid_name(name)
    if not ok then
      say("skin set: " .. why)
      return
    end
    local key = tokens[4]
    local value = rest(tokens, 5)
    if key == nil then
      say("skin set needs a property and a value, for example: skin set big font_size 18")
      return
    end
    local css, note = wrap(key, value)
    if css == nil then
      say("skin set: " .. note)
      return
    end
    local existing = get(name)
    if existing ~= nil then
      -- Editing in place rather than appending: two :root blocks for one
      -- preset would leave the user guessing which one wins.
      existing.css = css
      existing.note = note
      say("skin " .. name .. " = " .. css .. " (replaced). " .. note)
      return
    end
    if #order >= MAX_PRESETS then
      say("skin holds at most " .. MAX_PRESETS .. " presets")
      return
    end
    presets[name] = {name = name, css = css, note = note}
    order[#order + 1] = name
    say("skin " .. name .. " = " .. css .. ". " .. note)
    return
  end

  if action == "edit" then
    local name = tokens[3]
    local entry = name and get(name) or nil
    if entry == nil then
      say("skin edit needs a preset name, for example: skin edit big")
      return
    end
    local key = tokens[4]
    local value = rest(tokens, 5)
    if key == nil then
      say("skin edit needs a property and a value, for example: skin edit big font_size 18")
      return
    end
    local css, note = wrap(key, value)
    if css == nil then
      say("skin edit: " .. note)
      return
    end
    entry.css = css
    entry.note = note
    say("skin " .. name .. " = " .. css .. " (replaced). " .. note)
    return
  end

  if action == "apply" then
    local target = tokens[3]
    local entry
    if target ~= nil and tonumber(target) ~= nil then
      local at = math.floor(tonumber(target))
      local choices = #page > 0 and page or in_order()
      if at < 1 or at > #choices then
        say("skin apply: index " .. at .. " is outside the " ..
            plural(#choices, "preset") .. " available")
        return
      end
      entry = choices[at]
    else
      entry = target and get(target) or nil
      if entry == nil then
        say("skin apply needs a preset name, for example: skin apply big")
        return
      end
    end
    diftray.command("set theme " .. entry.css)
    say("skin applied " .. entry.name .. ": " .. entry.css)
    return
  end

  if action == "keys" then
    local lines = {"skin: the properties the compositor reads"}
    for i = 1, #sorted_keys() do
      local name = sorted_keys()[i]
      lines[#lines + 1] = string.format("  %-26s %s", name, KEYS[name])
    end
    say(table.concat(lines, "\n"))
    return
  end

  if action == "find" then
    local query = rest(tokens, 3)
    if query == "" then
      say("skin find needs something to look for")
      return
    end
    local needle = string.lower(query)
    local found = {}
    for i = 1, #order do
      local entry = presets[order[i]]
      if string.find(string.lower(entry.name .. " " .. entry.css), needle, 1, true) then
        found[#found + 1] = entry
      end
    end
    if #found == 0 then
      say("skin find: nothing matches " .. query)
      return
    end
    show_list(found, "skin find " .. query .. " (" .. #found .. " of " .. #order .. ")")
    return
  end

  if action == "rm" then
    local name = tokens[3]
    local at = name and index_of(name) or nil
    if at == nil then
      say("skin rm needs a preset name, for example: skin rm big")
      return
    end
    presets[name] = nil
    table.remove(order, at)
    say("skin forgot " .. name .. " (" .. plural(#order, "preset") ..
        "). The theme in effect is unchanged.")
    return
  end

  if action == "clear" then
    local count = #order
    presets = {}
    order = {}
    page = {}
    say("skin cleared " .. plural(count, "preset") ..
        ". The theme in effect is unchanged.")
    return
  end

  if action == "stats" then
    say("skin: " .. plural(#order, "preset") ..
        ". The compositor still owns every value; these are just CSS shorthands.")
    return
  end

  say("skin supports: add, set, apply, edit, show, keys, find, rm, clear, stats")
end)
