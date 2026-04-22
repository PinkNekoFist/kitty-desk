#!/usr/bin/env python3
import sys
import os
import struct
import subprocess
import threading
import select
import termios
import tty
import shutil
import base64
import random
import re

# Try to import zstandard (official), fallback to zstd.
_decompress = None
try:
    import zstandard

    _dctx = zstandard.ZstdDecompressor()
    _decompress = _dctx.decompress
except ImportError:
    try:
        import zstd

        _decompress = zstd.decompress
    except ImportError:
        pass

MAGIC = b"KGPF"
HDR_FMT = ">4sBBIHHHHHHI"
HDR_SIZE = struct.calcsize(HDR_FMT)  # 26 bytes

FLAG_FULL_FRAME = 0x01
FLAG_COMPRESSED = 0x02
FLAG_SKIP = 0x04

CHUNK_SIZE = 4096

INPUT_FMT = ">BBiHH"  # type, flags, code, mx, my (10 bytes)
INPUT_KEY = 1
INPUT_MOUSE = 2

SETUP = (
    b"\033[?1049h"  # Alternate screen
    b"\033[2J"  # Clear screen
    b"\033[H"  # Cursor to home
    b"\033[?25l"  # Hide cursor
    b"\033[?1003h"  # Any motion mouse tracking
    b"\033[?1006h"  # SGR 1006 mouse encoding
    b"\033[>11u"  # Kitty Keyboard Protocol
)

TEARDOWN = (
    b"\033[<u"  # Restore keyboard protocol
    b"\033[?1003l"  # Stop mouse tracking
    b"\033[?1006l"
    b"\033[?25h"  # Show cursor
    b"\033[?1049l"  # Back to main screen
)


class Frame:
    def __init__(self, x, y, w, h, fw, fh, seq, flags, rgb24):
        self.x, self.y, self.w, self.h = x, y, w, h
        self.full_w, self.full_h = fw, fh
        self.seq = seq
        self.is_full = bool(flags & FLAG_FULL_FRAME)
        self.is_skip = bool(flags & FLAG_SKIP)
        self.rgb24 = rgb24


def recv_exactly(stream, n):
    buf = b""
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise EOFError("SSH connection closed")
        buf += chunk
    return buf


def read_frame(stream):
    # Search for MAGIC word "KGPF" to sync with the stream, skipping any noise
    magic_buf = b""
    while magic_buf != MAGIC:
        chunk = stream.read(1)
        if not chunk:
            raise EOFError("SSH connection closed")
        magic_buf = (magic_buf + chunk)[-4:]

    # Read the rest of the header (26 bytes total - 4 bytes MAGIC)
    hdr_rest_fmt = ">BBIHHHHHHI"
    hdr_rest_size = struct.calcsize(hdr_rest_fmt)
    hdr_data = recv_exactly(stream, hdr_rest_size)
    ver, flags, seq, x, y, w, h, fw, fh, dsize = struct.unpack(hdr_rest_fmt, hdr_data)

    rgb24 = None
    if dsize > 0:
        data = recv_exactly(stream, dsize)
        if flags & FLAG_COMPRESSED:
            if _decompress is None:
                raise RuntimeError(
                    "zstd or zstandard module not installed. Please install python-zstandard."
                )
            rgb24 = _decompress(data)
        else:
            rgb24 = data

    return Frame(x, y, w, h, fw, fh, seq, flags, rgb24)


