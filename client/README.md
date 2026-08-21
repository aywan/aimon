# aiboard-client

Go CLI that sends a JSON control payload to the `aw_extramon_1` board over BLE
(uses `tinygo.org/x/bluetooth`, a regular desktop Go library — no TinyGo
compiler or flashing involved, just `go build`).

## Build

```
./build_macos.sh
```

This builds into `aiboard-client.app/Contents/MacOS/aiboard-client`. On
macOS a bare binary gets killed by the OS the moment it touches Bluetooth
(`NSBluetoothAlwaysUsageDescription` missing), so the binary must live
inside an `.app` bundle with the `Info.plist` already included in this repo.

## First run (macOS permission)

Run this from your own Terminal (not through an automation tool — otherwise
macOS attributes the Bluetooth permission check to that tool instead of this
app, and it will crash):

```
open client/aiboard-client.app --args send --json '{"slot_1":{"type":"gauge","value":80,"label":"codex","fg":"#3ecf8e","bg":"#1a3328"},"slot_2":{"type":"text","text":"3","label":"days"},"slot_3":{"type":"gauge","value":40,"label":"reset","fg":"#40a0ff","bg":"#1a2833"},"slot_4":{"type":"text","text":"idle","label":"status","fg":"#c8c8c8"}}'
```

The first time, macOS will show a system dialog asking to allow Bluetooth
access for `aiboard-client` — click Allow. After that it's a one-time grant.

## Usage

```
# from client/ — default payload fills all four slots (examples/all_slots.json)
make send
```

Or via the app bundle:

```
open client/aiboard-client.app --args send --json '{"slot_1":{"type":"gauge","value":80,"label":"codex","fg":"#3ecf8e","bg":"#1a3328"},"slot_2":{"type":"text","text":"3","label":"days"},"slot_3":{"type":"gauge","value":40,"label":"reset","fg":"#40a0ff","bg":"#1a2833"},"slot_4":{"type":"text","text":"idle","label":"status","fg":"#c8c8c8"}}'
```

Commands:
- `send --json '...'` — merge fields into board state and flush over BLE
- `send --hold 15s` — keep the connection open after writing (useful to see the status dot)
- `status` — scan and report whether the board is advertising + its RSSI; doesn't connect
- `run` — poll Codex onto the four slots and listen for `hook` IPC
  (`slot_1` remaining quota, `slot_2` time until reset, `slot_3` reset
  progress, `slot_4` hook status text)
- `hook <name>` — deliver a Codex-style hook to a running `run` daemon (must be running)
- `--name` — device name to scan for (default `aw_extramon_1`)
- `--timeout` — scan timeout (default `15s`)
- `--socket` — Unix socket path for run↔hook IPC (default `/tmp/aiboard-client.sock`)

## Hooks (Codex)

Start the daemon once:

```
make run
```

Then wire Codex hooks to notify it, for example in `~/.codex/hooks.json`:

```json
{
  "hooks": {
    "SessionStart": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook SessionStart"}]}],
    "UserPromptSubmit": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook UserPromptSubmit"}]}],
    "PreToolUse": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook PreToolUse"}]}],
    "PostToolUse": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook PostToolUse"}]}],
    "PermissionRequest": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook PermissionRequest"}]}],
    "PreCompact": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook PreCompact"}]}],
    "PostCompact": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook PostCompact"}]}],
    "SubagentStart": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook SubagentStart"}]}],
    "SubagentStop": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook SubagentStop"}]}],
    "Stop": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook Stop"}]}],
    "SessionEnd": [{"hooks": [{"type": "command", "command": "/path/to/aiboard-client.app/Contents/MacOS/aiboard-client hook SessionEnd"}]}]
  }
}
```

Or test manually while `run` is up:

```
make hook HOOK=SessionStart
make hook HOOK=Stop
```

Every hook writes its **name** into slot_4 (CamelCase wraps onto separate
lines so it fits the 160px cell) plus a mood `fg`. Unknown names still go
to the board (`#d6d3d1`). Not LED / screen color.

- `SessionStart` — `#22d3ee` (dawn)
- `UserPromptSubmit` — `#60a5fa` (listening)
- `PreToolUse` — `#facc15` (focus)
- `PostToolUse` — `#4ade80` (productive)
- `PermissionRequest` — `#ff4a1a` (alert)
- `PreCompact` — `#c084fc` (pressure)
- `PostCompact` — `#2dd4bf` (relief)
- `SubagentStart` — `#f472b6` (curious)
- `SubagentStop` — `#e879f9` (helper done)
- `Stop` — `#8fbf8f` (rest)
- `SessionEnd` — `#818cf8` (dusk)

Slot 4 returns to `idle` (`#c8c8c8`) ten minutes after the last hook.

## Checking connectivity

`make status` scans for the board's advertisement and reports whether it's
reachable, without touching the connection:

```
make status
```

Note: the board won't show up as a normal "device" in System Settings >
Bluetooth — it's a custom BLE peripheral (not a keyboard/speaker/etc.), so
macOS only exposes it to apps that scan for it directly, like this one. To
check what macOS's Bluetooth stack itself knows about it (bonded address,
etc.) from a terminal: `system_profiler SPBluetoothDataType | grep -A2 aw_extramon_1`.

## Pairing

The board accepts a *new* bond only after you tap **Pair** on its Settings
screen (passkey is shown there for 60s). Once your computer is bonded,
reconnecting works anytime. **Forget** on the board deletes its bond only —
macOS must Forget the device separately, or it will keep the old LTK and
fail to reconnect.

## JSON payload fields

The main screen is four horizontal slots (`slot_1` … `slot_4`). All keys
optional: omitted slots are left as they are. Send `"type":"empty"` or JSON
`null` to clear a slot.

Each slot object:
- `type` — `"gauge"`, `"text"`, or `"empty"`
- `value` — 0–100, gauge fill (clamped on the board)
- `label` — caption under the widget (Latin only; the board font has no Cyrillic)
- `text` — body of a text widget; if omitted, `label` is shown as the body
- `fg` / `bg` — hex `"#RRGGBB"`. `fg` is the gauge indicator **or** the text
  body color; `bg` is the gauge track (ignored for text). Omitted keeps the
  previous color (defaults: gauge `#3ecf8e` / `#333333`, text white)

Clock (top-right of the main screen, computer timezone):
- `unix` — UTC unix seconds. Omitted: board RTC is left as-is
- `tz_offset` — seconds east of UTC (from the computer's zone). Default 0

`send` and `run` stamp `unix`/`tz_offset` from this computer on every BLE
write, so you do not need to pass them by hand.

Examples:

```
# all four slots (this is also `make send` with no JSON=)
make send

# or pass a payload; quote it so the shell does not treat #RRGGBB as a comment
make send JSON='{"slot_1":{"type":"gauge","value":80,"label":"codex","fg":"#3ecf8e","bg":"#1a3328"},"slot_2":{"type":"text","text":"3","label":"days"},"slot_3":{"type":"gauge","value":40,"label":"reset","fg":"#40a0ff","bg":"#1a2833"},"slot_4":{"type":"text","text":"idle","label":"status","fg":"#c8c8c8"}}'

# clear slot 2
make send JSON='{"slot_2":{"type":"empty"}}'
```
