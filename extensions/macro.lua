-- DiftrayWM extension: named command macros.
--
-- Install: cp extensions/macro.lua ~/.config/diftraywm/extensions/
-- Command: :macro
--
-- A macro is a named list of Command Bar commands. `macro save` starts one,
-- `macro add` appends a step to it, and `macro run` replays the whole list in
-- order. Steps are queued rather than dispatched, so a macro cannot recurse
-- into the command bar while it is running, and the compositor's own queue
-- depth limits a macro to what it can absorb in a tick.
--
-- This is the scriptable half of what the configuration DSL's `macro` keyword
-- does with `bind()`, minus the binding: the config program's macros are
-- compile-time and tied to a chord, these are session-time and named by you.
-- When you find a sequence worth keeping, put it here and bind the chord with
-- `bind("Meta", keysym("F5"), "macro run name")`.
--
--   :macro                      the macro list
--   :macro save build spawn below    start a macro called build
--   :macro add build spawn below     append a step
--   :macro run build                 replay every step
--   :macro show build                print the steps without running them
--   :macro find cell                 search names and steps
--   :macro rm build                 forget a macro
--   :macro rename build b            rename it
--   :macro clear                    forget everything

local MAX_MACROS = 64
local MAX_STEPS = 32      -- the compositor drains 16 queued commands per tick,
                          -- so a longer macro is not merely slow, it is
                          -- half-applied for a frame
local PAGE = 8

local macros = {}   -- {name = "build", steps = {...}, when = tick}
local order = {}    -- creation order, so the listing matches how it was built
local page = {}     -- the last listing, for `macro run <n>`
local tick = 0

local function say(text)
  diftray.status(string.sub(text, 1, 4000))
end

