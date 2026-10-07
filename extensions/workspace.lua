-- DiftrayWM extension: workspace board.
--
-- Install: cp extensions/workspace.lua ~/.config/diftraywm/extensions/
-- Command: :ws
--
-- Workspaces are numbered 1 to 10 and switching is one command, which is easy.
-- Remembering which one holds what is not, and that is what this is for: a named
-- index of the ten workspaces, each with a label, so `ws` can print the board
-- and `ws mark` can record what a workspace is for.
--
-- There is no way for an extension to read the compositor's workspace state, so
-- this board is a record of what *you* said, not a mirror of what the compositor
-- did. It deliberately never claims otherwise: `ws board` says which workspaces
-- are labelled and which are blank rather than inventing a status for them.
-- Numbers can still be switched directly with `ws go 4` or the usual
-- `workspace 4`.
--
--   :ws                    the board
--   :ws mark 3 editor      label workspace 3
--   :ws unmark 3           remove the label
--   :ws go 4               switch workspace 4
--   :ws next / :ws prev    the next or previous labelled workspace
--   :ws find editor        labelled workspaces mentioning a word
--   :ws drop 3             label workspace 3 and switch there
--   :ws clear              forget every label

local MAX_LABEL = 40
local LAST = 10          -- workspaces are numbered 1..10; there is no eleventh

local labels = {}        -- [3] = "editor"
local order = {}         -- workspace numbers that carry a label, ascending
local here = nil         -- the last workspace this extension was told about
local page = {}          -- the last `find` result

local function say(text)
  diftray.status(string.sub(text, 1, 4000))
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

-- Workspaces are 1..10 and 10 is spelled 10, not 0. Meta+0 switches to
-- workspace 10 while Meta+1..9 are the rest, so the board has to agree with the
-- keybindings rather than with a 0-based habit.
local function workspace_number(value)
  local number = integer(value)
  if number == nil or number < 1 or number > LAST then
    return nil, "a workspace is 1 to " .. LAST
  end
  return number
end

local function label_for(number)
  return labels[number] or ""
end

local function refresh_order()
  order = {}
  for number = 1, LAST do
    if labels[number] ~= nil then
      order[#order + 1] = number
    end
  end
end

local function board()
  local lines = {"ws: workspace board" .. (here and (", last touched " .. here) or "")}
  for number = 1, LAST do
    local label = labels[number]
    local marker = (here == number) and "*" or " "
    local text
    if label == nil then
      text = "(unlabelled)"
    else
      text = label
    end
    lines[#lines + 1] = string.format("  %s %2d  %s", marker, number, text)
  end
  if #order == 0 then
    lines[#lines + 1] = "  label one with: ws mark 1 what goes here"
  end
  return table.concat(lines, "\n")
end

local function find_label(needle)
  local lower = string.lower(needle)
  local found = {}
  for i = 1, #order do
    local number = order[i]
    if string.find(string.lower(labels[number]), lower, 1, true) then
      found[#found + 1] = number
    end
  end
  return found
end

diftray.register_command("ws", function(tokens, scope)
  local action = tokens[2]

  if action == nil then
    say(board())
    return
  end

  if action == "board" then
    say(board())
    return
  end

  if action == "mark" then
    local number, why = workspace_number(tokens[3])
    if number == nil then
      say("ws mark: " .. why)
      return
    end
    local label = ""
    for i = 4, #tokens do
      if label ~= "" then
        label = label .. " "
      end
      label = label .. tokens[i]
    end
    if label == "" then
      say("ws mark needs a label, for example: ws mark 3 editor")
      return
    end
    if #label > MAX_LABEL then
      say("ws mark: a label longer than " .. MAX_LABEL ..
          " characters stops being a label")
      return
    end
    local previous = labels[number]
    labels[number] = label
    refresh_order()
    if previous ~= nil then
      say("ws " .. number .. " relabelled from \"" .. previous .. "\" to \"" ..
          label .. "\"")
    else
      say("ws " .. number .. " is now \"" .. label .. "\" (" ..
          plural(#order, "labelled workspace") .. ")")
    end
    return
  end

  if action == "unmark" then
    local number, why = workspace_number(tokens[3])
    if number == nil then
      say("ws unmark: " .. why)
      return
    end
    if labels[number] == nil then
      say("ws " .. number .. " has no label")
      return
    end
    local was = labels[number]
    labels[number] = nil
    refresh_order()
    say("ws " .. number .. " lost the label \"" .. was .. "\"")
    return
  end

  if action == "go" then
    local number, why = workspace_number(tokens[3])
    if number == nil then
      say("ws go: " .. why)
      return
    end
    here = number
    diftray.command("workspace " .. number)
    local label = labels[number]
    say("ws " .. number .. (label and (" \"" .. label .. "\"") or " (unlabelled)"))
    return
  end

  if action == "drop" or action == "jump" then
    -- Switch to a workspace and leave it ready for what the label says. The
    -- board cannot create anything there, so it says what it did instead of
    -- implying that a label spawns a layout.
    local number, why = workspace_number(tokens[3])
    if number == nil then
      say("ws " .. action .. ": " .. why)
      return
    end
    here = number
    diftray.command("workspace " .. number)
    local label = labels[number]
    if label == nil then
      say("ws " .. number .. " has no label; label it with ws mark " .. number ..
          " <text>")
    else
      say("ws " .. number .. ": " .. label)
    end
    return
  end

  if action == "next" or action == "prev" then
    if #order == 0 then
      say("ws " .. action .. ": no workspace is labelled yet")
      return
    end
    if here == nil or labels[here] == nil then
      -- Without a known current workspace there is no correct direction to step
      -- in, so start at an end rather than pretend.
      local number = order[action == "next" and 1 or #order]
      here = number
      diftray.command("workspace " .. number)
      say("ws " .. action .. ": " .. number .. " " .. labels[number] ..
          " (no current workspace was recorded)")
      return
    end
    local at
    for i = 1, #order do
      if order[i] == here then
        at = i
        break
      end
    end
    local next_at = action == "next" and at + 1 or at - 1
    if next_at < 1 or next_at > #order then
      say("ws " .. action .. ": " .. here .. " is the last labelled workspace")
      return
    end
    local number = order[next_at]
    here = number
    diftray.command("workspace " .. number)
    say("ws " .. number .. ": " .. labels[number])
    return
  end

  if action == "find" then
    local query = ""
    for i = 3, #tokens do
      if query ~= "" then
        query = query .. " "
      end
      query = query .. tokens[i]
    end
    if query == "" then
      say("ws find needs something to look for, for example: ws find editor")
      return
    end
    local found = find_label(query)
    if #found == 0 then
      say("ws find: no label mentions " .. query)
      return
    end
    page = found
    local lines = {"ws find " .. query .. " (" .. #found .. " of " .. #order .. "):"}
    for i = 1, #page do
      lines[#lines + 1] = "  " .. page[i] .. "  " .. labels[page[i]]
    end
    say(table.concat(lines, "\n"))
    return
  end

  if action == "clear" then
    local count = #order
    labels = {}
    refresh_order()
    say("ws cleared " .. plural(count, "label"))
    return
  end

  say("ws supports: board, mark, unmark, go, next, prev, find, clear")
end)

-- A view event means the layout changed somewhere. It does not say which
-- workspace became current, so nothing here is inferred from it: guessing would
-- put the board's marker on the wrong number and quietly mislead.
--
-- This extension therefore registers no event hook. The compositor knows the
-- current workspace; this board knows only what the user told it, and the header
-- says which workspace was last touched rather than implying it is reading
-- live state.
