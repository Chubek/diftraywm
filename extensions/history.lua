-- DiftrayWM extension: command history and undo.
--
-- Install: cp extensions/history.lua ~/.config/diftraywm/extensions/
-- Command: :hist
--
-- A journal of the Command Bar commands you have run, with the two things a
-- journal is actually for: finding the one you ran before, and taking it back.
--
-- The compositor does not report dispatched commands to extensions, so the
-- journal is fed by hand. `hist` records the command itself, which is one extra
-- word at the end of what you were going to type anyway, and it is the only
-- honest option: an extension that guessed at command traffic from input events
-- could not tell a keystroke in a terminal from one in the command bar.
--
-- Undo is the part worth having. The compositor dispatches the inverse
-- directly, so `hist undo` walks back through the journal and issues the reverse
-- of what it finds. Only reversible commands are undone -- spawning a cell is
-- recorded but not reversed, because killing the wrong cell to undo a spawn is
-- worse than not undoing at all. Everything `hist undo` touches is listed before
-- it runs.
--
--   :hist                  the recent journal, newest first
--   :hist 20               the last twenty entries
--   :hist record spawn below   add an entry by hand
--   :hist search spawn     entries matching a substring
--   :hist once workspace 3 the one command this journal is for
--   :hist undo             reverse the newest reversible entry
--   :hist undo 2           reverse two steps back
--   :hist undo --dry       say what undo would do, change nothing
--   :hist drop 3           forget entry 3
--   :hist clear
--   :hist stats

local MAX_ENTRIES = 512
local PAGE = 8

local journal = {}   -- newest last; each entry is {text, inverse, when}
local view = {}      -- the last listing, so `drop` and `undo` share its indices
local event_tick = 0 -- frame, view and input events, the only clock available
local shown = PAGE

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

local function integer(value)
  local number = tonumber(value)
  if number == nil then
    return nil
  end
  return math.floor(number)
end

