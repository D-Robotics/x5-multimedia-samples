
# 概述

## 框图
USB 传输模式
 ┌────────────┐                                 ┌───────────┐
 │            │                                 │           │
 │            │  [2Cam-NV12] + [Depth] + [IMU]  │           │
 │   X5 EVB   ├─────────────────────────────────┤  X5 EVB   │
 │   Server   │USB3.0                     USB3.0│  Client   │
 │            │Device                     Host  │           │
 │            │                                 │           │
 └────────────┘                                 └───────────┘

MIPI-CSI 传输模式
  ┌────────────┐                             ┌───────────┐
  │            │                             │           │
  │            │  [2Cam-NV12] + [Depth]      │           │
  │   X5 EVB   ├─────────────────────────────┤  X5 EVB   │
  │   Server   │MIPI-CSI             MIPI-CSI│  Client   │
  │            │ TX                     RX   │           │
  │            │                             │           │
  └────────────┘                             └───────────┘


## 功能描述
`multi_pipe_stereo_infer` 实现了接收和发送 Camera 拼接数据示例，包含 `server` 和 `client` 目录
- `server` 目录: 实现了深度推理，并将 2路Camera NV12数据 + 深度数据拼接发送（基于 socket 和 MIPI-CSI TX）
- `client` 目录: 实现了接收拼接数据（基于socket），解包后分别得到2路Camera N12数据 和 拼接数据，并将拼接数据转换为点云


## 注意事项
### 事项1: 程序运行平台
- `server` 程序仅运行在 X5 平台
- `client` 程序可运行在多个平台上，包括 X5、X86 等，根据运行平台指定其交叉编译链

### 事项2: 传输数据类型
`server` 支持两种传输模式，即 `USB模式` 和 `MIPI-CSI模式`，二者拼接数据不同
- `USB模式`: 支持 2路 NV12 数据 + 深度数据 + IMU数据
- `MIPI-CSI模式`: 支持 2路 NV12 数据 + 深度数据

### 事项3: 标定参数
`server` 支持通过修改 `multi_pipe_stereo_infer` 参数 `-g` 值来指定标定参数的保存方式，`-g`参数值如下
- `-g 0`: 标定参数保存在 EEPROM，量产保存方式
- `-g 1`: [默认方式]标定参数保存在 YAML 文件，调试阶段使用，默认标定文件路径`./config/x5/SC132gs_dual_calibration.yaml`

### 事项4: 畸变矫正
本示例仅支持 `SC132GS` 双目模组，开启畸变矫正后图像旋转90度，需注意原图分辨率将发生改变，1088x1280 -> 1280x1088

### 事项5: 传输带宽
由于传输数据量较大，`USB模式` 需使用 USB 3.0 作为传输接口。
在运行程序之前，可先测试带宽是否满足数据量需求，避免由于带宽不足导致的丢帧或报错。
网络带宽测试命令如下
Server端运行
```
ifconfig usb0 mtu 5000
iperf3 -s
```

Client端运行
```
ifconfig usb0 mtu 5000
iperf3 -c 192.168.5.5
```


# 编译
## Server 编译
Server 源码可以分为如下两个部分：
	- multi_pipe_stereo_infer 示例代码
	- hobot_stereonet 算法接口

其中：hobot_stereonet 需另外下载编译，hobot_stereonet 详细编译方法参考 `hobot_stereonet/DStereo_X5/README.md`

下载和编译方法参考如下：
步骤1：到 server 目录并下载 hobot_stereonet 仓库
```
cd server

git clone https://github.com/D-Robotics/hobot_stereonet.git
git checkout api_0.0.1
```

步骤2：编译 hobot_stereonet (仅需编译一次)
```
cd hobot_stereonet/DStereo_X5
bash ./run_build.sh
```

步骤3：编译 multi_pipe_stereo_infer 示例程序
```
cd -
make
```

## Client 编译
```
cd client
make
```
### 编译X86版本 client (如果接收端平台非 X86 则跳过此步骤)
```
make BOARD=x86
```

# 部署
将 `app/samples/platform_samples/sample_pipeline/multi_pipe_stereo_infer` 目录分别复制到发送和接收板端 `/userdata`

# 运行
## 本地模型推理运行

```
cd /app/platform_samples/sample_pipeline/multi_pipe_stereo_infer/server

./run_server.sh local

```

