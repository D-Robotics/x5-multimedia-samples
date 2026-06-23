# single_pipe_vin_isp_vse_quickstart_display

## 注意事项

**本示例仅适用于 SC230AI 传感器模组。**

该程序在启动时为 SC230AI 快速出图链路设置了默认环境（例如 `CAM_QUICKSTART_SC230AI_DELAY_SCALE`、`VP_QUICKSTART_VSE_BUF_NUM` 等），其它型号传感器未在此示例中验证。**请勿将本程序用于 SC230AI 以外的传感器**；若需其它传感器，请使用通用 `single_pipe_vin_isp_vse` 等示例并自行适配参数。

---

## 功能说明

该示例用于验证单路视频采集与显示功能：**VIN → ISP → VSE**，将 NV12 帧送到 **MIPI LCD**（`VP_DISPLAY_OUTPUT_MIPI`）。

相对常规 pipeline 示例，本程序侧重缩短首帧时间，有如下优化:

- 并行执行 `hbmem_open_worker`/`display_init_worker`
- 添加 sensor 传感器信息缓存（`/userdata/vp_sensor_quickstart.cache` 或 `/tmp/vp_sensor_quickstart.cache`）
- 添加运行过程中各阶段耗时（`CLOCK_MONOTONIC`）打印

---

## 编译

在已经配置 SDK 输出目录的前提下，于本目录执行：

```bash
export HR_BUILD_OUTPUT_DIR=<你的_SDK_out_路径>   # 若未在 Makefile.in 中写死
cd single_pipe_vin_isp_vse_quickstart_display
make
```

成功后在当前目录生成可执行文件 `single_pipe_vin_isp_vse_quickstart_display`。

---

## 运行

**请先确认硬件为 SC230AI 模组。**

```bash
./single_pipe_vin_isp_vse_quickstart_display -s <sensor_index> [选项]
```

- `-s <sensor_index>`：传感器在列表中的索引（需对应 **SC230AI** 配置项）。
- `-h`：打印帮助并列出当前构建中的传感器列表。
- `-c <channel_type>`：通道拓扑，默认 `vf:if`（与主 sample 一致）。支持 `vo` / `vf` / `io` / `if` 及组合，详见程序内 `print_help`。

可选环境变量：

| 变量 | 含义 |
|------|----------------|
| `VP_QUICKSTART_USE_SENSOR_CACHE` | 是否使用传感器信息缓存，默认开启（`0`/`false`/`no` 关闭） |
| `VP_QUICKSTART_VSE_BUF_NUM` | VSE 输出缓冲个数，默认 `2`，范围 2–6 |
| `CAM_QUICKSTART_SC230AI_DELAY_SCALE` | **SC230AI** 上电时序延时设置（ms），程序默认设为 `10`（可被运行前环境覆盖） |

---
