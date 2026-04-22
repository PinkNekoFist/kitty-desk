# kgp-test：Hyprland 畫面輸出到 Kitty terminal（Stage 2：inline base64 連續幀）

## 目標

在 Stage 1（`t=f` 單幀驗證）確認 pipeline 正確後，進入正式渲染路徑：

- 改用 inline base64（`t=d`）取代檔案路徑傳輸
- 實作連續幀迴圈，持續抓幀並用 `a=T` / `a=f` / `a=a` 三段邏輯更新畫面
- 環境：筆電跑 Hyprland，Kitty 在 Hyprland 裡開著
- 範圍：純本地，不考慮 SSH 和輸入回傳

## Pipeline

```
Hyprland（跑著）
    │
    │ wlr-screencopy（zwlr_screencopy_manager_v1）
    ▼
shm buffer（XRGB8888, width × height × 4 bytes）
    │
    │ XRGB → RGB24（去掉 X channel）
    ▼
RGB24 raw data（width × height × 3 bytes）
    │
    │ base64 encode（scalar 查表法，後續可換 SIMD）
    ▼
base64 字串（膨脹比 4/3）
    │
    │ 分成 4096 bytes 一塊
    │ 第一幀：a=T 建立 image
    │ 後續幀：a=f 更新 frame + a=a 觸發顯示
    ▼
批次 fwrite() 到 stdout → Kitty 渲染
```

## 專案結構

```
kgp-test/
├── Makefile
├── proto/
│   └── wlr-screencopy-unstable-v1.xml   # 從 wlroots 取得
└── src/
    ├── main.c      # 主迴圈、Wayland 初始化、串接各模組
    ├── capture.c   # wlr-screencopy：連接 Wayland、抓幀、拿 shm buffer
    ├── capture.h
    ├── base64.c    # scalar base64 encode（查表法）
    ├── base64.h
    ├── kitty.c     # renderer：RGB24 → base64 → Kitty GP → stdout
    └── kitty.h
```

`proto/wlr-screencopy-unstable-v1.xml` 在 build time 用 `wayland-scanner` 產生
`wlr-screencopy-unstable-v1-client-protocol.h` 和
`wlr-screencopy-unstable-v1-client-protocol.c`，不需要手動維護。

## 各模組職責

### main.c

- 呼叫 `wl_display_connect()` 連接 `$WAYLAND_DISPLAY`
- 用 `wl_registry` 綁定：
  - `zwlr_screencopy_manager_v1`
  - `wl_output`（取第一個 output 即可）
  - `wl_shm`
- 進入主迴圈：每幀呼叫 capture，拿到 RGB24 後交給 kitty 輸出
- 程式結束時送 Kitty GP cleanup sequence

### capture.c / capture.h

負責和 Wayland 溝通，對外只暴露一個函式：

```c
// 抓一幀，回傳 RGB24 data、寬高
// 內部用 zwlr_screencopy_frame_v1 + wl_shm
int capture_frame(struct capture_ctx *ctx,
                  uint8_t **rgb_out,
                  uint32_t *width, uint32_t *height);
```

內部流程：
1. `screencopy_manager.capture_output()` 發出抓幀請求
2. 收到 `buffer_event`：得知需要的 buffer 格式（XRGB8888）和尺寸
3. 用 `memfd_create` + `mmap` 建立 shm buffer
4. `frame.copy(buffer)` 讓 compositor 把幀複製進去
5. 收到 `ready_event`：幀資料就緒
6. XRGB8888 → RGB24 轉換（去掉每個 pixel 的 X byte）
7. 回傳 RGB24 pointer

注意事項：
- format 固定使用 `WL_SHM_FORMAT_XRGB8888`（little-endian：byte order 為 B G R X）
- RGB 轉換：`r = row[x*4+2], g = row[x*4+1], b = row[x*4+0]`
- 每次抓幀後重新 `capture_output()`，不要重用 frame object

### base64.c / base64.h

scalar 查表法實作，這個階段不做 SIMD 優化：

```c
// 回傳 encode 後的 byte 數
size_t base64_encode(const uint8_t *src, size_t src_len, uint8_t *dst);
```

每次處理 3 bytes → 4 chars，標準查表法。`dst` buffer 大小需為 `4 * ((src_len + 2) / 3) + 1`。

### kitty.c / kitty.h

持有 renderer 狀態，對外暴露三個函式：

```c
// 初始化：清畫面、產生 kitty_id、分配 encode buffer
struct kitty_renderer *kitty_renderer_create(int rows, int cols);

// 渲染一幀（內部判斷是第一幀還是後續幀）
void kitty_render_frame(struct kitty_renderer *r,
                        const uint8_t *rgb,
                        uint32_t width, uint32_t height);

// 清理：送 delete image 指令、還原 terminal
void kitty_renderer_destroy(struct kitty_renderer *r);
```

`kitty_renderer` 內部狀態：

```c
struct kitty_renderer {
    long     kitty_id;           // 隨機產生，用於識別 image
    int      frame_number;       // 0 = 第一幀，>0 = 後續幀
    int      screen_rows;
    int      screen_cols;
    uint8_t *encode_buf;         // base64 output buffer，預先分配
    size_t   encode_buf_size;
    char    *protocol_buf;       // 批次 I/O buffer
    size_t   protocol_buf_size;
};
```

#### 三段式幀更新邏輯（直接參考 kitty-doom renderer.c）

**第一幀（`frame_number == 0`）**，建立 image：

