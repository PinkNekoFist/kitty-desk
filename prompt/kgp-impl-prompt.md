# kgp：方案 A 完整實作（遠端 C daemon + 本地 Python kitten）

## 系統概述

```
本地 Kitty terminal
    │
    │  kitty +kitten kgp-client.py user@remote
    ▼
kgp-client.py（Python kitten，本地跑）
    ├─ 建立 SSH 連線到遠端
    ├─ 讀 frame stream（binary header + zstd RGB24）
    ├─ zstd 解壓 → RGB24
    ├─ Kitty GP escape sequence → stdout
    ├─ 讀 Kitty Keyboard Protocol + SGR mouse event
    └─ 透過 SSH stdin 送回遠端

遠端 kgp-remote（C daemon，遠端跑）
    ├─ wlr-screencopy 抓 Hyprland framebuffer
    ├─ XRGB8888 → RGB24
    ├─ dirty rect 計算（tile-based）
    ├─ zstd 壓縮 dirty rect 資料
    ├─ binary frame header + 壓縮資料 → stdout（SSH pipe）
    └─ 讀 SSH stdin → 解析 input packet → ydotool 注入 Hyprland
```

---

## 遠端：kgp-remote（C）

### 專案結構

```
kgp-remote/
├── Makefile
├── proto/
│   └── wlr-screencopy-unstable-v1.xml
└── src/
    ├── main.c          # 初始化、主迴圈、SIGINT 處理
    ├── capture.c/h     # wlr-screencopy，輸出 RGB24
    ├── diff.c/h        # tile-based dirty rect 計算
    ├── compress.c/h    # zstd 壓縮
    ├── scale.c/h       # nearest-neighbor downscale
    ├── transport.c/h   # binary frame header 組裝與寫出
    └── input.c/h       # 讀 SSH stdin，解析 input packet，ydotool 注入
```

### Wire Protocol：Frame Header

```c
#pragma pack(1)
struct frame_header {
    uint8_t  magic[4];   // "KGPF"
    uint8_t  version;    // 1
    uint8_t  flags;      // 見下方 FLAG_* 定義
    uint32_t seq;        // frame sequence number（big-endian）
    uint16_t x, y;       // dirty rect 左上角（pixel，big-endian）
    uint16_t w, h;       // dirty rect 寬高（pixel，big-endian）
    uint16_t full_w;     // 完整畫面寬（pixel，big-endian）
    uint16_t full_h;     // 完整畫面高（pixel，big-endian）
    uint32_t data_size;  // 後續資料 byte 數（big-endian）
};
#pragma pack()

// flags byte 定義
#define FLAG_FULL_FRAME  0x01  // 整幀更新（第一幀或 dirty rect 太大）
#define FLAG_COMPRESSED  0x02  // data 是 zstd 壓縮的
#define FLAG_SKIP        0x04  // 畫面沒變，data_size = 0，kitten 跳過渲染
```

總 header 大小：`4 + 1 + 1 + 4 + 2+2+2+2 + 2+2 + 4 = 26 bytes`

### Wire Protocol：Input Packet（SSH stdin 方向）

```c
#pragma pack(1)
struct input_packet {
    uint8_t  type;       // INPUT_KEY = 1, INPUT_MOUSE = 2
    uint8_t  flags;      // key: 0x01=press 0x02=release 0x04=repeat
                         // mouse: button bitmask
    uint32_t code;       // key: Unicode codepoint；mouse: 無意義
    int16_t  mx, my;     // mouse: pixel 座標（signed，big-endian）
};
#pragma pack()

#define INPUT_KEY   1
#define INPUT_MOUSE 2
```

總大小：`1 + 1 + 4 + 2 + 2 = 10 bytes`

---

### capture.c / capture.h

```c
struct capture_ctx {
    struct wl_display                    *display;
    struct wl_registry                   *registry;
    struct wl_shm                        *shm;
    struct wl_output                     *output;
    struct zwlr_screencopy_manager_v1    *screencopy_manager;
    uint8_t  *shm_data;      // mmap'd XRGB8888 shm buffer
    uint8_t  *rgb_curr;      // 當前幀 RGB24（malloc）
    uint8_t  *rgb_prev;      // 上一幀 RGB24（malloc，供 diff 用）
    uint32_t  width, height, stride;
    bool      frame_ready;
};

// 初始化 Wayland 連線，綁定所需 globals
int  capture_init(struct capture_ctx *ctx);

// 抓一幀，填入 ctx->rgb_curr，交換 prev/curr
// 回傳 0 成功，-1 失敗
int  capture_frame(struct capture_ctx *ctx);

// 釋放資源
void capture_destroy(struct capture_ctx *ctx);
```

