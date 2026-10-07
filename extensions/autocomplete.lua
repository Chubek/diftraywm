-- DiftrayWM extension: autocomplete and command palette.
--
-- Install: cp extensions/autocomplete.lua ~/.config/diftraywm/extensions/
-- Command: :ac
--
-- What this can and cannot do is worth stating plainly. An extension runs in a
-- sandbox with no file system, no clock and no access to a cell's text, so it
-- cannot watch what you type and complete it for you. It can hold a database of
-- lines you run, rank them, search them and replay one. That is the useful
-- half, and it is the half a command bar can actually do from a script: type a
-- fragment, get the candidates back as a numbered list on the status line, and
-- replay the one you want.
--
-- The built-in command surface below is preloaded so the palette is useful
-- before you have taught it anything. `ac add` teaches it your own lines.
--
-- Recency is counted in compositor events rather than seconds, because `os` is
-- not available in an extension state. Every frame, layout update or keypress
-- advances the tick, so "recent" means "recently in this session".
--
--   :ac                        the most likely lines
--   :ac key                    lines starting with "key"
--   :ac find theme             lines containing "theme"
--   :ac top 5                  the five most used lines
--   :ac add keymap show        remember a line
--   :ac add "launch firefox"   quotes hold a multi-word line together
--   :ac show 2                 print entry 2 without running it
--   :ac pick 2                 run entry 2
--   :ac rm "launch firefox"    forget one line
--   :ac decay                  halve every score: age the whole table
--   :ac stats                  what is in the table
--   :ac clear                  forget everything learned, keep the builtins

local PAGE = 8            -- candidates shown for a search
local MAX_ENTRIES = 256   -- learned lines the table will hold
local WINDOW = 96         -- events after which a score has decayed by half
local SEED = 2            -- starting score of a builtin line

local tick = 0
local db = {}             -- {text, score, hits, seen, builtin}
local page = {}           -- the last page of results, so `pick` knows its indices

local function say(text)
  diftray.status(string.sub(text, 1, 4000))
end

