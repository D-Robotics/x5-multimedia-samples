# sample_lvgl_camera 使用说明

## 功能概述

`sample_lvgl_camera` 是基于 **LVGL** 的相机界面示例程序，用于在**快速启动环境**下的 MIPI 屏上演示「实时预览 + 触摸交互 + 拍照 / 录制 + 相册回放（无音频）」的完整应用链路。

该示例有如下功能：

- **实时预览**：Camera Sensor 经 `VIN → ISP → VSE` 输出 NV12，经 DRM 视频 plane 全屏显示。
- **触摸 UI**：LVGL 绘制半透明控件，经 DRM ARGB overlay plane 叠在预览之上；触摸走 linux evdev（不依赖 LVGL 内置 `LV_USE_EVDEV`）。
- **拍照**：异步 JPEG 抓拍，带时间戳水印与快门反馈。
- **录像**：硬件 H.264 编码，封装为 MP4；相册列表可浏览 / 删除。
- **相册回放**：静图默认 JPEG 硬解后在 UI 层全屏显示；视频点开后由 FFmpeg 解封装，再经 VPU 硬解 H.264 到视频层播放。
- **状态显示**：照片余量、可用存储空间；空间不足时显示 `FULL` 并拒绝保存 / 可自动停录。

**注意**：本示例面向特定产品形态 （720x1280 竖屏 MIPI LCD + 720x1280 触摸板 + SC230AI）。

### 代码位置及目录结构

- 代码位置：`app/samples/platform_samples/sample_lvgl_camera`
- 目录结构：

```text
sample_lvgl_camera/
├── Makefile
├── README.md
├── setup_lvgl.sh                      # 下载 LVGL 9.5.0 → third_party/lvgl
├── config/
│   └── lv_conf.h                      # LVGL 编译期配置（色深、裁剪功能等）
├── src/
│   ├── main.c                         # 入口、参数、主循环
│   ├── camera/
│   │   ├── camera_pipeline.c/.h       # VIN→ISP→VSE 预览 + 抓拍/录像
│   │   ├── media_still.c/.h           # JPEG / 水印 / sidecar
│   │   ├── media_player.c/.h          # 相册 MP4 硬解播放
│   │   └── rec_mux.c/.h               # annex-B H.264 → MP4
│   ├── drm/
│   │   ├── drm_ui_plane.c/.h          # UI ARGB overlay
│   │   ├── vp_display.c/.h            # 视频 DRM plane（含 overlay）
│   │   └── uthash.h                   # vp_display 用
│   ├── lvgl_port/
│   │   ├── lvgl_display.c/.h          # LVGL → DRM ARGB flush
│   │   └── lvgl_input.c/.h            # linux evdev + 板级校准
│   ├── system/
│   │   └── sys_status.c/.h            # 轮询电量、温度、存储；UI 显示空间（FULL/xxMB/xxGB），有 capacity 时附带电量%
│   └── ui/
│       ├── ui_camera.c/.h             # 相机界面 + 快门反馈
│       └── ui_gallery.c/.h            # 相册
└── third_party/lvgl/                  # LVGL 源码（setup_lvgl.sh 后生成）
```

编译时还会链接同级目录：

- `platform_samples/utils/*.c`
- `platform_samples/vp_sensors/**`（Sensor 配置表）

## 硬件环境准备

- **SC230AI**模组，正确接到 MIPI CSI
- **MIPI LCD**（产品默认按 **720×1280** 竖屏调校）
- 触摸屏驱动已加载，存在 `/dev/input/event*` （产品使用的 gt9xx, 如加载驱动 `modprobe gt9xx_core`）
- DRM 显示通路可用（存在 `/dev/dri` 相关节点）

## 编译部署

### 编译

前置条件：

已生成 `out/deploy/system` 等依赖头文件与库。

编译方式：

- 方式一：

```bash
# 在 SDK 根目录，整体编译
./bd.sh

# 或在 SDK 根目录，编译 platform_samples（包含 sample_lvgl_camera）
./bd.sh app
```

- 方式二：

```bash
# 仅编译本目录
cd app/samples/platform_samples/sample_lvgl_camera
make
# 产物：./lvgl_camera
```

### 程序部署

完成编译并部署后，可执行文件默认安装到开发板：

- `/app/platform_samples/sample_lvgl_camera/lvgl_camera`

也可将本目录编译出的 `lvgl_camera` 手动拷贝到板端（如 `/userdata`），赋予执行权限：

```bash
chmod +x lvgl_camera
```

## 运行

### 程序运行方法

板端运行：

```bash
cd /app/platform_samples/sample_lvgl_camera
./lvgl_camera
```

### 界面操作说明

| 控件 | 功能 | 输出 |
|------|------|------|
| 快门（底栏中央大黑圆） | JPEG 抓拍（时间水印）+ 快门反馈 | `/userdata/lvgl_camera/capture_*.jpg` |
| REC （快门旁小红圆）| 开始 / 停止录像 | `/userdata/lvgl_camera/record_*.mp4` 及附属文件 |
| 相册 | 列表浏览、删除、静图查看、视频播放 | — |


录像同名附属文件：

| 文件 | 用途 |
|------|------|
| `.mp4` | 视频本体 |
| `.poster.jpg` | 录像首帧封面，列表 / 起播前垫图 |
| `.a565` | 列表短循环动图缩略（约前 3 秒、5 fps、160×90 RGB565） |
| `.vdur` | 时长（秒）文本，供列表 / 进度显示 |

相册删除媒体时会同步删除上述附属文件，`.poster.jpg` 不会作为独立相册条目显示；照片列表缩略图默认实时解码 JPEG。


### 运行效果

成功运行后：
- MIPI 屏双图层显示——底层实时预览，上层奶油色边框/底栏 UI，底栏含相册 / 拍照快门 / 录制 REC。
- 点击 Capture 生成 JPEG；点击 REC 开始录像，再次点击停止并生成 MP4。
- 进入相册可浏览最近媒体；点视频可硬解回放（无声音）。
- 串口可见类似日志：`Camera pipeline ready (1920x1080 -> MIPI plane 33)`、`sample_lvgl_camera running`、`lvgl_input: touch ready ...`。


正常退出：在串口发送 `Ctrl+C`，程序停止录像并释放 DRM / LVGL 资源。