內部實作重點：
- `wlr-screencopy-unstable-v1.xml` 用 `wayland-scanner` 在 build time 產生 binding
- `frame_buffer` callback：用 `memfd_create` + `mmap` 建立 shm，呼叫 `zwlr_screencopy_frame_v1_copy`
- `frame_ready` callback：XRGB8888 → RGB24 轉換

```c
// XRGB8888（little-endian）→ RGB24
// byte order: B G R X → 取 R G B
for (uint32_t i = 0; i < width * height; i++) {
    uint8_t *src = shm_data + i * 4;
    uint8_t *dst = rgb_curr + i * 3;
    dst[0] = src[2]; // R
    dst[1] = src[1]; // G
    dst[2] = src[0]; // B
}
```

---

### diff.c / diff.h

```c
#define TILE_SIZE 64

struct dirty_rect {
    uint32_t x, y;
    uint32_t w, h;
    bool     is_full;   // true = 整幀（第一幀或 prev == NULL）
    bool     is_empty;  // true = 畫面沒變
};

struct dirty_rect diff_compute(const uint8_t *prev,
                               const uint8_t *curr,
                               uint32_t width, uint32_t height);
```

實作策略：
1. 把畫面切成 `TILE_SIZE × TILE_SIZE` 的 tile grid
2. 每個 tile 逐行 `memcmp`（因為 row-major，tile 資料不連續）
3. 記錄所有變動 tile 的 min/max 行列索引
4. bounding box = `(min_col * TILE_SIZE, min_row * TILE_SIZE, (max_col+1)*TILE_SIZE, (max_row+1)*TILE_SIZE)`，clamp 到畫面邊界
5. 沒有任何 tile 變動 → `is_empty = true`

邊緣 tile 處理：畫面寬高不整除 TILE_SIZE 時，最後一列/欄的 tile 實際大小是餘數，`memcmp` 按實際大小計算。

---

### compress.c / compress.h

```c
// 初始化：預先分配 compress buffer
// max_src_size = width * height * 3（最壞情況整幀）
int  compress_init(size_t max_src_size,
                   uint8_t **buf_out, size_t *buf_cap_out);

// 壓縮，回傳壓縮後大小；失敗回傳 0
size_t compress_data(const uint8_t *src, size_t src_len,
                     uint8_t *dst, size_t dst_cap);

void compress_destroy(uint8_t *buf);
```

實作：`ZSTD_compress(dst, dst_cap, src, src_len, 1)`，level=1 最快。

---

### scale.c / scale.h

```c
// nearest-neighbor downscale
// dst 需預先分配 dst_w * dst_h * 3 bytes
void scale_rgb(const uint8_t *src, uint32_t src_w, uint32_t src_h,
               uint8_t       *dst, uint32_t dst_w, uint32_t dst_h);
```

---

### transport.c / transport.h

```c
// 組裝 frame_header + data，寫入 stdout
// 使用 writev() 或預先 memcpy 進 protocol_buf，一次 write()
void transport_send_frame(const struct dirty_rect *rect,
                          uint32_t full_w, uint32_t full_h,
                          const uint8_t *data, size_t data_size,
                          uint8_t flags, uint32_t seq);

// 傳送 skip frame（畫面沒變）
void transport_send_skip(uint32_t full_w, uint32_t full_h, uint32_t seq);
```

寫出策略：header + data 先組進 `protocol_buf`，一次 `fwrite()` + `fflush()`。

---

### input.c / input.h

```c
// 啟動 input thread，從 stdin 讀 input_packet，ydotool 注入
int  input_start(uint32_t screen_w, uint32_t screen_h);
void input_stop(void);
```

內部用獨立 pthread，`read(STDIN_FILENO, ...)` 讀 10 bytes packet，解析後：

```c
// 鍵盤
// flags & 0x01（press）→ ydotool key <code> down
// flags & 0x02（release）→ ydotool key <code> up

// 滑鼠
// ydotool mousemove --absolute -- <mx> <my>
// flags != 0 → ydotool click <button>
```

