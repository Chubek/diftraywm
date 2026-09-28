-- Install in ~/.config/diftraywm/extensions/notebook.lua.
-- :scratchpad opens a Notelet without launching a terminal subprocess.
diftray.register_command("scratchpad", function(tokens, scope)
  diftray.command("notelet open scratchpad")
end)
diftray.register_command("desktop-info", function(tokens, scope)
  diftray.command("notelet open desktop")
end)
