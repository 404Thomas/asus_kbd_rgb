# asus_kbd_rgb

🌐 **English** · [Português](README.pt-BR.md)

RGB keyboard backlight controller for the **ASUS VivoBook S14 (S5406)** laptop
on Linux, written in plain C.

This model has a **single-zone** backlight (all keys change color together),
exposed as a HID `LampArray` device (`ITE5570`, VID `0x0B05` / PID `0x19B6`).
The program talks to the controller directly over `hidraw` — no proprietary
drivers — and implements animated effects (breathe, cycle) in software using
`pthreads`.

## Features

- 🎨 **Static color** from any hex value (`RRGGBB`)
- 🌬️ **Breathe** — smoothly pulses the chosen color (sine curve)
- 🌈 **Cycle** — sweeps the full HSV spectrum
- 🔆 **4 brightness levels** and 3 speeds (slow / med / fast)
- 💾 **Persistence**: the last state is saved and restored at boot and on
  resume from suspend (via systemd)
- 🧰 **Interactive menu** in the terminal
- 🔌 Runs **without root** via a udev rule
- 🪶 No dependencies beyond `libc`, `libm` and `pthread`

## Compatibility

| Item        | Value                                   |
|-------------|-----------------------------------------|
| Laptop      | ASUS VivoBook S14 S5406                  |
| Controller  | ITE5570 (HID LampArray, single-zone)    |
| USB IDs     | VID `0x0B05`, PID `0x19B6` (and `0x1B2C`)|
| OS          | Linux (tested on Fedora)                |

> Other ASUS models with the same single-zone ITE controller may work. Use
> `--debug` to check your hardware's VID/PID.

## Installation

### Requirements

```sh
# Fedora
sudo dnf install gcc make

# Debian/Ubuntu
sudo apt install build-essential
```

### Build and install

```sh
git clone https://github.com/<your-username>/asus_kbd_rgb.git
cd asus_kbd_rgb

make                 # builds the ./asus_kbd_rgb binary
sudo make install    # installs to /usr/local/bin
```

### Root-less access (recommended)

By default `/dev/hidraw*` requires root. The udev rule grants access to the
`input` group:

```sh
sudo make udev
sudo usermod -aG input $USER
# log out and back in for the group change to take effect
```

### Restore the color at boot (optional)

Installs a systemd service that reapplies the last saved state when the laptop
powers on and when it resumes from suspend/hibernate:

```sh
sudo make service
```

## Usage

```sh
asus_kbd_rgb [options]
```

| Option               | Description                                            |
|----------------------|--------------------------------------------------------|
| `--color <color>`    | Hex color: `FF0000` or `"#FF0000"`                     |
| `--mode <mode>`      | `static` \| `breathe` \| `cycle` \| `off`              |
| `--speed <speed>`    | `slow` \| `med` \| `fast` (for breathe and cycle)      |
| `--brightness <0-3>` | Brightness level                                       |
| `--on`               | Turn on at full-brightness white                       |
| `--off`              | Turn the backlight off                                 |
| `--restore`          | Reapply the last saved state (used at boot)            |
| `--foreground`       | Keep breathe/cycle in the terminal (don't daemonize)   |
| `--interactive`      | Open the interactive menu                              |
| `--debug`            | Show hardware information                               |
| `--help`             | Help                                                   |

### Examples

```sh
# Static aqua-green at full brightness
asus_kbd_rgb --color 00FF88 --brightness 3

# Orange breathe effect, slow
asus_kbd_rgb --mode breathe --color FF6600 --speed slow

# Full spectrum, fast
asus_kbd_rgb --mode cycle --speed fast

# Turn off / interactive menu
asus_kbd_rgb --off
asus_kbd_rgb --interactive
```

> **Background animations:** `breathe` and `cycle` need a live process to
> animate, so the program daemonizes and frees the terminal. Use `--off` to
> stop them (or `--foreground` to keep them in the terminal until `Ctrl+C`).

## How it works

- **HID / hidraw** — the controller is located automatically by scanning
  `/sys/class/hidraw/*` for the known VID/PID. Colors are sent via HID
  LampArray *feature reports* (`LampArrayControl` to take host control and
  `LampRangeUpdate` to paint every lamp at once).
- **Effects** — since the firmware only does static color, breathe and cycle
  are generated in a thread that recomputes the color every frame (`breathe`
  with a sine curve, `cycle` sweeping the HSV hue). Color, brightness and speed
  can be adjusted live from the interactive menu.
- **Persistence** — the current state is written to
  `/var/lib/asus_kbd_rgb/state` (overridable via `ASUS_KBD_RGB_STATE`).
  `--restore` reads it back; the systemd service calls `--restore` at boot and
  on resume.
- **Single daemon** — the animation daemon's PID lives in
  `/var/lib/asus_kbd_rgb/pid`, ensuring only one effect runs at a time.

## Project layout

```
asus_kbd_rgb/
├── src/
│   └── asus_kbd_rgb.c          # source code (single file)
├── systemd/
│   └── asus-kbd-rgb.service    # boot/resume restore service
├── udev/
│   └── 99-asus-kbd-rgb.rules   # root-less access rule
├── Makefile
├── LICENSE
└── README.md
```

## Uninstall

```sh
sudo make uninstall
```

## Disclaimer

Independent project, not affiliated with ASUS. Use at your own risk — it sends
HID commands directly to the keyboard controller.

## License

[MIT](LICENSE)