或直接寫 `/dev/uinput`，不 fork ydotool（效能更好）。

---

### main.c

```c
int main(int argc, char *argv[]) {
    // 解析參數：--scale WxH
    // SIGINT handler → set running = false

    capture_init(&ctx);
    compress_init(ctx.width * ctx.height * 3, &comp_buf, &comp_cap);
    input_start(ctx.width, ctx.height);

    uint32_t seq = 0;
    bool first = true;

    while (running) {
        capture_frame(&ctx);        // 阻塞直到下一幀就緒

        uint8_t *frame = ctx.rgb_curr;
        uint32_t fw = ctx.width, fh = ctx.height;

        // 降解析度
        if (scale_enabled) {
            scale_rgb(frame, fw, fh, scale_buf, target_w, target_h);
            frame = scale_buf;
            fw = target_w; fh = target_h;
        }

        // 第一幀強制全幀
        struct dirty_rect rect;
        if (first) {
            rect = (struct dirty_rect){0, 0, fw, fh, true, false};
            first = false;
        } else {
            rect = diff_compute(prev_rgb, frame, fw, fh);
        }

        if (rect.is_empty) {
            transport_send_skip(fw, fh, seq++);
            memcpy(prev_rgb, frame, fw * fh * 3);
            continue;
        }

        // 取出 dirty rect → 連續 buffer
        extract_rect(frame, fw, &rect, dirty_buf);

        // zstd 壓縮
        size_t comp_size = compress_data(dirty_buf, rect.w * rect.h * 3,
                                         comp_buf, comp_cap);

        uint8_t flags = FLAG_COMPRESSED;
        if (rect.is_full) flags |= FLAG_FULL_FRAME;

        transport_send_frame(&rect, fw, fh,
                             comp_buf, comp_size,
                             flags, seq++);

        memcpy(prev_rgb, frame, fw * fh * 3);
    }

    input_stop();
    compress_destroy(comp_buf);
    capture_destroy(&ctx);
    return 0;
}
```

---

### Makefile

```makefile
CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra
LIBS    = -lwayland-client -lzstd -lpthread

PROTO_DIR = proto
PROTO_XML = $(PROTO_DIR)/wlr-screencopy-unstable-v1.xml
PROTO_H   = $(PROTO_DIR)/wlr-screencopy-unstable-v1-client-protocol.h
PROTO_C   = $(PROTO_DIR)/wlr-screencopy-unstable-v1-client-protocol.c

SRCS = src/main.c src/capture.c src/diff.c src/compress.c \
       src/scale.c src/transport.c src/input.c $(PROTO_C)

$(PROTO_H): $(PROTO_XML)
	wayland-scanner client-header $< $@

$(PROTO_C): $(PROTO_XML)
	wayland-scanner private-code $< $@

kgp-remote: $(SRCS) $(PROTO_H)
	$(CC) $(CFLAGS) -I$(PROTO_DIR) $(SRCS) $(LIBS) -o $@

clean:
	rm -f kgp-remote $(PROTO_H) $(PROTO_C)
```

### 外部依賴（Arch Linux）

```
libwayland-client  → wayland
wayland-scanner    → wayland
wlr-protocols      → wlr-protocols
libzstd            → zstd
ydotool            → ydotool（或直接寫 /dev/uinput）
```

---

## 本地：kgp-client.py（Python kitten）

放置位置：`~/.config/kitty/kgp-client.py`

執行方式：`kitty +kitten kgp-client.py user@remote [--scale WxH]`

### Python 依賴

```bash
pip install zstd     # zstd 解壓
# 無其他非標準庫依賴
```

### 模組結構

```python
# kgp-client.py
#
# 模組：
#   SSHTunnel      → 管理 SSH process（stdout pipe）
#   FrameReader    → 從 SSH stdout 讀 binary frame（header + data）
#   ZstdDecoder    → zstd 解壓
#   KittyRenderer  → RGB24 → base64 → Kitty GP escape sequence
#   InputCapture   → 讀 Kitty Keyboard Protocol + SGR mouse
#   InputSender    → 把 input event 打包成 input_packet → SSH stdin
#   main()         → 串接以上，render thread + input thread
```

