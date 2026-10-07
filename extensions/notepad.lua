-- DiftrayWM extension: cell and cursor notebook.
--
-- Install: cp extensions/notepad.lua ~/.config/diftraywm/extensions/
-- Command: :note
--
-- A written record of what is where. Cells and GCursors have identifiers, the
-- Command Bar can move them between stacks and Cursor Areas, and nothing keeps
-- a note of the arrangement you liked. This does: `note watch` records the
-- identifiers you name, and `note where` prints them back as a list of
-- `cell move` and `cursor move` commands.
--
-- The reason it stores commands rather than trying to read the compositor is
-- that an extension cannot read the compositor's state. `cell list ids` and
-- `cursor list ids` report the truth, and `note` composes that truth with what
-- you recorded, so a stale note shows up as a difference rather than as a lie.
--
--   :note                the notebook
--   :note ls             ask the compositor for the live identifiers
--   :note watch 3af1 vim     remember that cell 3af1 is the vim cell
--   :note watch-cursor quail browser   and that quail is the browser
--   :note pin quail F1    bind the browser to a Quick Restore key
--   :note pin browser F1  the same, by the name above
--   :note where          the restore commands, ready to run
--   :note find vim       search names
--   :note clear
--
-- Identifiers are the short hashes the compositor prints, so `note ls` followed
-- by `note watch <id> <name>` is the whole workflow: read the identifier off the
-- status line, name it once, refer to it by name from then on.

local MAX_ENTRIES = 64
local PAGE = 8

local cells = {}    -- {id = "3af1", name = "vim"}
local cursors = {}  -- {id = "quail", name = "browser"}
local page = {}

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

