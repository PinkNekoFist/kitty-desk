# kgp-test：Hyprland 畫面輸出到 Kitty terminal（Stage 3：dirty rect + zstd + 降解析度）

## 前置狀態（已完成）

Stage 2 已完成並測量各段耗時，現有 pipeline：

```
wlr-screencopy → XRGB8888 shm buffer
→ XRGB → RGB24
→ scalar base64 encode
→ 分塊 Kitty GP（a=T / a=f / a=a）
→ 批次 fwrite() stdout
```

現有專案結構：

```
kgp-test/
├── Makefile
├── proto/
│   └── wlr-screencopy-unstable-v1.xml
└── src/
    ├── main.c
    ├── capture.c / capture.h
    ├── base64.c / base64.h
    └── kitty.c / kitty.h
```

---

## 目標

加入三個降低傳輸量的優化，優先順序依序實作：

1. **dirty rect**：只傳變動區域，減少靜態/少動畫面的傳輸量
2. **zstd 壓縮**：對 RGB24 壓縮後再 base64，典型桌面壓縮比 3–5x
3. **降解析度**：抓幀後 downscale，720p 傳輸量只有 1080p 的 44%

---

## 傳輸量基準（1080p，無優化）

```
RGB24 per 幀        = 1920 × 1080 × 3 = 6,220,800 bytes = 5.9 MB
base64 後           = 8,294,400 bytes  = 7.9 MB
1GbE 無壓縮上限     ≈ 14 FPS
跨網際網路 100Mbps  ≈ 1.5 FPS（不可用）
```

加入 zstd 後（壓縮比估 4x）：

```
zstd 壓縮後 RGB24   ≈ 1,555,200 bytes = 1.5 MB
base64 後           ≈ 2,073,600 bytes = 2.0 MB
1GbE 可達           ≈ 60 FPS
跨網際網路 100Mbps  ≈ 6 FPS（勉強可用）
```

---

## 新增模組

### diff.c / diff.h（dirty rect 計算）

```c
struct dirty_rect {
    uint32_t x, y;       // 左上角（pixel）
    uint32_t w, h;       // 寬高（pixel）
    bool     full_frame; // true = 全幀更新（第一幀或變動太大）
};

// 比較兩幀，回傳需要傳輸的 dirty rect
// prev 為 NULL 時直接回傳 full_frame
struct dirty_rect diff_compute(const uint8_t *prev_rgb,
                               const uint8_t *curr_rgb,
                               uint32_t width, uint32_t height);
```

實作策略：把畫面切成 N×N 的 tile（建議 64×64 pixels），標記哪些 tile 有變動，取所有變動 tile 的 bounding box 作為 dirty rect。

```
畫面切成 tile grid：
┌───┬───┬───┬───┐
│ . │ . │ X │ . │   X = 有變動
│ . │ X │ X │ . │   . = 無變動
│ . │ . │ . │ . │
└───┴───┴───┴───┘

dirty rect = 包住所有 X tile 的 bounding box
```

跳過傳輸的條件：變動 tile 數量為 0（畫面完全沒變）。

tile 比對用 `memcmp`，每個 tile `64 × 64 × 3 = 12,288 bytes`，後續可換 SIMD。

### compress.c / compress.h（zstd 壓縮）

```c
// 壓縮 RGB24 data，回傳壓縮後大小
// dst 需預先分配 ZSTD_compressBound(src_len) bytes
size_t compress_rgb(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap,
                    int level); // level 建議 1（最快）
```

壓縮對象是 **dirty rect 的 RGB24 data**，不是整幀。流程：

```
full RGB24 → 取 dirty rect 區域 → zstd 壓縮 → base64 encode → Kitty GP
```

注意：**Kitty GP 本身不認識 zstd**，壓縮和解壓都在程式端處理——壓縮後的 binary 仍要 base64 encode 後塞進 escape sequence，Kitty 收到的依然是 RGB24，只是傳輸過程中被壓縮。因此 zstd 只對 **SSH pipe 的傳輸量**有效，本地 PoC 測試時效果不明顯，主要為 SSH 階段預備。

### scale.c / scale.h（降解析度）

```c
// nearest-neighbor downscale，src → dst
// dst 需預先分配 dst_w * dst_h * 3 bytes
void scale_rgb(const uint8_t *src, uint32_t src_w, uint32_t src_h,
               uint8_t       *dst, uint32_t dst_w, uint32_t dst_h);
```

目標解析度由 command line 參數指定，預設不縮放。nearest-neighbor 實作簡單，品質夠用，後續可換 bilinear。

各解析度傳輸量對比：

```
解析度      RGB24/幀   base64/幀   zstd+base64/幀（估）
─────────────────────────────────────────────────────
1920×1080   5.9 MB    7.9 MB      2.0 MB
1280×720    2.6 MB    3.5 MB      0.9 MB
960×540     1.5 MB    2.0 MB      0.5 MB
640×360     0.7 MB    0.9 MB      0.2 MB
```

---

## 修改現有模組

### capture.c

新增 `prev_rgb` buffer，每幀抓完後保留一份，供 diff 使用：

```c
struct capture_ctx {
    // ... 現有欄位 ...
    uint8_t *prev_rgb;   // 上一幀的 RGB24，用於 dirty rect 比對
};
```

### kitty.c

`kitty_render_frame()` 介面擴充，接受 dirty rect 參數：