### Terminal 設定（啟動時送到 stdout）

```python
SETUP = (
    b'\033[?1049h'    # 切換 alternate screen
    b'\033[2J'        # 清畫面
    b'\033[H'         # 游標回原點
    b'\033[?25l'      # 隱藏游標
    b'\033[?1003h'    # any motion mouse tracking
    b'\033[?1006h'    # SGR 1006 extended mouse encoding
    b'\033[>11u'      # Kitty Keyboard Protocol：
                      #   1=disambiguate + 2=report events + 8=report all keys
)

TEARDOWN = (
    b'\033[<u'        # 還原 Kitty Keyboard Protocol
    b'\033[?1003l'    # 關閉 mouse tracking
    b'\033[?1006l'
    b'\033[?25h'      # 恢復游標
    b'\033[?1049l'    # 離開 alternate screen
)
```

### FrameReader

```python
import struct

MAGIC    = b'KGPF'
HDR_FMT  = '>4sBBIHHHHHHI'
HDR_SIZE = struct.calcsize(HDR_FMT)  # 26 bytes

FLAG_FULL_FRAME = 0x01
FLAG_COMPRESSED = 0x02
FLAG_SKIP       = 0x04

class Frame:
    x: int; y: int; w: int; h: int
    full_w: int; full_h: int
    seq: int
    is_full: bool
    is_skip: bool
    rgb24: bytes   # 解壓後的 RGB24，skip 時為 None

def read_frame(stream) -> Frame:
    hdr = recv_exactly(stream, HDR_SIZE)
    magic, ver, flags, seq, x, y, w, h, fw, fh, dsize = \
        struct.unpack(HDR_FMT, hdr)
    assert magic == MAGIC

    f = Frame()
    f.x, f.y, f.w, f.h = x, y, w, h
    f.full_w, f.full_h = fw, fh
    f.seq = seq
    f.is_full = bool(flags & FLAG_FULL_FRAME)
    f.is_skip = bool(flags & FLAG_SKIP)

    if dsize > 0:
        data = recv_exactly(stream, dsize)
        if flags & FLAG_COMPRESSED:
            import zstd
            f.rgb24 = zstd.decompress(data)
        else:
            f.rgb24 = data
    else:
        f.rgb24 = None

    return f

def recv_exactly(stream, n) -> bytes:
    buf = b''
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise EOFError('SSH connection closed')
        buf += chunk
    return buf
```

### KittyRenderer

```python
import base64, random, sys

CHUNK_SIZE = 4096

class KittyRenderer:
    def __init__(self, screen_rows, screen_cols, cell_w_px, cell_h_px):
        self.kitty_id     = random.randint(1, 2**31)
        self.frame_number = 0
        self.rows         = screen_rows
        self.cols         = screen_cols
        self.cell_w       = cell_w_px
        self.cell_h       = cell_h_px
        self.out          = sys.stdout.buffer

    def render(self, frame: Frame):
        if frame.is_skip:
            return

        encoded = base64.b64encode(frame.rgb24)
        total   = len(encoded)
        offset  = 0
        first_chunk = True

        while offset < total:
            chunk = encoded[offset:offset + CHUNK_SIZE]
            more  = 1 if offset + CHUNK_SIZE < total else 0

            if first_chunk:
                if self.frame_number == 0:
                    # 第一幀：a=T，建立 image
                    hdr = (f'\033_Ga=T,i={self.kitty_id},f=24,q=2,'
                           f'c={self.cols},r={self.rows},'
                           f's={frame.w},v={frame.h},m={more};')
                else:
                    # 後續幀：a=f，更新 dirty rect
                    cx = frame.x // self.cell_w
                    cy = frame.y // self.cell_h
                    hdr = (f'\033_Ga=f,r=1,i={self.kitty_id},f=24,q=2,'
                           f'x={cx},y={cy},'
                           f's={frame.w},v={frame.h},m={more};')
                first_chunk = False
            else:
                if self.frame_number == 0:
                    hdr = f'\033_Gm={more};'
                else:
                    hdr = f'\033_Ga=f,r=1,q=2,m={more};'

            self.out.write(hdr.encode() + chunk + b'\033\\')
            offset += CHUNK_SIZE

        # 後續幀需要 a=a 觸發顯示
        if self.frame_number > 0:
            self.out.write(
                f'\033_Ga=a,q=2,c=1,i={self.kitty_id};\033\\'.encode()
            )

        self.out.flush()
        self.frame_number += 1

    def destroy(self):
        self.out.write(
            f'\033_Ga=d,q=2,i={self.kitty_id};\033\\'.encode()
        )
        self.out.flush()
```