-- Everything after token `first`, rejoined with single spaces. The Command Bar
-- strips quotes while tokenizing, so a quoted argument arrives here already
-- grouped and joins back into the line the user meant.
local function rest(tokens, first)
  local parts = {}
  for i = first, #tokens do
    parts[#parts + 1] = tokens[i]
  end
  return table.concat(parts, " ")
end

local function integer(value)
  local number = tonumber(value)
  if number == nil then
    return nil
  end
  return math.floor(number)
end

-- Nearly every message here reports a count, and "1 lines" in a status line
-- reads as a compositor bug rather than as this file's.
local function plural(count, singular, many)
  return tostring(count) .. " " .. (count == 1 and singular or (many or singular .. "s"))
end

-- The command surface the compositor actually implements, so the palette has
-- something to complete before it has been taught anything. Kept as bare
-- command lines: replaying one is a Command Bar dispatch.
local BUILTIN = {
  {"cell focus", "focus the selected cell"},
  {"cell move CELLID NCURSORID", "move a cell to another NCursor"},
  {"cell promote", "make the cell a full-screen TCursor"},
  {"cell restore", "return a TCursor to its place"},
  {"cell select", "enter and leave Cell Select Mode"},
  {"cell spawn above", "new cell above the focused one"},
  {"cell spawn below", "new cell below the focused one"},
  {"cheddar", "open the embedded editor"},
  {"cheddar close", "close the editor"},
  {"config eval EXPR", "evaluate a config expression"},
  {"config open", "open the config file in $EDITOR"},
  {"config path", "print the config file in use"},
  {"config reload", "re-read the config file"},
  {"config run EXPR", "run the commands an expression returns"},
  {"config vars", "list typed config variables"},
  {"cursors view docked", "list the GCursors docked here"},
  {"cursor assign ID F1", "bind a GCursor to a Quick Restore key"},
  {"cursor dock ID", "dock a GCursor into the Cursor Area"},
  {"cursor list ids", "list live GCursor identifiers"},
  {"cursor move ID cell CELLID", "re-own a GCursor"},
  {"cursor restore ID", "restore a docked GCursor"},
  {"dispatch PROGRAM", "launch through the editor's dispatch mode"},
  {"extension exec PATH", "load a Lua extension now"},
  {"extension list", "list loaded extensions"},
  {"help PAGE", "open a manual page"},
  {"help bookmark-open NAME", "open a help bookmark"},
  {"help bookmark-set NAME", "bookmark the current help page"},
  {"help find REGEX", "search the manual pages"},
  {"kill", "kill the focused cell"},
  {"keymap check PATH", "validate a keymap file without adopting it"},
  {"keymap chord <C-q>", "validate one chord spelling"},
  {"keymap path", "print the keymap INI in use"},
  {"keymap profile NAME", "switch keymap profile"},
  {"keymap reload", "re-read the keymap INI"},
  {"keymap reset", "return to the default keymap profile"},
  {"keymap show", "print the loaded keymap"},
  {"launch PROGRAM", "start a graphical program"},
  {"launcher lock", "lock the launch bar to the top"},
  {"launcher status", "report the launch bar state"},
  {"launcher toggle", "flip the launch bar lock"},
  {"launcher unlock", "release the launch bar"},
  {"move down", "move the selected cell down"},
  {"move left", "previous tab"},
  {"move right", "next tab"},
  {"move up", "move the selected cell up"},
  {"mux focus next", "focus the next multiplexer pane"},
  {"mux kill", "remove the focused pane"},
  {"mux list", "list multiplexer panes"},
  {"mux split horizontal", "split the stack side by side"},
  {"mux split vertical", "split the cell stack vertically"},
  {"mux zoom", "zoom the focused pane to a TCursor"},
  {"ncursor list", "list NCursor and cell identifiers"},
  {"ncursor new", "another NCursor on this output"},
  {"notelet close", "close the open notelet"},
  {"notelet list", "list available notelets"},
  {"notelet open NAME", "open a notelet"},
  {"notelet refresh", "re-run the open notelet"},
  {"output focus next", "focus the next monitor"},
  {"output list", "list monitor geometry"},
  {"output move NAME", "move this NCursor to a monitor"},
  {"output position NAME X Y", "place a monitor"},
  {"output rotate NAME 90", "rotate a monitor"},
  {"output scale NAME 1.5", "scale a monitor"},
  {"plugin list", "list loaded native plugins"},
  {"plugin load PATH", "load a native plugin"},
  {"plugin unload PATH", "unload a native plugin"},
  {"quit", "leave the session"},
  {"script source FILE.tsc", "run a Termscript file"},
  {"session restart", "re-execute the compositor in place"},
  {"session status", "report session state"},
  {"set shell PATH", "override the shell for cells"},
  {"spawn above", "new cell above the focused one"},
  {"spawn below", "new cell below the focused one"},
  {"tab next", "cycle tabs forward"},
  {"tab prev", "cycle tabs back"},
  {"terminal script FILE.tsc", "script the active terminal cell"},
  {"theme load FILE.css", "apply a CSS theme"},
  {"theme show", "list active theme properties"},
  {"workspace 1", "switch workspace (1-10)"},
}

local function find_index(text)
  for i = 1, #db do
    if db[i].text == text then
      return i
    end
  end
  return nil
end

local function find_record(text)
  local index = find_index(text)
  return index and db[index] or nil
end

-- A line's current standing: what it has earned, aged by how long ago it was
-- last used. Divergent decay is the whole point, so it is computed at search
-- time and the stored score is left alone.
local function weight(record)
  local age = tick - record.seen
  if age < 0 then
    age = 0
  end
  return record.score / (1 + age / WINDOW)
end

local function by_rank(a, b)
  local wa, wb = weight(a), weight(b)
  if wa ~= wb then
    return wa > wb
  end
  return a.text < b.text
end

local function subsequence(needle, haystack)
  -- Palette match: every character of the needle appears, in order. Case
  -- folded on both sides so "MS" finds "mux split".
  local target = string.lower(haystack)
  local at = 1
  for i = 1, #needle do
    local found = string.find(target, string.lower(string.sub(needle, i, i)), at, true)
    if not found then
      return false
    end
    at = found + 1
  end
  return true
end

-- Try progressively looser matches and stop at the first one that finds
-- anything, which is what makes a single-word query enough. A prefix match
-- always outranks a substring match, which always outranks a gapped one.
local function search(query)
  if query == "" then
    local all = {}
    for i = 1, #db do
      all[#all + 1] = db[i]
    end
    table.sort(all, by_rank)
    return all, "all"
  end
  local lower = string.lower(query)
  for _, mode in ipairs({"prefix", "any", "fuzzy"}) do
    local found = {}
    for i = 1, #db do
      local text = db[i].text
      local haystack = string.lower(text)
      local keep
      if mode == "prefix" then
        keep = string.sub(haystack, 1, #lower) == lower
      elseif mode == "any" then
        keep = string.find(haystack, lower, 1, true) ~= nil
      else
        keep = subsequence(lower, text)
      end
      if keep then
        found[#found + 1] = db[i]
      end
    end
    if #found > 0 then
      table.sort(found, by_rank)
      return found, mode
    end
  end
  return {}, "none"
end

local function note_of(record)
  if record.builtin then
    return record.hint or ""
  end
  if record.hits > 1 then
    return "used " .. record.hits .. " times"
  end
  return "learned"
end

local function render(records, title)
  local lines = {title}
  for i = 1, #page do
    lines[#lines + 1] = string.format("  %d) %-24s %s", i, page[i].text, note_of(page[i]))
  end
  if #page > #records then
    lines[#lines + 1] = string.format("  ... %d more", #records - #page)
  end
  return table.concat(lines, "\n")
end

local function record_use(record)
  record.hits = record.hits + 1
  record.seen = tick
  record.score = record.score + 1
end

local function learn(text)
  local existing = find_record(text)
  if existing then
    if existing.builtin then
      -- A builtin line becomes a learned line the first time it is used; the
      -- hint is dropped because it describes the command, not the usage.
      existing.builtin = false
      existing.hint = nil
      existing.score = existing.score + 1
    else
      record_use(existing)
    end
    return existing, false
  end
  if #db >= MAX_ENTRIES then
    -- Drop the least useful learned line rather than the table. A builtin is
    -- never dropped, so the palette keeps a floor even when full.
    local worst, worst_index
    for i = 1, #db do
      if not db[i].builtin and (worst == nil or weight(db[i]) < weight(worst)) then
        worst, worst_index = db[i], i
      end
    end
    if worst == nil then
      return nil, false
    end
    table.remove(db, worst_index)
  end
  local record = {text = text, score = 3, hits = 1, seen = tick}
  db[#db + 1] = record
  return record, true
end

for _, entry in ipairs(BUILTIN) do
  db[#db + 1] = {text = entry[1], hint = entry[2], score = SEED, hits = 0,
                 seen = 0, builtin = true}
end

diftray.register_command("ac", function(tokens, scope)
  local action = tokens[2]
  if action == nil or action == "" then
    -- A bare `ac` is a search for everything, not an error.
    local records, mode = search("")
    page = {}
    for i = 1, math.min(PAGE, #records) do
      page[#page + 1] = records[i]
    end
    say(render(records, "ac: " .. plural(#db, "line") .. ", most likely first"))
    return
  end

  if action == "add" then
    local text = rest(tokens, 3)
    if text == "" then
      say("ac add needs a line, for example: ac add keymap show")
      return
    end
    local record, created = learn(text)
    if record == nil then
      say("ac table is full of builtin lines; nothing was forgotten")
      return
    end
    say("ac " .. (created and "learned " or "already knew ") .. record.text ..
        " (" .. plural(#db, "line") .. ")")
    return
  end

  if action == "rm" then
    local text = rest(tokens, 3)
    local record = find_record(text)
    if record == nil then
      say("ac does not know: " .. text)
      return
    end
    if record.builtin then
      say("ac keeps " .. text .. ": it is part of the builtin command surface")
      return
    end
    table.remove(db, find_index(text))
    say("ac forgot " .. text .. " (" .. plural(#db, "line") .. ")")
    return
  end

  if action == "decay" then
    for i = 1, #db do
      db[i].score = db[i].score / 2
      if db[i].score < 0.25 and not db[i].builtin then
        db[i].score = 0.25
      end
    end
    say("ac aged every score by half")
    return
  end

  if action == "top" then
    local wanted = integer(tokens[3]) or PAGE
    if wanted < 1 then wanted = 1 end
    if wanted > 64 then wanted = 64 end
    local all = {}
    for i = 1, #db do
      if not db[i].builtin then
        all[#all + 1] = db[i]
      end
    end
    table.sort(all, by_rank)
    page = {}
    for i = 1, math.min(wanted, #all) do
      page[#page + 1] = all[i]
    end
    if #page == 0 then
      say("ac top: nothing learned yet; ac add a line you run often")
      return
    end
    say(render(all, "ac: your " .. plural(#page, "most used line")))
    return
  end

  if action == "stats" then
    local learned, builtin, hits, oldest = 0, 0, 0, 0
    for i = 1, #db do
      if db[i].builtin then
        builtin = builtin + 1
      else
        learned = learned + 1
        if db[i].seen < oldest or oldest == 0 then
          oldest = db[i].seen
        end
      end
      hits = hits + db[i].hits
    end
    say(string.format("ac: %s (%d builtin, %d learned), %d uses, tick %d",
                      plural(#db, "line"), builtin, learned, hits, tick))
    return
  end

  if action == "clear" then
    local kept = {}
    for i = 1, #db do
      if db[i].builtin then
        kept[#kept + 1] = db[i]
      end
    end
    db = kept
    say("ac forgot every learned line")
    return
  end

  if action == "show" then
    local wanted = integer(tokens[3])
    if #page == 0 then
      say("ac show: nothing is listed; search first, for example: ac key")
      return
    end
    if wanted == nil or wanted < 1 or wanted > #page then
      say("ac show needs an index between 1 and " .. #page)
      return
    end
    say(page[wanted].text)
    return
  end

  if action == "pick" then
    local wanted = integer(tokens[3])
    if #page == 0 then
      say("ac pick: nothing is listed; search first, for example: ac key")
      return
    end
    if wanted == nil or wanted < 1 or wanted > #page then
      say("ac pick needs an index between 1 and " .. #page)
      return
    end
    local record = page[wanted]
    record_use(record)
    diftray.command(record.text)
    say("ac: " .. record.text)
    return
  end

  -- Anything else is a query. `ac find <text>` searches anywhere in a line,
  -- which is what you want when you remember the words but not the order.
  local query
  if action == "find" then
    query = rest(tokens, 3)
  else
    query = rest(tokens, 2)
  end
  if query == "" then
    say("ac find needs something to look for")
    return
  end
  local records, mode = search(query)
  page = {}
  for i = 1, math.min(PAGE, #records) do
    page[#page + 1] = records[i]
  end
  if #page == 0 then
    say("ac: nothing matches " .. query)
    return
  end
  say(render(records, "ac " .. query .. " (" .. mode .. " match, " ..
              plural(#records, "hit") .. ")"))
end)

-- The tick is the only clock available here. Each event advances it, which is
-- what makes `ac decay` and the recency half of the ranking mean anything.
diftray.on("view", function()
  tick = tick + 1
end)
diftray.on("input", function()
  tick = tick + 1
end)
diftray.on("frame", function()
  tick = tick + 1
end)
