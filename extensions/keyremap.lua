-- DiftrayWM extension: key remapping workbench.
--
-- Install: cp extensions/keyremap.lua ~/.config/diftraywm/extensions/
-- Command: :remap
--
-- Every key in DiftrayWM lives in one keyd-format INI file, and that file is
-- the only place keys are configured. This extension is a front end for
-- working on that file without leaving the Command Bar: it keeps a table of
-- the chords you have declared, checks each one against the spelling the
-- compositor's own parser accepts, and writes the finished INI out through a
-- plain `launch` of the editor of your choice.
--
-- What it deliberately does not do is edit the file behind your back. An
-- extension has no file system, so it cannot write keymap.ini, and it cannot
-- read one either. The compositor owns loading and validating the file; this
-- owns assembling it. `remap write` therefore hands you the text and the
-- command to run, and `remap check` asks the compositor to validate a file
-- you have already saved. Keeping the compositor the single writer is what
-- makes a broken chord a load-time error with a line number instead of a key
-- that silently stops working.
--
--   :remap                       the binding table
--   :remap add <M-f3> spawn below  declare a chord
--   :remap find key               search chords and actions
--   :remap check <M-f3>           validate one chord's spelling
--   :remap profile work           write a [profile] section, plus the
--                                 [init] prefix and action that enter it
--   :remap write                  print the whole INI to write out
--   :remap edit                   open the INI in $EDITOR
--   :remap show                   ask the compositor what it loaded
--   :remap reload                 re-read the INI after editing it
--   :remap diff                   compare this table with the loaded keymap
--   :remap clear                  empty the table

local MAX_BINDINGS = 256
local MAX_PREVIEW = 8

local bindings = {}   -- {chord = "<M-f3>", action = "Diftray(spawn below)"},
                      -- held in declaration order because that is the order
                      -- the INI is written in
local profile = "profile-1"
local prefix_chord = "<C-g>"
local meta_chord = "<C-q>"
local dirty = false
local page = {}       -- last listing, for `remap run`

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

-- "1 chord" / "2 chords". Counts appear in nearly every message here and a
-- pluralisation bug in a status line reads as a bug in the compositor.
local function plural(count, singular, many)
  return tostring(count) .. " " .. (count == 1 and singular or (many or singular .. "s"))
end

local function index_of(chord)
  for i = 1, #bindings do
    if bindings[i].chord == chord then
      return i
    end
  end
  return nil
end

-- The named keys keymap.ini accepts, mirroring kNamedKeys in
-- src/keymap/Keymap.cpp. The check is local rather than delegated to
-- `keymap chord` because a table entry should be rejected while it is being
-- typed, not after it has been written into a file.
local NAMED = {}
for _, name in ipairs({
  "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
  "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
  "minus", "equal", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p",
  "leftbrace", "rightbrace", "backslash", "a", "s", "d", "f", "g", "h", "j",
  "k", "l", "semicolon", "apostrophe", "z", "x", "c", "v", "b", "n", "m",
  "comma", "dot", "slash", "space", "grave",
  "enter", "return", "tab", "backspace", "esc", "escape", "del", "delete",
  "ins", "insert", "home", "end", "pageup", "pagedown", "up", "down", "left",
  "right", "menu", "capslock", "numlock", "scrolllock", "prtsc", "pause",
  "leftshift", "rightshift", "leftctrl", "rightctrl", "leftalt", "rightalt",
  "leftlogo", "rightlogo",
  "-", "=", "[", "{", "]", "}", "\\", "|", ";", ":", "'", '"', ",", "<", ".",
  ">", "/", "?", "`", "~", " ",
}) do
  NAMED[name] = true
end

