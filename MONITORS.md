# Monitor configuration

Use `output list` in the Command Bar to find connector names such as `DP-1` or
`HDMI-A-1`. It also reports logical size, position, rotation and scale.

YAML (`diftray.yaml`):

```yaml
monitors:
  - name: DP-1
    rotation: 90
    scale: 1.0
    x: 0
    y: 0
  - name: HDMI-A-1
    rotation: 0
    x: 1080
    y: 0
```

TOML (`diftray.toml`):

```toml
[[monitors]]
name = "DP-1"
rotation = 90
scale = 1.0
x = 0
y = 0

[[monitors]]
name = "HDMI-A-1"
x = 1080
y = 0
```

Original DSL (`diftray.conf`):

```text
monitor {
  name = DP-1
  rotation = 90
  scale = 1
  x = 0
  y = 0
}
monitor {
  name = HDMI-A-1
  x = 1080
  y = 0
}
```

Rotation accepts **0, 90, 180, or 270 degrees counter-clockwise**, following
Wayland's transform convention. For clockwise rotation by 90 degrees, use 270.
A 1920×1080 output rotated by 90 degrees has logical size 1080×1920 at scale 1.
The example places a second monitor to its right. Scale accepts finite numbers
from 0.5 to 4, including fractional values such as 1.25. Logical dimensions
account for both scale and rotation.

Omit **both** `x` and `y` for automatic placement; provide both for an explicit
position in logical desktop coordinates. Negative positions are supported.
Defaults are rotation 0, scale 1, and automatic placement. Unspecified settings
in a named entry take these defaults.

A `name: '*'` entry is a fallback for monitors without an exact entry. In YAML,
quote the asterisk. Exact entries replace the fallback entry completely;
file order does not affect matching. Duplicate names, unknown properties,
invalid values and more than 64 entries are rejected without partially applying
the configuration. Entries for disconnected connectors remain available for
hotplug.

Settings apply when a monitor connects, before its first layout. Backend-rejected
settings produce an error instead of being reported as applied. Rotating or
scaling an output updates NCursor, TCursor, graphical surfaces and terminal
sizes while preserving the focused monitor and cell.

# Live adjustments

Both Command Bars, Lua's queued commands, and the CLI accept:

```text
output rotate DP-1 90
output scale DP-1 1.25
output position DP-1 -1080 0
output position DP-1 auto
```

These commands require a connected output and a Wayland backend. They validate
and test the output state before committing it. Successful overrides are kept
for the current compositor session, including reconnection. They do not rewrite
the configuration file. Use `output move <name>` to move an NCursor to another
monitor; `output position` moves the monitor's logical rectangle.

Notelets receive an `outputs` event after geometry, rotation, scale or connected
monitor changes. Their context includes `rotation`, `scale`, `output_width`,
`output_height` and the full `outputs` listing, so the desktop inspector refreshes
automatically even when a 180-degree rotation leaves its dimensions unchanged.