### InputCapture + InputSender

```python
import struct, threading, select, termios, tty

INPUT_FMT = '>BBiHH'   # type, flags, code, mx, my（10 bytes）

INPUT_KEY   = 1
INPUT_MOUSE = 2

class InputHandler:
    def __init__(self, ssh_stdin):
        self.sink = ssh_stdin
        self._stop = threading.Event()

    def start(self):
        t = threading.Thread(target=self._loop, daemon=True)
        t.start()

    def stop(self):
        self._stop.set()

    def _loop(self):
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        new = termios.tcgetattr(fd)
        new[3] &= ~(termios.ISIG | termios.ICANON | termios.ECHO)
        termios.tcsetattr(fd, termios.TCSADRAIN, new)

        try:
            parser = KittyInputParser()
            while not self._stop.is_set():
                r, _, _ = select.select([sys.stdin], [], [], 0.05)
                if not r:
                    continue
                data = sys.stdin.buffer.read(256)
                for ev in parser.parse(data):
                    self._send(ev)
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)

    def _send(self, ev):
        if ev['type'] == 'key':
            pkt = struct.pack(INPUT_FMT,
                INPUT_KEY, ev['flags'], ev['code'], 0, 0)
        else:  # mouse
            pkt = struct.pack(INPUT_FMT,
                INPUT_MOUSE, ev['buttons'], 0, ev['x'], ev['y'])
        self.sink.write(pkt)
        self.sink.flush()
```

### KittyInputParser

解析 Kitty Keyboard Protocol（`CSI ... u`）和 SGR mouse（`CSI < ... M/m`）：

```python
class KittyInputParser:
    """
    Kitty Keyboard Protocol：
      CSI <codepoint> ; <modifiers> : <event_type> u
      event_type: 1=press 2=repeat 3=release

    SGR Mouse：
      CSI < <cb> ; <cx> ; <cy> M   → press
      CSI < <cb> ; <cx> ; <cy> m   → release
    """

    def parse(self, data: bytes) -> list:
        events = []
        buf = data.decode('utf-8', errors='replace')
        i = 0
        while i < len(buf):
            if buf[i] == '\x1b' and i+1 < len(buf) and buf[i+1] == '[':
                # CSI sequence
                end = i + 2
                while end < len(buf) and buf[end] not in 'ABCDEFGHIJKLMPSTXZabcdefghijklmnopqrstuvwxyz~u':
                    end += 1
                if end < len(buf):
                    seq = buf[i:end+1]
                    ev = self._parse_csi(seq)
                    if ev:
                        events.append(ev)
                    i = end + 1
                else:
                    i += 1
            else:
                i += 1
        return events

    def _parse_csi(self, seq):
        # Kitty keyboard：CSI ... u
        if seq.endswith('u'):
            inner = seq[2:-1]  # 去掉 ESC[ 和 u
            parts = inner.split(';')
            codepoint = int(parts[0]) if parts[0] else 0
            mods = int(parts[1]) if len(parts) > 1 and parts[1] else 1
            # event type 在 mods 的 : 後面
            event_type = 1  # press
            if ':' in (parts[1] if len(parts) > 1 else ''):
                m, e = parts[1].split(':')
                mods = int(m) if m else 1
                event_type = int(e) if e else 1

            flags = 0
            if event_type == 1: flags |= 0x01  # press
            if event_type == 3: flags |= 0x02  # release
            if event_type == 2: flags |= 0x04  # repeat

            return {'type': 'key', 'flags': flags,
                    'code': codepoint, 'mods': mods}

        # SGR mouse：CSI < cb ; cx ; cy M/m
        if seq[2:3] == '<' and seq[-1] in 'Mm':
            inner = seq[3:-1]
            cb, cx, cy = (int(x) for x in inner.split(';'))
            pressed = seq[-1] == 'M'
            buttons = cb & 0x03 if pressed else 0
            return {'type': 'mouse', 'buttons': buttons,
                    'x': cx - 1, 'y': cy - 1}  # 1-based → 0-based

        return None
```