class KittyRenderer:
    def __init__(self, rows, cols, cell_w, cell_h):
        self.kitty_id = random.randint(1, 2**31 - 1)
        self.frame_number = 0
        self.rows = rows
        self.cols = cols
        self.cell_w = cell_w
        self.cell_h = cell_h
        self.out = sys.stdout.buffer

    def render(self, frame: Frame):
        if frame.is_skip or frame.rgb24 is None:
            return

        encoded = base64.b64encode(frame.rgb24)
        total = len(encoded)
        offset = 0
        first_chunk = True

        while offset < total:
            chunk = encoded[offset : offset + CHUNK_SIZE]
            more = 1 if offset + CHUNK_SIZE < total else 0

            if first_chunk:
                if self.frame_number == 0 or frame.is_full:
                    # Create/Replace image
                    hdr = (
                        f"\033_Ga=T,i={self.kitty_id},f=24,q=2,"
                        f"c={self.cols},r={self.rows},"
                        f"s={frame.w},v={frame.h},m={more};"
                    )
                else:
                    # Update dirty rect
                    cx = frame.x // self.cell_w
                    cy = frame.y // self.cell_h
                    hdr = (
                        f"\033_Ga=f,r=1,i={self.kitty_id},f=24,q=2,"
                        f"x={cx},y={cy},"
                        f"s={frame.w},v={frame.h},m={more};"
                    )
                first_chunk = False
            else:
                if self.frame_number == 0 or frame.is_full:
                    hdr = f"\033_Gm={more};"
                else:
                    hdr = f"\033_Ga=f,r=1,q=2,m={more};"

            self.out.write(hdr.encode() + chunk + b"\033\\")
            offset += CHUNK_SIZE

        if self.frame_number > 0 and not frame.is_full:
            self.out.write(f"\033_Ga=a,q=2,c=1,i={self.kitty_id};\033\\".encode())

        self.out.flush()
        self.frame_number += 1

    def destroy(self):
        self.out.write(f"\033_Ga=d,q=2,i={self.kitty_id};\033\\".encode())
        self.out.flush()


class KittyInputParser:
    def parse(self, data: bytes) -> list:
        events = []
        i = 0
        while i < len(data):
            if data[i] == 0x1B and i + 1 < len(data) and data[i + 1] == ord("["):
                # CSI sequence
                end = i + 2
                while (
                    end < len(data)
                    and chr(data[end])
                    not in "ABCDEFGHIJKLMPSTXZabcdefghijklmnopqrstuvwxyz~u"
                ):
                    end += 1
                if end < len(data):
                    seq = data[i : end + 1].decode("utf-8", errors="replace")
                    ev = self._parse_csi(seq)
                    if ev:
                        events.append(ev)
                    i = end + 1
                else:
                    i += 1
            else:
                # Raw ASCII character
                ch = data[i]
                events.append(
                    {"type": "key", "flags": 0x01, "code": ch}
                )  # Assume Press
                events.append(
                    {"type": "key", "flags": 0x00, "code": ch}
                )  # Immediate Release for raw ASCII
                i += 1
        return events

    def _parse_csi(self, seq):
        if seq.endswith("u"):
            inner = seq[2:-1]
            parts = inner.split(";")
            codepoint = int(parts[0]) if parts[0] else 0
            event_type = 1
            if len(parts) > 1 and ":" in parts[1]:
                m, e = parts[1].split(":")
                event_type = int(e) if e else 1

            flags = 0
            if event_type == 1:
                flags |= 0x01
            if event_type == 3:
                flags |= 0x02
            if event_type == 2:
                flags |= 0x04
            return {"type": "key", "flags": flags, "code": codepoint}

        if seq.startswith("\x1b[<") and seq[-1] in "Mm":
            inner = seq[3:-1]
            parts = inner.split(";")
            if len(parts) == 3:
                cb, cx, cy = (int(x) for x in parts)
                pressed = seq[-1] == "M"
                buttons = (
                    (cb & 0x03) + 1 if pressed else 0
                )  # 1-based button for ydotool click
                return {"type": "mouse", "buttons": buttons, "x": cx - 1, "y": cy - 1}
        return None