local function put(table_, id, name)
  for i = 1, #table_ do
    if table_[i].id == id then
      table_[i].name = name
      return i, false
    end
  end
  if #table_ >= MAX_ENTRIES then
    return nil, "full"
  end
  table_[#table_ + 1] = {id = id, name = name}
  return #table_, true
end

local function drop(table_, id)
  for i = 1, #table_ do
    if table_[i].id == id then
      table.remove(table_, i)
      return true
    end
  end
  return false
end

local function render(entries, title)
  local lines = {title}
  for i = 1, #page do
    lines[#lines + 1] = string.format("  %d) %-10s %s", i, page[i].id, page[i].name)
  end
  if #page < #entries then
    lines[#lines + 1] = string.format("  ... %d more", #entries - #page)
  end
  return table.concat(lines, "\n")
end

local function show_table(table_, title)
  page = {}
  for i = 1, math.min(PAGE, #table_) do
    page[#page + 1] = table_[i]
  end
  if #page == 0 then
    say(title .. " (empty)")
    return
  end
  say(render(table_, title))
end

diftray.register_command("note", function(tokens, scope)
  local action = tokens[2]

  if action == nil then
    local lines = {"note: " .. plural(#cells, "cell") .. ", " ..
                   plural(#cursors, "cursor") .. " on record"}
    for i = 1, #cells do
      lines[#lines + 1] = "  cell   " .. cells[i].id .. "  " .. cells[i].name
    end
    for i = 1, #cursors do
      lines[#lines + 1] = "  cursor " .. cursors[i].id .. "  " .. cursors[i].name
    end
    if #cells == 0 and #cursors == 0 then
      lines[#lines + 1] = "  note ls prints the live identifiers to record"
    end
    say(table.concat(lines, "\n"))
    return
  end

  if action == "ls" then
    -- The compositor is the only source of truth about what exists. Queued
    -- rather than asked for, because a status line has one line and this needs
    -- two answers.
    diftray.command("ncursor list")
    say("note ls: the live NCursor and cell identifiers are above")
    return
  end

  if action == "cursors" then
    diftray.command("cursor list ids")
    say("note cursors: the live GCursor identifiers are above")
    return
  end

  if action == "watch" then
    local id = tokens[3]
    if id == nil then
      say("note watch needs an identifier, for example: note watch 3af1 vim")
      return
    end
    local name = rest(tokens, 4)
    if name == "" then
      say("note watch needs a name for " .. id .. ", for example: note watch " ..
          id .. " vim")
      return
    end
    local at, created = put(cells, id, name)
    if at == nil then
      say("note holds at most " .. MAX_ENTRIES .. " cells")
      return
    end
    say("note " .. (created and "watching " or "renamed ") .. "cell " .. id ..
        " as " .. name)
    return
  end

  if action == "watch-cursor" then
    local id = tokens[3]
    if id == nil then
      say("note watch-cursor needs an identifier, for example: note watch-cursor quail browser")
      return
    end
    local name = rest(tokens, 4)
    if name == "" then
      say("note watch-cursor needs a name for " .. id)
      return
    end
    local at, created = put(cursors, id, name)
    if at == nil then
      say("note holds at most " .. MAX_ENTRIES .. " cursors")
      return
    end
    say("note " .. (created and "watching " or "renamed ") .. "cursor " .. id ..
        " as " .. name)
    return
  end

  if action == "unwatch" then
    local id = tokens[3]
    if id == nil then
      say("note unwatch needs an identifier")
      return
    end
    if drop(cells, id) then
      say("note stopped watching cell " .. id)
    elseif drop(cursors, id) then
      say("note stopped watching cursor " .. id)
    else
      say("note does not have " .. id)
    end
    return
  end

  if action == "where" then
    if #cells == 0 and #cursors == 0 then
      say("note where: nothing on record yet")
      return
    end
    local lines = {"note where: your arrangement as commands"}
    for i = 1, #cells do
      lines[#lines + 1] = "  " .. cells[i].id .. "  " .. cells[i].name
    end
    lines[#lines + 1] = "  Identifiers do not carry their NCursor, so a restore " ..
                        "needs the destination:"
    lines[#lines + 1] = "  cell move <cell-id> <ncursor-id>"
    lines[#lines + 1] = "  cursor move <cursor-id> cell|ncursor <destination-id>"
    say(table.concat(lines, "\n"))
    return
  end

  if action == "pin" then
    local id = tokens[3]
    local key = tokens[4]
    if id == nil or key == nil then
      say("note pin needs a cursor identifier and a key, for example: " ..
          "note pin quail F1")
      return
    end
    local known = nil
    for i = 1, #cursors do
      if cursors[i].id == id or string.lower(cursors[i].name) == string.lower(id) then
        known = cursors[i]
        break
      end
    end
    local target = known and known.id or id
    local where = known and (" (" .. known.name .. ")") or ""
    -- A GCursor's identifier is not guessable, so a name works here as an
    -- alias. `cursor assign` is the one operation a notebook can perform
    -- without knowing where anything is: the key is an absolute slot.
    diftray.command("cursor assign " .. target .. " " .. key)
    say("note pinned " .. target .. where .. " to Quick Restore " .. key)
    return
  end

  if action == "find" then
    local query = rest(tokens, 3)
    if query == "" then
      say("note find needs something to look for")
      return
    end
    local needle = string.lower(query)
    local found = {}
    for i = 1, #cells do
      if string.find(string.lower(cells[i].name), needle, 1, true) then
        found[#found + 1] = cells[i]
      end
    end
    if #found == 0 then
      say("note find: no cell is named " .. query)
      return
    end
    show_table(found, "note find " .. query .. " (" .. #found .. " cells)")
    return
  end

  if action == "cells" then
    show_table(cells, "note: " .. plural(#cells, "cell") .. " on record")
    return
  end

  if action == "clear" then
    local count = #cells + #cursors
    cells = {}
    cursors = {}
    page = {}
    say("note cleared " .. plural(count, "entry", "entries") ..
        ". Nothing on screen moved.")
    return
  end

  say("note supports: watch, watch-cursor, unwatch, pin, where, find, ls, " ..
      "cursors, cells, clear")
end)