### main()

```python
import subprocess, time, shutil

def query_terminal_cell_size():
    """回傳 (cell_w_px, cell_h_px, cols, rows)"""
    sys.stdout.buffer.write(b'\033[14t')  # 查 pixel 大小
    sys.stdout.buffer.flush()

    import re
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    resp = b''
    try:
        while True:
            r, _, _ = select.select([sys.stdin], [], [], 1.0)
            if not r: break
            ch = sys.stdin.buffer.read(1)
            resp += ch
            if ch == b't': break
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)

    m = re.search(rb'\[4;(\d+);(\d+)t', resp)
    total_h, total_w = (int(m.group(1)), int(m.group(2))) if m else (1080, 1920)
    cols, rows = shutil.get_terminal_size()
    return total_w // cols, total_h // rows, cols, rows


def main(args):
    if len(args) < 2:
        print('Usage: kitty +kitten kgp-client.py user@host [--scale WxH]')
        return

    remote = args[1]

    # 啟動 SSH，kgp-remote 的 stdout → pipe，stdin ← pipe
    ssh = subprocess.Popen(
        ['ssh', '-T', remote, 'kgp-remote'],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )

    # Terminal 設定
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

    except (EOFError, BrokenPipeError):
        pass
    finally:
        input_handler.stop()
        renderer.destroy()
        sys.stdout.buffer.write(TEARDOWN)
        sys.stdout.buffer.flush()
        ssh.terminate()
```

---

## 實作順序

```
遠端端：
  Step 1：capture.c
    → wlr-screencopy 抓幀，XRGB→RGB24，確認輸出正確

  Step 2：transport.c（只傳 raw，不壓縮）
    → 寫 frame_header + RGB24 到 stdout
    → ssh user@remote kgp-remote | xxd 驗證 header 格式

  Step 3：diff.c
    → tile-based dirty rect
    → 測試靜態桌面的 is_empty 比例

  Step 4：compress.c
    → zstd level=1 壓縮 dirty rect
    → 測量壓縮比和耗時

  Step 5：input.c
    → 讀 stdin input_packet，ydotool 注入

本地端：
  Step 6：FrameReader + KittyRenderer
    → 接收 step 2 的 raw stream，顯示靜態畫面

  Step 7：整合連續幀
    → a=T / a=f / a=a 三段邏輯，確認畫面流暢更新

  Step 8：InputCapture + InputSender
    → Kitty Keyboard Protocol 解析
    → SGR mouse 解析
    → 打包 input_packet 送回遠端

  Step 9：端對端測試
    → 確認輸入可以操作遠端 Hyprland
```

---

## 注意事項

**遠端：**
- `WAYLAND_DISPLAY` 透過 SSH 登入時不會自動繼承，需在啟動指令裡明確指定：`ssh user@remote 'WAYLAND_DISPLAY=wayland-1 kgp-remote'`
- `ydotool` 需要 `ydotoold` daemon 在遠端跑，或改用直接寫 `/dev/uinput`（需要 `input` group 權限）
- frame_header 的數值全用 big-endian（`>` in struct），兩端一致
- `transport_send_frame` 用 `writev()` 或 memcpy 成連續 buffer 再一次 `write()`，避免 header 和 data 分兩次 syscall 造成 TCP 分包

**本地：**
- Kitty Keyboard Protocol 要搭配 `tcsetattr` 關掉 `ISIG`，否則 Ctrl+C 仍被 PTY 攔截
- `query_terminal_cell_size` 需要在 raw mode 下讀回應，注意 timeout
- kitten 的 `main()` 不需要 `handle_result()`，直接跑完退出即可
- SSH 用 `-T`（不分配 PTY），讓 stdout 保持 binary 模式，不被 PTY line discipline 干擾

---

## 參考資料

- kitty-doom（https://github.com/jserv/kitty-doom）：Kitty GP 分塊傳輸、SIMD base64、frame diff
- grim（https://git.sr.ht/~emersion/grim）：wlr-screencopy C 實作
- Kitty Graphics Protocol 規格：https://sw.kovidgoyal.net/kitty/graphics-protocol/
- Kitty Keyboard Protocol 規格：https://sw.kovidgoyal.net/kitty/keyboard-protocol/