class InputHandler:
    def __init__(self, ssh_stdin):
        self.sink = ssh_stdin
        self._stop = threading.Event()

    def start(self):
        threading.Thread(target=self._loop, daemon=True).start()

    def stop(self):
        self._stop.set()

    def _loop(self):
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        new = termios.tcgetattr(fd)
        new[3] &= ~(termios.ISIG | termios.ICANON | termios.ECHO)
        termios.tcsetattr(fd, termios.TCSADRAIN, new)
        parser = KittyInputParser()
        try:
            while not self._stop.is_set():
                r, _, _ = select.select([sys.stdin], [], [], 0.05)
                if r:
                    # Use os.read to avoid any internal buffering in Python
                    data = os.read(fd, 4096)
                    if not data:
                        break
                    for ev in parser.parse(data):
                        self._send(ev)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)

    def _send(self, ev):
        if ev["type"] == "key":
            pkt = struct.pack(INPUT_FMT, INPUT_KEY, ev["flags"], ev["code"], 0, 0)
            sys.stderr.write(
                f"CLIENT_DEBUG: sending key code={ev['code']} flags={ev['flags']}\n"
            )
        else:
            pkt = struct.pack(
                INPUT_FMT, INPUT_MOUSE, ev["buttons"], 0, ev["x"], ev["y"]
            )
            # sys.stderr.write(f"CLIENT_DEBUG: sending mouse x={ev['x']} y={ev['y']} buttons={ev['buttons']}\n")

        sys.stderr.flush()
        self.sink.write(pkt)
        self.sink.flush()


def query_terminal_cell_size():
    sys.stdout.buffer.write(b"\033[14t")
    sys.stdout.buffer.flush()
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    resp = b""
    try:
        while True:
            r, _, _ = select.select([sys.stdin], [], [], 0.5)
            if not r:
                break
            ch = sys.stdin.buffer.read(1)
            resp += ch
            if ch == b"t":
                break
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
    m = re.search(rb"\[4;(\d+);(\d+)t", resp)
    th, tw = (int(m.group(1)), int(m.group(2))) if m else (1080, 1920)
    cols, rows = shutil.get_terminal_size()
    return tw // cols, th // rows, cols, rows


def main(args):
    if len(args) < 2:
        print("Usage: kitty +kitten kgp-client.py user@host [server_options]")
        print("Example: kitty +kitten kgp-client.py user@host -d -s 1280x720")
        return

    remote_host = args[1]
    server_args = " ".join(args[2:])

    # Search for the binary in common locations: current dir, or PATH
    remote_cmd = (
        f"export PATH=$HOME/.local/bin:/usr/local/bin:$PATH; "
        f"export WAYLAND_DISPLAY=${{WAYLAND_DISPLAY:-wayland-1}}; "
        f'if [ -f "./kgp-test-bin" ]; then CMD="./kgp-test-bin"; '
        f'elif [ -f "./Coding/hypremote/kgp-test/kgp-test-bin" ]; then CMD="./Coding/hypremote/kgp-test/kgp-test-bin"; '
        f'else CMD="kgp-test-bin"; fi; '
        f"exec $CMD {server_args}"
    )

    sys.stderr.write(f"DEBUG: Executing remote command: {remote_cmd}\n")
    sys.stderr.flush()

    ssh = subprocess.Popen(
        ["ssh", "-q", "-T", remote_host, remote_cmd],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=sys.stderr,  # IMPORTANT: forward stderr to see errors
        bufsize=0,
    )

    sys.stdout.buffer.write(SETUP)
    sys.stdout.buffer.flush()
    cell_w, cell_h, cols, rows = query_terminal_cell_size()
    renderer = KittyRenderer(rows, cols, cell_w, cell_h)
    input_handler = InputHandler(ssh.stdin)

    try:
        input_handler.start()
        while True:
            frame = read_frame(ssh.stdout)
            renderer.render(frame)
    except EOFError:
        print("\n[kgp] Connection closed by remote host.", file=sys.stderr)
    except Exception as e:
        import traceback

        traceback.print_exc(file=sys.stderr)
        # Give user time to see the error before kitten closes
        input("\nPress Enter to exit...")
    finally:
        input_handler.stop()
        renderer.destroy()
        sys.stdout.buffer.write(TEARDOWN)
        sys.stdout.buffer.flush()
        ssh.terminate()


if __name__ == "__main__":
    main(sys.argv)
