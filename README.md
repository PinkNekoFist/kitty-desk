# kitty-desk

A client-less remote desktop tool designed for the **Kitty terminal**. It captures a Wayland desktop and renders it directly into your terminal window using the **Kitty Graphics Protocol**.

The main philosophy of `kitty-desk` is simplicity and portability: you don't need to install any specialized client software on your local machine. If you have the Kitty terminal, you can access your remote desktop immediately.

## Features

- **No Client Installation:** Works out of the box with any Kitty terminal. No "kittens" or plugins required on the local side.
- **Single Binary:** The server is a single binary that works over any standard PTY stream, such as SSH. (Input injection relies on `ydotool` being installed on the remote.)
- **Kitty Graphics Protocol:** Utilizes native terminal features to display high-quality images without an X11/Wayland bridge.
- **Full Input Forwarding:** Transparently forwards keyboard (Kitty Keyboard Protocol) and mouse events back to the server.

## Prerequisites

### Server Side (Remote Machine)
- **Wayland Compositor:** Must support `wlr-screencopy-unstable-v1` (e.g., Hyprland, Sway).
- **Dependencies:**
    - `libpng`, `wayland-client`, `pkg-config`, `wayland-scanner`
    - `ydotool`, with the `ydotoold` daemon running (required for remote input injection)
- **Build Tools:** `gcc`, `make`

### Client Side (Local Machine)
- **Terminal:** [Kitty](https://sw.kovidgoyal.net/kitty/) (Required).

## Installation

1. **Clone the repository on the remote machine:**
   ```bash
   git clone https://github.com/PinkNekoFist/kitty-desk.git
   cd kitty-desk
   ```

2. **Build:**
   ```bash
   make clean && make
   ```
   This produces the `kgp-remote` binary.

## Usage

Run the binary on the remote machine via SSH from inside Kitty. The `-t` flag is required: `kgp-remote` needs a real PTY to switch the terminal into raw mode and to read the window size.

```bash
ssh -t user@remote-host "/path/to/kgp-remote [options]"
```

An SSH session usually doesn't inherit the desktop's `WAYLAND_DISPLAY`. If it's unset, libwayland falls back to `wayland-0`, which may not be your compositor's socket (Hyprland often uses `wayland-1`). Check `ls $XDG_RUNTIME_DIR` on the remote and set it explicitly:

```bash
ssh -t user@remote-host "WAYLAND_DISPLAY=wayland-1 /path/to/kgp-remote [options]"
```

### Options
| Option | Default | Description |
|---|---|---|
| `-s, --scale <W>x<H>` | off (native resolution) | Downscale frames to `W`x`H` before encoding (nearest-neighbour), e.g. `-s 1280x720`. Reduces bandwidth at the cost of sharpness. |
| `-m, --mode <mode>` | `indexed` | `indexed`: 8-bit PNG using a fixed 256-colour palette (6×6×6 colour cube + 40 greys). Smaller and faster, but gradients and photos will band. `rgb24`: full-colour PNG. Any other value falls back to `indexed`. |
| `-l, --level <0-9>` | `1` | zlib compression level for the PNG. Higher is smaller but slower to encode. |
| `-v, --verbose` | off | Log parsed keyboard and mouse events to stderr. |

When the program exits, it prints a performance summary to stderr: the average time spent in each stage (capture, scale, diff, quantize, PNG, render, terminal I/O), the effective FPS, and the number of dropped frames. Use it to tune `-s`, `-m` and `-l` for your link.

### Controls
- **Exit:** `Ctrl + Alt + Q`, or close the SSH session.
- `Ctrl + C` does **not** exit. The terminal runs in raw mode, so `Ctrl + C` (like every other key) is forwarded to the remote desktop.
- On exit, all modifier keys are released on the remote so none are left stuck.

## Known Limitations

- **Mouse position mapping:** Mouse coordinates are sent as the terminal's pixel coordinates and passed to `ydotool` unchanged. They are not scaled to the remote screen, and `-s` doesn't affect them. Clicks land where you point only when the Kitty window's pixel size matches the remote screen's resolution. Otherwise the pointer drifts the further you move from the top-left corner.
- **Single monitor:** Only the first output the compositor reports is captured. On multi-monitor setups you can't choose which one.
- **Pixel format:** Captured frames are assumed to be 32-bit BGRX/BGRA with no row padding. Compositors that hand out other formats or padded strides will show garbled output.

## How It Works

1. **Capture:** The `kgp-remote` binary captures frames from the Wayland compositor using the `wlr-screencopy` protocol.
2. **Processing:** Frames are analyzed for changes, and modified regions are encoded as PNG data.
3. **Display:** The data is wrapped in Kitty Graphics Protocol escape sequences and written to standard output.
4. **Input:** The binary reads standard input for keyboard and mouse sequences, parsing them and injecting them into the remote system via `ydotool`.

## Troubleshooting

- **No capture / `wl_display_connect failed`:** Set `WAYLAND_DISPLAY` to the compositor's socket (see [Usage](#usage)) and check that the compositor supports `wlr-screencopy` (wlroots-based: Hyprland, Sway, etc.). GNOME and KDE don't support it.
- **Keys echo locally / nothing responds / image is the wrong size:** You probably ran `ssh` without `-t`.
- **Input not working:** Ensure `ydotoold` is running on the remote host and the user has appropriate permissions.
