# padbridge

**Makes the 8BitDo Ultimate C 2.4G work in Kingdom Come: Deliverance II on macOS with
[Bridgeplay](https://github.com/raymondchins/Bridgeplay), rumble included.** The pad shows up in
the game as a DualSense: every button, both sticks, analog triggers, and smooth vibration.

- [Is this your problem?](#is-this-your-problem)
- [How it works](#how-it-works)
- [Install](#install)
- [Button mapping](#button-mapping)
- [Settings](#settings)
- [Tuning rumble](#tuning-rumble)
- [Troubleshooting](#troubleshooting)
- [Uninstall](#uninstall)
- [Credits](#credits)

## Is this your problem?

- You play Windows games on a Mac through Bridgeplay (Wine/CrossOver under the hood).
- Kingdom Come: Deliverance II says **no controller is connected**, even though the pad works in
  other games. Disabling Steam Input or replugging doesn't help.
- Your pad is an **8BitDo Ultimate C 2.4G** (USB dongle `2dc8:3016`) in **D-input** mode.

That's [Wine bug 60226](https://bugs.winehq.org/show_bug.cgi?id=60226): since 1.5.6, KCD2 checks for
XInput pads but never reads them under Wine. Its PlayStation path works, so padbridge gives it a
DualSense.

## How it works

Wine's macOS HID bus (`winebus.sys`, `bus_iohid.c`) learns a device from `IOHIDDeviceGetProperty`,
reads it with `IOHIDDeviceRegisterInputReportCallback`, and writes it with
`IOHIDDeviceSetReport`/`GetReport`. Wine passes DualSense IDs (`054c:0ce6`) to games as raw HID.

`padbridge.dylib` is inserted into the game's Wine processes and interposes those IOKit calls, for the
8BitDo only:

| Wine asks | padbridge answers |
|---|---|
| vendor, product, version, name | a USB DualSense's (`054c:0ce6`) |
| report descriptor, report sizes | a genuine DualSense's 337-byte descriptor |
| input reports | each 8BitDo report translated into a 64-byte DualSense report, and the last state repeated every 4 ms between them, like a real DualSense's 250 Hz stream |
| feature reports 05, 09, 20 | calibration, pairing and firmware replies the game's PlayStation code reads |
| output report 02 (rumble, lightbar) | rumble, scaled by the game's motor power setting, becomes the 8BitDo's vendor motor report `81 11 04 08`; the rest is accepted and ignored |

Wine's HID thread only hands reports on when its run loop has handled a source, so the 250 Hz
repeat is a run loop source that a timer signals; a bare timer left repeats queued for up to 10 s.

**Sleep.** After a few idle minutes the pad sleeps and the receiver swaps its gamepad interface for a
vendor "IDLE" one (same IDs). padbridge leaves the IDLE interface alone and, by default
(`keep_alive`), keeps the sleep from Wine: the DualSense stays connected with the pad centred, and
when a button wakes the pad, its new interface takes over the same DualSense. If the new interface
can't be opened, Wine gets the removal and the pad as a new DualSense, as without `keep_alive`.

The pad itself is untouched, and nothing in Bridgeplay, Wine or the game is modified. Bridgeplay's
launcher forwards `STEAM_DYLD_INSERT_LIBRARIES` to Wine (and drops most other variables), so the
launch option wrapper puts the library in front of Steam's overlay there.

## Install

You need Bridgeplay, KCD2 from Steam, and Apple's Command Line Tools (`xcode-select --install`).

**1. Build and install:**

```bash
git clone https://github.com/FijshH/padbridge.git
```

```bash
cd padbridge && ./build.sh
```

This installs `~/.local/lib/padbridge.dylib`, the `~/.local/bin/padbridge` wrapper and
`~/.local/bin/padbridge-rumble`.

**2. Set the controller to D-input:** hold **B + Home** until it connects. It remembers the mode.
(XInput mode has no macOS driver, so nothing on the Mac can see the pad in it.)

**3. Add the launch option.** In Steam, right-click **Kingdom Come: Deliverance II → Properties →
General → Launch Options** and paste the line `build.sh` printed, with your username:

```
/Users/yourname/.local/bin/padbridge %command%
```

Or, with Steam closed:

```bash
python3 scripts/set-launch-options.py 1771300 "$HOME/.local/bin/padbridge %command%"
```

**4. Play.** The game's control prompts switch to PlayStation icons once you touch the pad.

The same launch option works for other Bridgeplay games with PlayStation controller support.
Games that only read Xbox controllers won't see a DualSense, so leave the option off for them.

## Button mapping

| 8BitDo | DualSense |
|---|---|
| A / B / X / Y | Cross / Circle / Square / Triangle |
| LB / RB | L1 / R1 |
| LT / RT | L2 / R2, analog |
| L3 / R3 | L3 / R3 |
| D-pad | D-pad |
| View | Touchpad click (`view` setting) |
| Menu | Options |
| Home | PS |

The sticks' resting value, 127, becomes a DualSense's 128, with both ends kept. The pad has no
touchpad, motion sensors or adaptive triggers. padbridge reports the touchpad as
untouched, the pad as level and still, and the battery as wired and full.

## Settings

`~/.config/padbridge/padbridge.conf`, read when the game starts. Every key is optional.
[`docs/padbridge.conf.example`](docs/padbridge.conf.example) lists them all.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `0` leaves the pad as it is |
| `view` | `touchpad` | What View sends: `touchpad`, `create`, `mute`, `ps` or `none` |
| `button3`, `button6` | `create`, `mute` | HID buttons 3 and 6, for 8BitDo pads that have them |
| `rumble_strength` | `100` | Percent, 0-200 |
| `rumble_max` | `180` | Soft ceiling for strong rumble, 1-255; `255` turns it off |
| `rumble_floor` | `0` | Lowest motor level for any nonzero rumble, 0-200 |
| `rumble_release_ms` | `30` | How long a falling level takes to fade out |
| `swap_motors` | `0` | `1` swaps left and right |
| `keep_alive` | `1` | Keep the DualSense while the pad sleeps (see [How it works](#how-it-works)); `0` lets it disconnect |
| `log` | `1` | `0` off, `1` events, `2` every input and rumble change |

The log is `~/Library/Logs/padbridge.log`. Past 8 MB it moves to `padbridge.log.1` when the game
starts.

## Tuning rumble

The game's own vibration power (a DualSense output field, 100% down to 12.5% in eighths) applies
first; padbridge logs it when it changes (`game motor power`). Then: a DualSense makes a soft tick
out of a weak request; the 8BitDo's motors are felt from level 4 of 255,
so low levels go through unchanged. At full power they get harsh, so strong rumble bends smoothly
toward `rumble_max` (a `tanh` curve) instead of clipping:

| Game asks for | 4 | 16 | 64 | 128 | 192 | 255 |
|---|---|---|---|---|---|---|
| Motor gets (default) | 4 | 16 | 61 | 110 | 142 | 160 |

Rises land at once; a fall from full power fades out over `rumble_release_ms`, by the clock. The
game's lightbar reports leave the motors as they are. Motor reports go out at most every 8 ms and
are refreshed before the receiver's 200 ms timeout while a level is held.

To feel the raw motor levels, with the game closed or open:

```bash
padbridge-rumble 4 16 32 64 128 255
```

## Troubleshooting

**KCD2 still says no controller.** Check `~/Library/Logs/padbridge.log`:

| Line | Meaning |
|---|---|
| `padbridge … loaded` (from `winedevice`) | The library is in. If missing, check the launch option |
| `8BitDo … found, presenting it as a DualSense` | The pad was seen. If missing, check D-input and the dongle, and wake the pad |
| `input opened` | Wine is reading it |
| `game read feature 05` | The game's PlayStation code took the pad |
| `8BitDo … gone (asleep?)`, then `… back` | The pad slept and woke; the game kept its DualSense |

`kcd.log` in the game folder then says `CScePad::SetControllerType(), m_ControllerType: DualSense`.

With `log = 2`, every Wine process logs `padbridge … inserted`.

**The pad was asleep when the game started.** Wake it: Wine adds it then. If the game doesn't pick it
up, restart the game with the pad awake.

**No rumble.** Try `padbridge-rumble 64` (the pad must be awake). If that's silent too, replug the
dongle. If the log shows a low `game motor power`, raise the game's vibration setting.

## Uninstall

Remove the launch option, then:

```bash
rm ~/.local/lib/padbridge.dylib ~/.local/bin/padbridge ~/.local/bin/padbridge-rumble
```

## Credits

- The DualSense report descriptor and firmware-info report are a genuine DualSense's, from
  [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle) (MIT).
- Report layouts and rumble flags follow SDL's `SDL_hidapi_ps5.c` and Linux's `hid-playstation`.
- The 8BitDo motor report comes from
  [cyberpunk-2077-mac-controller-fix](https://github.com/FijshH/cyberpunk-2077-mac-controller-fix).
- [Bridgeplay](https://github.com/raymondchins/Bridgeplay) and CodeWeavers'
  [WineCX](https://www.codeweavers.com/crossover) run the game.

MIT license.
