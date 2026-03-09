# sample_qt 目录使用说明

本文档用于说明 `sample_qt` 示例程序的目录结构、编译方式、运行方法及核心配置逻辑，适用于 X5 系统 Qt5 环境验证场景。

## 1. 目录结构与文件说明

| 文件名                | 功能描述                                                                 |
|-----------------------|--------------------------------------------------------------------------|
| main.cpp              | Qt 示例程序核心源码：实现基础 Qt Widgets 界面（窗口、按钮等），用于验证 Qt5 在 X5 系统上的运行环境（窗口创建、事件循环、图形显示等）。 |
| run.sh                | 板端运行脚本：自动设置 Qt 运行所需环境变量（如下），并启动示例程序<br>- `QT_QPA_PLATFORM=eglfs`（指定 EGLFS 平台）<br>- `QT_QPA_EGLFS_KMS_CONFIG=$(pwd)/eglfs_kms_config.json`（绑定 KMS 配置文件）<br>- `QT_QPA_EGLFS_HIDECURSOR=0`（显示鼠标光标）<br>最终通过 `exec ./sample_qt "$@"` 启动程序。 |
| eglfs_kms_config.json | EGLFS KMS 配置文件：指定 DRM 设备、输出端口、分辨率等参数<br>默认配置：关闭 DSI、启用 HDMI1、分辨率 1920x1080，将 HDMI1 设为 primary 输出。 |
| Makefile.bak          | 交叉编译备份文件：控制 sample_qt 是否参与 BSP 构建（需重命名为 Makefile 启用），核心特性：<br>- 依赖上层 `Makefile.in` 提供的工具链和环境变量<br>- 使用 `$(HR_BUILD_OUTPUT_DIR)/deploy/system` 中的 Qt 头文件和库<br>- 链接 Vivante GPU 栈（libGLESv2、libGAL、libVSC），运行时从 `/usr/hobot/lib` 加载<br>- 支持 `make` 编译（生成 sample_qt 及目标文件）、`make install` 部署（安装到 out/deploy/app/platform_samples/sample_qt/）<br>- 支持本地单独编译或顶层脚本间接调用。 |

## 2. 核心配置说明：为何 Makefile 命名为 Makefile.bak？

为实现 `sample_qt` 示例的「按需启用」，避免无 Qt5 依赖时冗余编译，采用以下设计：

### 2.1 默认场景（x5_system_defconfig）
- 系统仅需基础样例，不强制依赖 Qt5；
- 顶层 `app/samples/platform_samples/Makefile` 会自动扫描子目录的 `Makefile`，因本目录默认是 `Makefile.bak`，会跳过 sample_qt 编译，减少构建时间。

### 2.2 Qt5 启用场景（x5_system_qt5_defconfig）
- 该配置明确启用 Qt5 能力，需同步编译 sample_qt 示例；
- 启用方式：将 `Makefile.bak` 重命名为 `Makefile`，即可让顶层构建脚本识别并纳入编译流程。

**核心目的**：仅在 X5 系统启用 Qt5 配置时，才构建 sample_qt，确保其他场景下构建环境干净、高效。

## 3. 编译方式

### 3.1 编译前置条件
1. 需要重新制作根文件系统，详细的编译步骤请参考芯片手册(根文件系统适配指南相关章节)，编译根文件系统的配置应该是 `system/buildroot/source/configs/x5_system_qt5_defconfig`（而非默认的 `system/buildroot/source/configs/x5_system_defconfig`），确保根文件系统包含 Qt5 依赖；
2. 已将 `sample_qt` 目录下的 `Makefile.bak` 重命名为 `Makefile`（启用编译开关）。

### 3.2 两种编译方式

```bash
# 编译方式一
# 进入工程根目录
cd <X5_BSP_ROOT>
# 执行顶层构建脚本，整体编译
./bd.sh
# 或者执行顶层构建脚本，编译 platform_samples（包含 sample_qt）
./bd.sh app samples/platform_samples

# 编译方式二
# 进入 sample_qt 目录
cd app/samples/platform_samples/sample_qt
# 执行编译（依赖上层 Makefile.in 提供的交叉工具链与 Qt5 环境）
make
```