```
第一塊：
\033_G a=T,i=<id>,f=24,s=<w>,v=<h>,q=2,c=<cols>,r=<rows>,m=1;<chunk>\033\\

continuation 塊：
\033_G m=1;<chunk>\033\\

最後一塊：
\033_G m=0;<chunk>\033\\
```

**後續幀（`frame_number > 0`）**，更新既有 image：

```
第一塊：
\033_G a=f,r=1,i=<id>,f=24,q=2,x=0,y=0,s=<w>,v=<h>,m=1;<chunk>\033\\

continuation 塊：
\033_G a=f,r=1,q=2,m=1;<chunk>\033\\

最後一塊：
\033_G a=f,r=1,q=2,m=0;<chunk>\033\\
```

**每幀結束後**，觸發顯示（後續幀才需要）：

```
\033_G a=a,q=2,c=1,i=<id>;\033\\
```

`a=a,c=1` 是 animation 指令，告訴 Kitty 播放剛加入的 frame。沒有這行，`a=f` 加入的 frame 不會顯示。

#### I/O 批次策略

所有 chunk 先 `memcpy` 進 `protocol_buf`，最後一次 `fwrite()` 送出，避免每個 chunk 各一次 syscall：

```c
// 不是：
for each chunk: write(chunk_header + chunk_data + trailer)

// 而是：
for each chunk: memcpy(protocol_buf + offset, ...)
fwrite(protocol_buf, total_offset, 1, stdout)
fflush(stdout)
```

#### chunk size

固定 4096 bytes base64 per chunk。一幀 1080p 的資料量：

```
1920 × 1080 × 3 = 6,220,800 bytes RGB24
base64 後       = 8,294,400 bytes
chunk 數量      = 約 2026 個
```

#### 程式結束清理

```c
void kitty_renderer_destroy(struct kitty_renderer *r) {
    // 刪除 Kitty 裡的 image
    printf("\033_G a=d,q=2,i=%ld;\033\\", r->kitty_id);
    // 還原畫面
    printf("\033[H\033[2J");
    // 還原 window title
    printf("\033]2;\033\\");
    fflush(stdout);
    free(r->encode_buf);
    free(r->protocol_buf);
    free(r);
}
```

## Makefile 重點

```makefile
# wayland-scanner 產生 protocol binding
proto/wlr-screencopy-unstable-v1-client-protocol.h: proto/wlr-screencopy-unstable-v1.xml
	wayland-scanner client-header $< $@

proto/wlr-screencopy-unstable-v1-client-protocol.c: proto/wlr-screencopy-unstable-v1.xml
	wayland-scanner private-code $< $@

# 編譯連結
CFLAGS = -std=c11 -O2 -Wall
LIBS   = -lwayland-client
```

## 外部依賴

| 依賴 | 用途 | 套件名（Arch） |
|------|------|---------------|
| `libwayland-client` | Wayland 連線 | `wayland` |
| `wayland-scanner` | build time 產生 protocol binding | `wayland` |
| `wlr-protocols` | 取得 wlr-screencopy xml | `wlr-protocols` |

zstd 壓縮和 dirty rect 在這個 PoC 階段不需要，先跑通 pipeline 再加。

## 實作順序

```
Step 1：實作 base64.c
  → scalar 查表法，先不做 SIMD
  → 寫一個小測試：encode 已知字串，驗證輸出符合標準

Step 2：實作 kitty_renderer_create / destroy
  → 清畫面、產生 kitty_id、分配 encode_buf 和 protocol_buf
  → 驗證 destroy 後 terminal 正常還原

Step 3：實作第一幀輸出（a=T）
  → 抓一幀，base64 encode，分塊送出
  → 確認 Kitty 正確顯示靜態畫面

Step 4：實作連續幀迴圈（a=f + a=a）
  → while loop 持續抓幀
  → 後續幀用 a=f 更新 + a=a 觸發顯示
  → 觀察幀率和畫面更新是否流暢

Step 5：測量各段耗時
  → wlr-screencopy 等待時間
  → XRGB→RGB 轉換時間
  → base64 encode 時間（決定是否需要 SIMD）
  → fwrite() 時間
```

## 參考專案

- **kitty-doom**（https://github.com/jserv/kitty-doom）
  - SIMD base64 encode（NEON/SSE3）可直接借用
  - SIMD frame diff（SSE2/SSE4.2）可直接借用
  - Kitty GP 分塊傳輸邏輯參考
  - 輸入處理（SGR 1006 mouse + VT state machine）供後續階段參考

- **grim**（https://git.sr.ht/~emersion/grim）
  - wlr-screencopy 的標準 C 實作參考
  - shm buffer 建立方式參考

## 注意事項

- `WAYLAND_DISPLAY` 在 Hyprland session 裡已自動設好，通常是 `wayland-1`
- `kitty_id` 用 `srand(time(NULL)); rand()` 產生即可，不需要保證唯一性
- `encode_buf` 大小：`4 * ((width * height * 3 + 2) / 3) + 1`
- `protocol_buf` 大小：`chunk 數量 × (4096 + 80 header + 2 trailer) + 64 animation cmd`
- `a=f` 的 continuation chunk header 不能省略 `a=f,r=1,q=2`，否則 Kitty 不知道這是 frame update
- 程式收到 SIGINT 時要確保 `kitty_renderer_destroy()` 被呼叫，否則 terminal 殘留 image
- 這個階段不處理 terminal resize，寬高在 `renderer_create` 時固定