local function valid_chord(text)
  if text == "" then
    return false, "empty chord"
  end
  local body = text
  if string.sub(body, 1, 1) == "<" and string.sub(body, -1) == ">" then
    body = string.sub(body, 2, #body - 1)
  end
  if body == "" then
    return false, "empty chord"
  end
  -- Modifiers are consumed left to right, exactly as parse_key_chord does: a
  -- single character followed by '-' or '+'. Only a body of three or more can
  -- start with one, which is why 'c-' alone is never a modifier.
  local seen = {}
  while #body > 2 do
    local letter = string.lower(string.sub(body, 1, 1))
    local separator = string.sub(body, 2, 2)
    if separator ~= "-" and separator ~= "+" then
      break
    end
    if letter ~= "c" and letter ~= "a" and letter ~= "s" and
       letter ~= "m" and letter ~= "g" then
      break
    end
    if seen[letter] then
      return false, "duplicate modifier in chord"
    end
    seen[letter] = true
    body = string.sub(body, 3)
  end
  if body == "" then
    return false, "chord has modifiers and no key"
  end
  if #body == 1 or NAMED[string.lower(body)] then
    return true
  end
  return false, "unknown key: " .. string.lower(body)
end

-- Only the six actions keymap.ini defines, with the spelling the compositor
-- writes back. An unknown action is a load-time error there, so it is caught
-- here instead.
local ACTIONS = {
  diftray = "Diftray", exec = "Exec", typeout = "Typeout",
  trigger = "Trigger", remap = "Remap", ignore = "Ignore",
}

local function split_action(text)
  local opening = string.find(text, "%(")
  if opening == nil then
    -- A bare line is a Command Bar command. That is what almost every chord
    -- wants, and writing it as `remap add <M-f3> spawn below` instead of
    -- spelling out Diftray() each time is worth the one convention.
    if string.find(text, "%s") == nil then
      return nil, "action needs an argument, for example Diftray(spawn below)"
    end
    return "Diftray(" .. text .. ")"
  end
  if string.sub(text, -1) ~= ")" then
    return nil, "action must be Name(argument), for example Diftray(spawn below)"
  end
  local name = string.lower(string.sub(text, 1, opening - 1))
  local canonical = ACTIONS[name]
  if canonical == nil then
    return nil, "unknown action: " .. name ..
                " (diftray, exec, typeout, trigger, remap, ignore)"
  end
  local argument = string.sub(text, opening + 1, #text - 1)
  if name ~= "ignore" and argument == "" then
    return nil, name .. " requires an argument"
  end
  return canonical .. "(" .. argument .. ")"
end

-- Actions that only mean something inside the compositor. diftrayremap works at
-- the evdev layer, so it honours Ignore(), Remap() and Trigger() and lets these
-- three pass through untouched; declaring them here without a warning would
-- suggest they work system-wide when they do not.
local COMPOSITOR_ONLY = {
  Diftray = true, Exec = true, Typeout = true,
}

local function local_only_warning(action)
  local name = string.match(action, "^(%a+)")
  if name ~= nil and COMPOSITOR_ONLY[name] then
    return " (compositor-only: diftrayremap passes this key through)"
  end
  return ""
end

local function render(records, title)
  local lines = {title}
  for i = 1, #page do
    local entry = page[i]
    lines[#lines + 1] = string.format("  %d) %-12s = %s", i, entry.chord, entry.action)
  end
  if #page < #records then
    lines[#lines + 1] = string.format("  ... %d more", #records - #page)
  end
  return table.concat(lines, "\n")
end

-- The INI this table describes, in the format keyd and keymap.ini both read.
-- Rendered here rather than written to disk because an extension has no file
-- system; `remap write` prints it and `remap edit` opens the editor on it.
local function render_ini()
  local lines = {
    "# Written by the DiftrayWM remap extension.",
    "# Validate before adopting: keymap check " .. "keymap.ini",
    "",
    "[init]",
    "prefix = " .. prefix_chord,
    "action = Trigger(" .. profile .. ")",
    "",
    "[meta]",
    "prefix = " .. meta_chord,
    "",
    "[" .. profile .. "]",
  }
  for i = 1, #bindings do
    -- No inline comments here. The rendered text is what the user pastes into
    -- keymap.ini, and a trailing remark on a binding line is a parse error, not
    -- a note.
    lines[#lines + 1] = bindings[i].chord .. " = " .. bindings[i].action
  end
  if #bindings > 0 then
    -- The one place a remark belongs: a comment line of its own.
    lines[#lines + 1] = ""
    lines[#lines + 1] = "# Diftray(), Exec() and Typeout() are compositor"
    lines[#lines + 1] = "# actions. diftrayremap honours Ignore(), Remap() and"
    lines[#lines + 1] = "# Trigger() at the evdev layer and passes these through."
  end
  if #bindings == 0 then
    lines[#lines + 1] = "# no chords declared yet"
  end
  return table.concat(lines, "\n")
end

diftray.register_command("remap", function(tokens, scope)
  local action = tokens[2]

  if action == nil then
    local entries = {}
    for i = 1, #bindings do
      entries[#entries + 1] = bindings[i]
    end
    page = {}
    for i = 1, math.min(MAX_PREVIEW, #entries) do
      page[#page + 1] = entries[i]
    end
    say(render(entries, "remap: " .. plural(#bindings, "chord") .. " for [" ..
                profile .. "]" .. (dirty and ", unapplied" or "")))
    return
  end

  if action == "add" then
    local chord = tokens[3]
    local text = rest(tokens, 4)
    if chord == nil or text == "" then
      say("remap add needs a chord and an action, for example: " ..
          "remap add <M-f3> spawn below")
      return
    end
    local ok, why = valid_chord(chord)
    if not ok then
      say("remap add: " .. why)
      return
    end
    local spelling, action_error = split_action(text)
    if spelling == nil then
      say("remap add: " .. action_error)
      return
    end
    local existing = index_of(chord)
    if existing ~= nil then
      bindings[existing].action = spelling
      dirty = true
      say("remap " .. chord .. " now maps to " .. spelling ..
          local_only_warning(spelling) .. " (edited)")
      return
    end
    if #bindings >= MAX_BINDINGS then
      say("remap holds at most " .. MAX_BINDINGS .. " chords")
      return
    end
    bindings[#bindings + 1] = {chord = chord, action = spelling}
    dirty = true
    say("remap " .. chord .. " = " .. spelling .. local_only_warning(spelling) ..
        " (" .. plural(#bindings, "chord") .. ")")
    return
  end

  if action == "rm" then
    local chord = tokens[3]
    local at = chord and index_of(chord) or nil
    if at == nil then
      say("remap rm needs a chord that is in the table, for example: remap rm <M-f3>")
      return
    end
    table.remove(bindings, at)
    dirty = true
    say("remap removed " .. chord .. " (" .. plural(#bindings, "chord") .. ")")
    return
  end

  if action == "check" then
    local chord = tokens[3]
    if chord == nil then
      say("remap check needs a chord, for example: remap check <M-f3>")
      return
    end
    local ok, why = valid_chord(chord)
    if not ok then
      say("remap check: " .. chord .. " is not a chord this keymap accepts: " .. why)
      return
    end
    -- The compositor's parser is the authority on the result, so ask it. Its
    -- answer, including the key code, lands on the status line after this.
    diftray.command("keymap chord " .. chord)
    say("remap check: " .. chord .. " is spelled correctly")
    return
  end

  if action == "find" then
    local query = rest(tokens, 3)
    if query == "" then
      say("remap find needs something to look for")
      return
    end
    local needle = string.lower(query)
    local found = {}
    for i = 1, #bindings do
      local entry = bindings[i]
      local haystack = string.lower(entry.chord .. " " .. entry.action)
      if string.find(haystack, needle, 1, true) then
        found[#found + 1] = entry
      end
    end
    page = {}
    for i = 1, math.min(MAX_PREVIEW, #found) do
      page[#page + 1] = found[i]
    end
    if #page == 0 then
      say("remap: no chord or action mentions " .. query)
      return
    end
    say(render(found, "remap find " .. query .. " (" .. #found .. " found)"))
    return
  end

  if action == "run" then
    local wanted = tonumber(tokens[3])
    local at = wanted and math.floor(wanted) or nil
    if at == nil then
      say("remap run needs an index, for example: remap run 1")
      return
    end
    -- Falls back to the whole table when no listing is showing, because an
    -- index into "the chords I declared" is what the user means either way.
    local choices = #page > 0 and page or bindings
    if at < 1 or at > #choices then
      say("remap run: index " .. at .. " is outside the " ..
          plural(#choices, "chord") .. " available")
      return
    end
    local entry = choices[at]
    local action_name, argument = string.match(entry.action, "^(%a+)%((.*)%)")
    if action_name == "Diftray" then
      diftray.command(argument)
      say("remap run " .. entry.chord .. ": " .. argument)
    else
      -- Typeout, Exec and the rest are the compositor's business, not
      -- something an extension can perform on its own.
      say("remap run: " .. action_name ..
          " runs inside the compositor; bind it in the INI instead")
    end
    return
  end

  if action == "prefix" then
    local chord = tokens[3]
    if chord == nil then
      say("remap prefix is " .. prefix_chord)
      return
    end
    local ok, why = valid_chord(chord)
    if not ok then
      say("remap prefix: " .. why)
      return
    end
    if chord == meta_chord then
      say("remap prefix: that is the [meta] chord; giving one key two jobs " ..
          "means whichever runs first swallows it")
      return
    end
    prefix_chord = chord
    dirty = true
    say("remap [init] prefix is now " .. chord)
    return
  end

  if action == "metaprefix" then
    local chord = tokens[3]
    if chord == nil then
      say("remap [meta] prefix is " .. meta_chord)
      return
    end
    local ok, why = valid_chord(chord)
    if not ok then
      say("remap metaprefix: " .. why)
      return
    end
    if chord == prefix_chord then
      say("remap metaprefix: that is the [init] prefix; the two layers must " ..
          "answer to different keys")
      return
    end
    meta_chord = chord
    dirty = true
    say("remap [meta] prefix is now " .. chord)
    return
  end

  if action == "profile" then
    local wanted = tokens[3]
    if wanted == nil then
      say("remap profile is [" .. profile .. "]")
      return
    end
    if string.find(wanted, "[%s%[%]]") then
      say("remap profile: a section name cannot contain spaces or brackets")
      return
    end
    profile = wanted
    dirty = true
    say("remap writes section [" .. profile .. "]; use keymap profile " ..
        profile .. " after reloading to enter it")
    return
  end

  if action == "write" then
    local text = render_ini()
    say("remap INI for [" .. profile .. "] (" .. plural(#bindings, "chord") ..
        "):\n" .. text)
    return
  end

  if action == "edit" then
    -- $EDITOR is honoured by the compositor's own config-open path, so reuse
    -- it: the file being edited is named by the config's keymap setting.
    diftray.command("config open")
    say("remap edit: open keymap.ini, paste what `remap write` printed, then " ..
        "run keymap check and keymap reload")
    return
  end

  if action == "show" then
    diftray.command("keymap show")
    say("remap show: the loaded keymap is above")
    return
  end

  if action == "reload" then
    diftray.command("keymap reload")
    dirty = false
    say("remap reload: the previous keymap stays in force if the file is invalid")
    return
  end

  if action == "diff" then
    -- The compositor owns the loaded file and an extension cannot read it, so
    -- the comparison is the two views the user already has: what this table
    -- declares, and what `keymap show` reports. Both are asked for and the
    -- status line shows them one after the other.
    diftray.command("keymap show")
    say("remap diff: " .. plural(#bindings, "chord") ..
        " declared here; the loaded keymap is above. Chords in one but not " ..
        "the other are the difference.")
    return
  end

  if action == "clear" then
    bindings = {}
    dirty = false
    say("remap table emptied")
    return
  end

  say("remap supports: add, rm, find, check, run, prefix, metaprefix, profile, " ..
      "write, edit, show, reload, diff, clear")
end)

-- Nothing here observes the compositor. A chord table is a proposal about a
-- file, and the compositor is the only thing that can accept one, so this
-- extension stays a pure front end rather than guessing at the keymap's state
-- from layout events.