local function rest(tokens, first)
  local parts = {}
  for i = first, #tokens do
    parts[#parts + 1] = tokens[i]
  end
  return table.concat(parts, " ")
end

local function plural(count, singular, many)
  return tostring(count) .. " " .. (count == 1 and singular or (many or singular .. "s"))
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
  return at and macros[name] or nil
end

-- A name has to survive being a single Command Bar token, because that is what
-- `macro run <name>` hands over.
local function valid_name(name)
  if name == nil or name == "" then
    return false, "a macro needs a name"
  end
  if #name > 48 then
    return false, "a macro name longer than 48 characters is not worth typing"
  end
  if string.find(name, "[%s\"]") then
    return false, "a macro name cannot contain spaces or quotes"
  end
  local reserved = {save = true, add = true, run = true, show = true,
                    find = true, rm = true, rename = true, clear = true,
                    stats = true}
  if reserved[string.lower(name)] then
    return false, "that name is a macro subcommand: " .. name
  end
  return true
end

local function preview(entry, limit)
  local parts = {}
  local count = math.min(limit or #entry.steps, #entry.steps)
  for i = 1, count do
    parts[#parts + 1] = entry.steps[i]
  end
  return table.concat(parts, "  ->  ")
end

local function render(entries, title)
  local lines = {title}
  for i = 1, #page do
    local entry = page[i]
    lines[#lines + 1] = string.format("  %d) %-14s %-8s %s", i, entry.name,
                                     plural(#entry.steps, "step"),
                                     preview(entry, 3))
  end
  if #page < #entries then
    lines[#lines + 1] = string.format("  ... %d more", #entries - #page)
  end
  return table.concat(lines, "\n")
end

local function in_order()
  local list = {}
  for i = 1, #order do
    list[#list + 1] = macros[order[i]]
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

-- Replays a macro. Each step goes through the queue so the compositor runs them
-- one at a time on its own tick; a macro of 30 steps therefore takes two ticks,
-- which is the intended behaviour for a layout change and not a defect.
local function replay(entry)
  for i = 1, #entry.steps do
    diftray.command(entry.steps[i])
  end
  entry.when = tick
  return entry
end

diftray.register_command("macro", function(tokens, scope)
  local action = tokens[2]

  if action == nil then
    show_list(in_order(), "macro: " .. plural(#order, "macro") .. ", oldest first")
    return
  end

  if action == "save" then
    local name = tokens[3]
    local ok, why = valid_name(name)
    if not ok then
      say("macro save: " .. why)
      return
    end
    if get(name) ~= nil then
      say("macro save: " .. name .. " already exists; use macro add to extend it")
      return
    end
    if #order >= MAX_MACROS then
      say("macro holds at most " .. MAX_MACROS .. " macros")
      return
    end
    local step = rest(tokens, 4)
    if step == "" then
      -- A macro with no step is a legitimate thing to create and fill in, so
      -- this is allowed and reported rather than rejected.
      macros[name] = {name = name, steps = {}, when = tick}
      order[#order + 1] = name
      say("macro " .. name .. " created with no steps; add one with macro add " ..
          name .. " spawn below")
      return
    end
    macros[name] = {name = name, steps = {step}, when = tick}
    order[#order + 1] = name
    say("macro " .. name .. " = " .. step)
    return
  end

  if action == "add" then
    local name = tokens[3]
    local step = rest(tokens, 4)
    if name == nil or step == "" then
      say("macro add needs a name and a step, for example: macro add build spawn below")
      return
    end
    local entry = get(name)
    if entry == nil then
      say("macro add: no macro called " .. name .. "; macro save " .. name .. " first")
      return
    end
    if #entry.steps >= MAX_STEPS then
      say("macro " .. name .. " is at the " .. MAX_STEPS ..
          "-step limit, which is what the command queue drains in a few ticks")
      return
    end
    entry.steps[#entry.steps + 1] = step
    say("macro " .. name .. " step " .. #entry.steps .. ": " .. step)
    return
  end

  if action == "run" then
    -- `macro run <n>` runs the nth listed macro; `macro run <name>` runs a named
    -- one. The index form exists because the listing is numbered and typing a
    -- name from a status line is more error-prone than typing a digit.
    local target = tokens[3]
    local entry
    if target ~= nil and tonumber(target) ~= nil then
      local at = math.floor(tonumber(target))
      -- Falls back to creation order when nothing is listed, because an index
      -- into "my macros" means the same thing either way.
      local choices = #page > 0 and page or in_order()
      if at < 1 or at > #choices then
        say("macro run: index " .. at .. " is outside the " ..
            plural(#choices, "macro") .. " available")
        return
      end
      entry = choices[at]
    else
      entry = target and get(target) or nil
      if entry == nil then
        say("macro run needs a macro name, for example: macro run build")
        return
      end
    end
    if #entry.steps == 0 then
      say("macro " .. entry.name .. " has no steps")
      return
    end
    replay(entry)
    say("macro " .. entry.name .. ": " .. preview(entry) ..
        " (" .. plural(#entry.steps, "step") .. ")")
    return
  end

  if action == "show" then
    local name = tokens[3]
    local entry = name and get(name) or nil
    if entry == nil then
      say("macro show needs a macro name, for example: macro show build")
      return
    end
    local lines = {"macro " .. entry.name .. ", " .. plural(#entry.steps, "step") .. ":"}
    for i = 1, #entry.steps do
      lines[#lines + 1] = "  " .. i .. ") " .. entry.steps[i]
    end
    say(table.concat(lines, "\n"))
    return
  end

  if action == "find" then
    local query = rest(tokens, 3)
    if query == "" then
      say("macro find needs something to look for")
      return
    end
    local needle = string.lower(query)
    local found = {}
    for i = 1, #order do
      local entry = macros[order[i]]
      local haystack = string.lower(entry.name .. " " ..
                                    table.concat(entry.steps, " "))
      if string.find(haystack, needle, 1, true) then
        found[#found + 1] = entry
      end
    end
    if #found == 0 then
      say("macro find: nothing matches " .. query)
      return
    end
    show_list(found, "macro find " .. query .. " (" .. #found .. " of " .. #order .. ")")
    return
  end

  if action == "rm" then
    local name = tokens[3]
    local at = name and index_of(name) or nil
    if at == nil then
      say("macro rm needs a macro name, for example: macro rm build")
      return
    end
    macros[name] = nil
    table.remove(order, at)
    say("macro forgot " .. name .. " (" .. plural(#order, "macro") .. ")")
    return
  end

  if action == "rename" then
    local from, to = tokens[3], tokens[4]
    local entry = from and get(from) or nil
    if entry == nil then
      say("macro rename needs an existing macro, for example: macro rename build b")
      return
    end
    local ok, why = valid_name(to)
    if not ok then
      say("macro rename: " .. why)
      return
    end
    if get(to) ~= nil then
      say("macro rename: " .. to .. " already exists")
      return
    end
    macros[to] = entry
    entry.name = to
    order[index_of(from)] = to
    say("macro " .. from .. " renamed to " .. to)
    return
  end

  if action == "clear" then
    macros = {}
    order = {}
    page = {}
    say("macro cleared")
    return
  end

  if action == "stats" then
    local steps = 0
    for i = 1, #order do
      steps = steps + #macros[order[i]].steps
    end
    say(string.format("macro: %s, %d steps, tick %d",
                      plural(#order, "macro"), steps, tick))
    return
  end

  say("macro supports: save, add, run, show, find, rm, rename, clear, stats")
end)

-- Events are the only clock an extension state has; `os` is not available here.
-- The tick stamps when a macro last ran, which is the only ordering the status
-- line can honestly report.
diftray.on("view", function()
  tick = tick + 1
end)
diftray.on("input", function()
  tick = tick + 1
end)
