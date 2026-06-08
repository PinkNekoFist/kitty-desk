# kitty-desktop

A client-less remote desktop tool designed for the **Kitty terminal**. It captures a Wayland desktop and renders it directly into your terminal window using the **Kitty Graphics Protocol**.

The main philosophy of `kitty-desktop` is simplicity and portability: you don't need to install any specialized client software on your local machine. If you have the Kitty terminal, you can access your remote desktop immediately.

## Features

- **No Client Installation:** Works out of the box with any Kitty terminal. No "kittens" or plugins required on the local side.
- **Standalone Binary:** The server is a single binary that works over any standard PTY stream, such as SSH.
- **Kitty Graphics Protocol:** Utilizes native terminal features to display high-quality images without an X11/Wayland bridge.
- **Full Input Forwarding:** Transparently forwards keyboard (Kitty Keyboard Protocol) and mouse events back to the server.

## Prerequisites

### Server Side (Remote Machine)
- **Wayland Compositor:** Must support `wlr-screencopy-unstable-v1` (e.g., Hyprland, Sway).
- **Dependencies:**
    - `libpng`, `wayland-client`, `pkg-config`, `wayland-scanner`
    - `ydotool` (required for remote input injection)
- **Build Tools:** `gcc`, `make`

### Client Side (Local Machine)
- **Terminal:** [Kitty](https://sw.kovidgoyal.net/kitty/) (Required).

## Installation

1. **Clone the repository on the remote machine:**
   ```bash
   git clone https://github.com/PinkNekoFist/kitty-desktop.git
   cd kitty-desktop
   ```

2. **Build:**
   ```bash
   make clean && make
   ```
   This produces the `kgp-remote` binary.

## Usage

Simply run the binary on the remote machine via SSH.

```bash
ssh user@remote-host "/path/to/kgp-remote [options]"
```

### Options
- `-s <WIDTH>x<HEIGHT>`: Scale the output to a specific resolution (e.g., `-s 1280x720`).
- `-m <mode>`: Encoding mode (`indexed` for 8-bit color or `rgb24` for full color).
- `-l <level>`: PNG compression level (0-9).
- `-v`: Verbose mode.

### Controls
- **Exit:** `Ctrl + Alt + Q` (Emergency exit sequence) or close the SSH session.

## How It Works

1. **Capture:** The `kgp-remote` binary captures frames from the Wayland compositor using the `wlr-screencopy` protocol.
2. **Processing:** Frames are analyzed for changes, and modified regions are encoded as PNG data.
3. **Display:** The data is wrapped in Kitty Graphics Protocol escape sequences and written to standard output.
4. **Input:** The binary reads standard input for keyboard and mouse sequences, parsing them and injecting them into the remote system via `ydotool`.

## Troubleshooting

- **No capture:** Verify `WAYLAND_DISPLAY` is set and the compositor supports screencopy.
- **Input not working:** Ensure `ydotoold` is running on the remote host and the user has appropriate permissions.
