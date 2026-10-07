-- DiftrayWM extension: session event sampler.
--
-- Install: cp extensions/pulse.lua ~/.config/diftraywm/extensions/
-- Command: :pulse
--
-- Every other extension here keeps a database. This one watches. It counts
-- frames, layout updates and keypresses, measures the gap between them, and
-- reports whether the session is healthy -- which is the question you have when
-- something feels sluggish and no extension will admit to being the cause.
--
-- The numbers are honest about what they can and cannot be. There is no clock in
-- an extension state, so `pulse` counts events and event gaps rather than
-- milliseconds or frames per second. That is enough to answer the two questions
-- that actually matter here: is anything arriving at all, and is one source
-- drowning the others out.
--
-- A `frame` event is one successful output frame commit, once per output, so a
-- compositor drawing two monitors reports two frames per drawn frame. `pulse`
-- does not divide by an output count it cannot read, and says so instead.
--
--   :pulse              the report
--   :pulse watch        start counting (it is on by default)
--   :pulse pause        stop counting without discarding the totals
--   :pulse resume       start again
--   :pulse reset        zero the counters
--   :pulse alerts on    status line when a source goes quiet
--   :pulse alerts off
--   :pulse threshold N  frames without a keypress before that counts as idle
--
-- What the gaps mean:
--
--   key gap     how long since the last keypress. Rising while frames rise
--               means the session is busy drawing and idle waiting, which is
--               what an animation looks like.
--   view gap    how long since the last layout change. Rising while key gaps
--               fall means keys are arriving that change nothing, which is what
--               a swallowed key looks like.
--
-- A layout change with no keypress is normal: an animation drives one.

local IDLE_FRAMES = 600      -- frames without a keypress before that is idle
local MAX_GAPS = 32          -- gap samples kept for the recent-history display

local frames, views, inputs = 0, 0, 0
local last_frame_gap, last_view_gap, last_input_gap = nil, nil, nil
local recent = {}            -- newest last, each "f12 v3 k40" style sample
local watching = true
local alerts = false
local threshold = IDLE_FRAMES
local silenced = {}          -- which sources have already been announced

local function say(text)
  diftray.status(string.sub(text, 1, 4000))
end

local function plural(count, singular, many)
  return tostring(count) .. " " .. (count == 1 and singular or (many or singular .. "s"))
end

-- A gap is the number of frames since the previous event of the same kind, or
-- since counting started. Rendering it as a count rather than a duration is the
-- honest form: with one output and a steady compositor, a gap of 60 means about
-- a second, but the compositor does not tell this how many outputs it has.
local function gap_text(gap)
  if gap == nil then
    return "never"
  end
  return tostring(gap) .. " frames"
end

local function push(line)
  recent[#recent + 1] = line
  while #recent > MAX_GAPS do
    table.remove(recent, 1)
  end
end

local function sample()
  push(string.format("f%d v%d k%d", frames, views, inputs))
end

local function quiet(name, gap)
  if not alerts or gap == nil then
    return
  end
  if gap < threshold then
    silenced[name] = false
    return
  end
  if silenced[name] then
    return
  end
  -- Announced once per quiet spell, not once per frame. Without this the status
  -- line would be rewritten thousands of times an hour by a hook that is meant
  -- to be cheap.
  silenced[name] = true
  local message = "pulse: no " .. name .. " for " .. gap_text(gap)
  if name == "keys" and frames > 0 then
    message = message .. "; the session is drawing but nothing is being pressed"
  end
  if name == "views" and inputs > 0 then
    message = message .. "; " .. plural(inputs, "keypress") ..
              " changed nothing on screen"
  end
  diftray.status(message)
end

local function report()
  local lines = {}
  lines[#lines + 1] = "pulse: " .. (watching and "watching" or "paused")
  lines[#lines + 1] = string.format("  frames  %-8d last %s", frames, gap_text(last_frame_gap))
  lines[#lines + 1] = string.format("  views   %-8d last %s", views, gap_text(last_view_gap))
  lines[#lines + 1] = string.format("  keys    %-8d last %s", inputs, gap_text(last_input_gap))
  if frames == 0 and views == 0 and inputs == 0 then
    lines[#lines + 1] = "  nothing has been observed yet"
  end
  lines[#lines + 1] = "  alerts " .. (alerts and "on" or "off") ..
                      ", idle after " .. plural(threshold, "frame")
  lines[#lines + 1] = "  frames are counted once per output, so a gap is not a duration"
  if #recent > 0 then
    lines[#lines + 1] = "  recent: " .. table.concat(recent, "  ")
  end
  say(table.concat(lines, "\n"))
end

diftray.register_command("pulse", function(tokens, scope)
  local action = tokens[2]

  if action == nil then
    report()
    return
  end

  if action == "watch" then
    watching = true
    say("pulse is watching")
    return
  end

  if action == "pause" then
    watching = false
    say("pulse paused; totals kept")
    return
  end

  if action == "resume" then
    watching = true
    say("pulse resumed")
    return
  end

  if action == "reset" then
    frames, views, inputs = 0, 0, 0
    last_frame_gap, last_view_gap, last_input_gap = nil, nil, nil
    recent = {}
    silenced = {}
    say("pulse reset")
    return
  end

  if action == "alerts" then
    local wanted = tokens[3]
    if wanted == "on" then
      alerts = true
      silenced = {}
      say("pulse alerts on")
      return
    end
    if wanted == "off" then
      alerts = false
      say("pulse alerts off")
      return
    end
    say("pulse alerts is " .. (alerts and "on" or "off") .. "; use: pulse alerts on|off")
    return
  end

  if action == "threshold" then
    local wanted = tonumber(tokens[3])
    if wanted == nil then
      say("pulse threshold is " .. plural(threshold, "frame"))
      return
    end
    wanted = math.floor(wanted)
    if wanted < 1 or wanted > 1000000 then
      say("pulse threshold: a frame count from 1 to 1000000")
      return
    end
    threshold = wanted
    silenced = {}
    say("pulse idle after " .. plural(threshold, "frame"))
    return
  end

  if action == "keys" or action == "views" or action == "frames" then
    local counts = {frames = frames, views = views, inputs = inputs}
    local gaps = {frames = last_frame_gap, views = last_view_gap, inputs = last_input_gap}
    local names = {frames = "frames", views = "views", inputs = "keys"}
    local key = action == "keys" and "inputs" or action
    say(string.format("pulse %s: %s seen, last %s ago", names[key], counts[key],
                      gap_text(gaps[key])))
    return
  end

  say("pulse supports: watch, pause, resume, reset, alerts, threshold, " ..
      "frames, views, keys")
end)

-- The three hooks are the whole point of this script, so each one is kept to a
-- few instructions: a counter, a gap and a comparison. An extension runs inside
-- the compositor's event loop, and a hook that does real work would be charged
-- for it on every frame of every session.
diftray.on("frame", function()
  if not watching then
    return
  end
  last_frame_gap = frames
  frames = frames + 1
  quiet("keys", last_input_gap)
  quiet("views", last_view_gap)
  if frames % 60 == 0 then
    sample()
  end
end)

diftray.on("view", function()
  if not watching then
    return
  end
  last_view_gap = frames
  views = views + 1
  silenced.views = false
end)

diftray.on("input", function()
  if not watching then
    return
  end
  last_input_gap = frames
  inputs = inputs + 1
  silenced.keys = false
end)
