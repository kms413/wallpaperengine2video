# wallpaperengine2video

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](./LICENSE)
[![Platform: Linux](https://img.shields.io/badge/platform-Linux%20(Wayland)-green.svg)](#依赖依赖项)
[![Python](https://img.shields.io/badge/python-3.10%2B-blue.svg)](#依赖依赖项)

**把 Wallpaper Engine 壁纸导出为恒定帧率 mp4 的 Linux 命令行工具。**
**Export Wallpaper Engine wallpapers (video & scene) to constant-frame-rate mp4 on Linux.**

视频壁纸无损直出；场景壁纸通过"时钟冻结 + 逐帧渲染"实现**零丢帧的确定性导出**——GPU 性能只影响导出耗时，绝不影响输出质量。

- [中文](#中文) · [English](#english)

---

## 中文

### 特性

- **视频壁纸**：mp4 直接 `copy` 无损导出，webm/mkv/mov 自动转码
- **场景壁纸**：定步长离线渲染（核心亮点，见[工作原理](#工作原理)）
  - 输出帧数严格等于 `时长 × fps`，零重复帧、零丢帧
  - 每一帧都是渲染器完整画完才采集的确定性画面
- **`.mpkg` 文件**：内置 `PKGM0014` 格式解析器，直接解包导出
- **自动发现**：自动扫描全部 Steam 库的 Workshop 内容（AppID 431960），从 `project.json` 读取标题并分类
- **编码**：实时回退管线优先 VAAPI 硬编（省 CPU），逐步渲染管线用 x264 CRF 18
- **健壮**：任一环节失败自动回退到实时录制，视频导出永不中断

### 工作原理

场景壁纸（.pkg）无法像视频一样直接转码，传统做法是实时录屏——但 GPU 一旦错过合成器的帧截止时间，那一帧动画就永久丢失，只能用重复帧填补，这就是实时录制卡顿的根源。

本工具的解法是把**渲染器的时钟**劫持掉：

```
                    ┌──────────────────────────────────────────┐
  无头 sway         │  linux-wallpaperengine                   │
  (headless         │    └─ LD_PRELOAD we_clock.so             │
   Wayland 合成器)  │       虚拟时钟冻结，FIFO 收到 "step"      │
                    │       才前进 1/fps 秒                    │
                    └──────────────┬───────────────────────────┘
                                   │ 每步进一次 = 一帧确定的动画状态
                                   ▼
                    grim 抓帧 → ffmpeg image2pipe → x264 → mp4
```

1. `we_clock.so`（LD_PRELOAD 垫片）接管进程内的 `clock_gettime`/`gettimeofday`，lwe、CEF、mpv、std::chrono 看到的全部是同一个被冻结的虚拟时钟
2. 每向控制 FIFO 写一次 `step`，虚拟时间前进 1/fps 秒，渲染器画出下一帧
3. 用探针帧 hash 感知画面变化，等渲染完成后再全帧抓取（GPU 再慢也只是等）
4. 帧序列按精确 fps 编码——输出帧数、时长、帧率全部精确命中

GPU 性能因此与输出正确性**完全解耦**：导入耗时约为视频时长的 3~4 倍，但每一帧都完整渲染。

### 依赖

| 组件 | 用途 | 必需性 |
|---|---|---|
| python3 ≥ 3.10、ffmpeg | 主程序 / 编码 | 必需 |
| sway | 无头离屏合成器 | 场景壁纸必需 |
| grim | 逐帧抓取 | 场景壁纸必需 |
| [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) | 场景壁纸渲染器 | 场景壁纸必需 |
| gcc / make | 编译时钟垫片 | 构建时必需 |
| wf-recorder | 实时回退管线 | 可选（`--realtime`） |

安装依赖（Debian/Ubuntu）：

```bash
sudo apt install python3 ffmpeg sway grim gcc make wf-recorder
# linux-wallpaperengine 请按上游文档编译安装
```

### 安装

```bash
git clone https://github.com/<your-name>/wallpaperengine2video.git
cd wallpaperengine2video
make            # 编译 lib/we_clock.so
make install    # 安装到 ~/.local/bin（无需 root）
```

### 使用

```bash
wallpaperengine2video --list              # 列出本地全部壁纸及类型
wallpaperengine2video 3497756108          # 按工坊 ID 导出（5 秒 1080p60 默认）
wallpaperengine2video 3497756108 -t 15 --fps 60 --size 1920x1080
wallpaperengine2video ~/Downloads/xxx.mpkg   # 导出 .mpkg
wallpaperengine2video --all -o ~/videos   # 导出全部壁纸
wallpaperengine2video 3497756108 --json   # JSON 输出（便于脚本处理）
```

| 参数 | 说明 | 默认 |
|---|---|---|
| `-o, --out DIR` | 输出目录 | `./mp4` |
| `-t, --seconds N` | 场景渲染时长（秒） | `10` |
| `--size WxH` | 渲染分辨率 | `1920x1080` |
| `--fps N` | 帧率 | `60` |
| `--all` | 导出全部壁纸 | |
| `--list` | 列出壁纸及类型 | |
| `--json` | JSON 输出 | |
| `--force` | 覆盖已存在文件 | |
| `--realtime` | 强制实时录制（旧行为，帧率受 GPU 影响） | |

### 已知限制

- 场景导出**无音轨**（渲染为静音）；视频壁纸直出保留原音轨
- 导出的时间轴从时钟冻结瞬间开始（即壁纸播放中途的某一刻）
- CEF 网页壁纸在冻结时钟下可能初始化失败，会自动回退实时录制
- 噪声/粒子类壁纸的步进判定按画面 hash 比较，极高速动画可能引入 ≤1 帧的采集相位偏差

---

## English

A Linux CLI tool that exports Wallpaper Engine wallpapers to constant-frame-rate mp4.

- **Video wallpapers**: lossless stream-copy for mp4 sources, transcode for webm/mkv/mov.
- **Scene wallpapers**: *deterministic frame-stepped rendering* — an `LD_PRELOAD` shim (`lib/we_clock.c`) freezes the renderer's virtual clock; each `step` command advances time by exactly 1/fps seconds and one fully-rendered frame is captured. GPU speed affects only export wall-time, never the output: frame count is exactly `duration × fps`, with zero dropped or duplicated frames.
- **`.mpkg` files**: built-in `PKGM0014` parser.
- **Automatic discovery**: scans all Steam library Workshop content (AppID 431960), classifies wallpapers from `project.json`.

### Requirements

- python3 ≥ 3.10, ffmpeg (required)
- sway + grim + [linux-wallpaperengine](https://github.com/Almamu/linux-wallpaperengine) (scene wallpapers)
- gcc + make (to build the clock shim)
- wf-recorder (optional, only for `--realtime` fallback)

### Install & usage

```bash
git clone https://github.com/<your-name>/wallpaperengine2video.git
cd wallpaperengine2video && make && make install

wallpaperengine2video --list
wallpaperengine2video <workshop_id> -t 15 --fps 60 --size 1920x1080 -o out/
wallpaperengine2video --all --json
wallpaperengine2video <workshop_id> --realtime   # legacy realtime capture
```

See the 中文 section above for full documentation, architecture and limitations.

### How the deterministic renderer works (short version)

Realtime screen capture stutters because any frame the GPU misses its compositor deadline is lost forever. Instead, this tool preloads a shim that intercepts `clock_gettime`/`gettimeofday` inside `linux-wallpaperengine`, freezing its view of time. A control FIFO accepts `step` commands that advance the virtual clock by 1/fps seconds; after each step the tool waits (however long the GPU needs) until the frame is fully rendered, then grabs it with `grim` and pipes it to ffmpeg. Output timing is therefore exact by construction.

---

## 许可证 / License

[AGPL-3.0](./LICENSE) © 2026