```c
void kitty_render_frame(struct kitty_renderer *r,
                        const uint8_t *rgb,
                        uint32_t width, uint32_t height,
                        struct dirty_rect rect);  // 新增
```

當 `rect.full_frame == false` 時，Kitty GP 的 `x`, `y`, `s`, `v` 參數改用 dirty rect 的值：

```
\033_G a=f,r=1,i=<id>,f=24,q=2,
       x=<rect.x / cell_w>,
       y=<rect.y / cell_h>,
       s=<rect.w>,
       v=<rect.h>,
       m=1;<base64 of dirty rect RGB>\033\\
```

dirty rect 的 RGB24 data 需要從完整幀逐行 `memcpy` 取出（原始資料是 row-major，dirty rect 不是連續記憶體）：

```c
for (uint32_t row = rect.y; row < rect.y + rect.h; row++) {
    memcpy(dst + (row - rect.y) * rect.w * 3,
           rgb + row * width * 3 + rect.x * 3,
           rect.w * 3);
}
```

### main.c

主迴圈加入完整流程：

```c
while (running) {
    capture_frame(&ctx, &rgb, &w, &h);

    // 1. 降解析度（如有指定）
    if (scale_enabled)
        scale_rgb(rgb, w, h, scaled_buf, target_w, target_h);

    // 2. dirty rect 計算
    struct dirty_rect rect = diff_compute(prev_rgb, curr_rgb, w, h);
    if (!rect.full_frame && rect.w == 0)
        continue; // 畫面沒變，跳過這幀

    // 3. 取出 dirty rect 資料到連續 buffer
    extract_dirty_rect(curr_rgb, w, rect, dirty_buf);

    // 4. zstd 壓縮（SSH 階段才有意義，本地可 bypass）
    size_t compressed_size = compress_rgb(dirty_buf, rect.w * rect.h * 3,
                                          compress_buf, compress_buf_cap, 1);

    // 5. 渲染
    kitty_render_frame(r,
                       compressed_size > 0 ? compress_buf : dirty_buf,
                       rect.w, rect.h,
                       rect);

    // 6. 更新 prev
    memcpy(prev_rgb, curr_rgb, w * h * 3);
}
```

---

## 更新後的專案結構

```
kgp-test/
├── Makefile
├── proto/
│   └── wlr-screencopy-unstable-v1.xml
└── src/
    ├── main.c
    ├── capture.c / capture.h    # 新增 prev_rgb buffer
    ├── base64.c / base64.h      # 不變
    ├── kitty.c / kitty.h        # 擴充接受 dirty_rect 參數
    ├── diff.c / diff.h          # 新增：tile-based dirty rect 計算
    ├── compress.c / compress.h  # 新增：zstd 壓縮
    └── scale.c / scale.h        # 新增：nearest-neighbor downscale
```

---

## Makefile 更新

```makefile
LIBS = -lwayland-client -lzstd
```

---

## 外部依賴更新

| 依賴 | 用途 | 套件名（Arch） |
|------|------|---------------|
| `libwayland-client` | Wayland 連線 | `wayland` |
| `wayland-scanner` | build time protocol binding | `wayland` |
| `wlr-protocols` | wlr-screencopy xml | `wlr-protocols` |
| `libzstd` | zstd 壓縮 | `zstd` |

---

## 實作順序

```
Step 1：diff.c
  → tile-based dirty rect 計算（tile size = 64×64）
  → 測試：全黑幀 vs 部分變動幀，確認 bounding box 正確
  → 測試：靜態桌面的跳過幀比例

Step 2：整合 dirty rect 進主迴圈
  → capture → diff → extract_dirty_rect → kitty_render_frame
  → 測量：靜態桌面的傳輸量降幅
  → 測量：全動畫面（例如播影片）的 dirty rect 效果

Step 3：scale.c
  → nearest-neighbor downscale
  → 加入 --scale WxH command line 參數
  → 測試 720p / 540p 的幀率和畫質

Step 4：compress.c
  → zstd level=1 壓縮 dirty rect 資料
  → 本地測量壓縮耗時 vs 傳輸量降幅
  → 預備 SSH 階段使用
```

---

## 注意事項

- dirty rect 的 `x`, `y` 換算成 Kitty cell 座標時，用 `\033[14t` 查到的 cell pixel 大小整除，向下取整
- tile grid 的邊緣 tile 可能不足 64×64（畫面寬高不整除時），`memcmp` 要按實際大小計算
- zstd 壓縮後的 binary 仍需 base64 encode，`encode_buf` 大小要按壓縮後大小重算：`4 * ((compressed_size + 2) / 3) + 1`
- `compress_buf` 預先分配 `ZSTD_compressBound(max_dirty_size)` bytes，`max_dirty_size = width * height * 3`（最壞情況整幀）
- scale 後的 buffer 要獨立分配，不覆蓋原始 RGB24（diff 需要原始解析度的 prev_rgb）
- 程式收到 SIGINT 確保 `kitty_renderer_destroy()` 被呼叫

---

## 參考專案

- **kitty-doom**（https://github.com/jserv/kitty-doom）
  - SIMD frame diff（SSE2/SSE4.2）供 diff.c 後續優化參考
  - SIMD base64 encode（NEON/SSE3）供 base64.c 後續優化參考

- **grim**（https://git.sr.ht/~emersion/grim）
  - wlr-screencopy shm buffer 參考