local function words_of(text)
  local list = {}
  for word in string.gmatch(text, "%S+") do
    list[#list + 1] = word
  end
  return list
end

-- The reverse of a command, or nil when there is no safe reverse. This is a
-- table rather than a rule engine because the compositor's command set is small
-- and a wrong inverse does real damage: `mux kill` after a `mux split` is fine,
-- `kill` after a `spawn` might not be.
local INVERSE = {
  ["spawn below"] = nil,  -- recorded, but the cell it made is not identifiable
  ["spawn above"] = nil,
  kill = nil,             -- a kill may follow a prompt, so it is not replayable
  ["tab next"] = "tab prev",
  ["tab prev"] = "tab next",
  ["move up"] = "move down",
  ["move down"] = "move up",
  ["mux focus next"] = "mux focus prev",
  ["mux focus prev"] = "mux focus next",
  ["cursor dock"] = nil,   -- needs the id, handled below
}

-- Commands whose reverse needs a specific argument the entry carries, because
-- the journal has it but the plain form does not.
local function argument_inverse(text)
  local words = words_of(text)
  if #words < 2 then
    return nil
  end
  if words[1] == "cursor" and words[2] == "dock" and words[3] then
    return "cursor restore " .. words[3]
  end
  if words[1] == "cursor" and words[2] == "restore" and words[3] then
    return "cursor dock " .. words[3]
  end
  if words[1] == "cursor" and words[2] == "assign" and words[3] then
    -- The previous slot is not recorded, so this is reported rather than guessed.
    return nil
  end
  if words[1] == "mux" and words[2] == "split" then
    return "mux kill"
  end
  if words[1] == "cell" and words[2] == "promote" then
    return "cell restore"
  end
  if words[1] == "cell" and words[2] == "restore" then
    return "cell promote"
  end
  if words[1] == "launcher" and words[2] == "lock" then
    return "launcher unlock"
  end
  if words[1] == "launcher" and words[2] == "unlock" then
    return "launcher lock"
  end
  if words[1] == "workspace" and words[2] then
    return nil  -- the previous workspace is not known to this extension
  end
  if words[1] == "keymap" and words[2] == "profile" and words[3] then
    return nil
  end
  return nil
end

local function inverse_of(text)
  local words = words_of(text)
  if #words == 0 then
    return nil, "empty entry"
  end
  local exact = INVERSE[text]
  if exact ~= nil then
    return exact
  end
  local via_argument = argument_inverse(text)
  if via_argument ~= nil then
    return via_argument
  end
  return nil, "no safe reverse for " .. words[1]
end

local function note_of(entry)
  if entry.inverse then
    return "undo with: " .. entry.inverse
  end
  return entry.reason or "not reversible"
end

local function render(entries, title)
  local lines = {title}
  for i = 1, #view do
    lines[#lines + 1] = string.format("  %d) %-28s %s", i, view[i].text, note_of(view[i]))
  end
  if #view < #entries then
    lines[#lines + 1] = string.format("  ... %d more", #entries - #view)
  end
  return table.concat(lines, "\n")
end

-- Newest first, which is the order a journal is read in.
local function newest_first(count)
  local list = {}
  for i = #journal, 1, -1 do
    list[#list + 1] = journal[i]
  end
  if count ~= nil and #list > count then
    for i = #list, count + 1, -1 do
      list[i] = nil
    end
  end
  return list
end

local function remember(text)
  local inverse, reason = inverse_of(text)
  local entry = {text = text, inverse = inverse, reason = reason, when = event_tick}
  if #journal >= MAX_ENTRIES then
    table.remove(journal, 1)
  end
  journal[#journal + 1] = entry
  return entry
end

local function show_list(entries, title)
  view = {}
  local limit = math.min(shown > 0 and shown or PAGE, #entries)
  for i = 1, limit do
    view[#view + 1] = entries[i]
  end
  if #view == 0 then
    say(title .. " (nothing to show)")
    return
  end
  say(render(entries, title))
end

diftray.register_command("hist", function(tokens, scope)
  local action = tokens[2]

  -- `hist <n>` is a shorthand for `hist <n>` lines, and `hist 20` must not be
  -- read as an unknown subcommand.
  if action ~= nil and tonumber(action) ~= nil then
    local wanted = integer(action)
    if wanted < 1 then wanted = 1 end
    if wanted > 128 then wanted = 128 end
    shown = wanted
    show_list(newest_first(shown), "hist: last " .. plural(shown, "entry", "entries"))
    return
  end

  if action == nil then
    if #journal == 0 then
      say("hist: empty. Record one with: hist record spawn below")
      return
    end
    show_list(newest_first(shown), "hist: " .. plural(#journal, "entry", "entries") ..
                ", newest first")
    return
  end

  if action == "record" then
    local text = rest(tokens, 3)
    if text == "" then
      say("hist record needs a command, for example: hist record spawn below")
      return
    end
    local entry = remember(text)
    say("hist recorded " .. entry.text .. " (" ..
        (entry.inverse and "undo with " .. entry.inverse or entry.reason) .. ")")
    return
  end

  if action == "once" then
    local text = rest(tokens, 3)
    if text == "" then
      say("hist once needs a command, for example: hist once workspace 3")
      return
    end
    remember(text)
    diftray.command(text)
    say("hist ran and recorded " .. text)
    return
  end

  if action == "search" then
    local query = rest(tokens, 3)
    if query == "" then
      say("hist search needs something to look for, for example: hist search spawn")
      return
    end
    local needle = string.lower(query)
    local found = {}
    for i = #journal, 1, -1 do
      if string.find(string.lower(journal[i].text), needle, 1, true) then
        found[#found + 1] = journal[i]
      end
    end
    if #found == 0 then
      say("hist: nothing recorded mentions " .. query)
      return
    end
    show_list(found, "hist search " .. query .. " (" .. #found .. " of " ..
                #journal .. ")")
    return
  end

  if action == "undo" then
    local dry = false
    local steps = 1
    for i = 3, #tokens do
      if tokens[i] == "--dry" then
        dry = true
      else
        steps = integer(tokens[i]) or 1
      end
    end
    if steps < 1 then
      say("hist undo needs a positive step count")
      return
    end
    -- Collect first, dispatch second. Nothing is issued until the whole plan
    -- exists, so a request that cannot be satisfied in full changes nothing
    -- rather than reversing half of itself.
    local plan = {}
    local skipped = {}
    local at = #journal
    while #plan < steps and at >= 1 do
      local entry = journal[at]
      if entry.inverse then
        plan[#plan + 1] = entry
      else
        skipped[#skipped + 1] = entry.text
      end
      at = at - 1
    end
    if #plan < steps then
      say("hist undo: only " .. plural(#plan, "reversible step") ..
          " found in " .. plural(#journal, "entry", "entries") ..
          "; nothing was changed")
      return
    end
    local lines = {"hist undo:"}
    for i = #plan, 1, -1 do
      lines[#lines + 1] = "  " .. plan[i].text .. "  ->  " .. plan[i].inverse
    end
    if #skipped > 0 then
      lines[#lines + 1] = "  skipped: " .. table.concat(skipped, ", ")
    end
    if dry then
      lines[#lines + 1] = "  (dry run: nothing was dispatched)"
      say(table.concat(lines, "\n"))
      return
    end
    -- Queued, not dispatched: the queue drains on a later tick, which is what
    -- keeps this from recursing into the command bar. The cost is that each
    -- inverse reports its own result afterwards and replaces this plan on the
    -- status line. That is the right trade: the compositor's answer to "mux
    -- kill" is more informative than this extension's opinion of it.
    for i = #plan, 1, -1 do
      diftray.command(plan[i].inverse)
      -- The undo itself belongs in the journal, so `hist undo` twice does not
      -- do the same thing twice.
      remember(plan[i].inverse)
    end
    say(table.concat(lines, "\n"))
    return
  end

  if action == "drop" then
    local wanted = integer(tokens[3])
    if #view == 0 then
      say("hist drop: nothing is listed; run hist first")
      return
    end
    if wanted == nil or wanted < 1 or wanted > #view then
      say("hist drop needs an index between 1 and " .. #view)
      return
    end
    local text = view[wanted].text
    for i = #journal, 1, -1 do
      if journal[i].text == text then
        table.remove(journal, i)
        break
      end
    end
    say("hist dropped " .. text .. " (" .. plural(#journal, "entry", "entries") .. ")")
    return
  end

  if action == "clear" then
    journal = {}
    view = {}
    say("hist cleared")
    return
  end

  if action == "stats" then
    local reversible, distinct = 0, {}
    for i = 1, #journal do
      if journal[i].inverse then
        reversible = reversible + 1
      end
      distinct[journal[i].text] = true
    end
    local kinds = 0
    for _ in pairs(distinct) do
      kinds = kinds + 1
    end
    say(string.format("hist: %s, %d distinct, %d reversible, tick %d",
                      plural(#journal, "entry", "entries"), kinds, reversible,
                      event_tick))
    return
  end

  say("hist supports: record, once, search, undo, drop, clear, stats")
end)

-- The tick exists so entries can say when they happened relative to each other.
-- Events are the only clock an extension state has: `os` is not available.
diftray.on("view", function()
  event_tick = event_tick + 1
end)
diftray.on("input", function()
  event_tick = event_tick + 1
end)
diftray.on("frame", function()
  event_tick = event_tick + 1
end)
