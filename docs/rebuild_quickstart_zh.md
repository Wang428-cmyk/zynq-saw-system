# 工程构建与运行说明

本工程采用 Zynq-7000 与 AD9361 实现无线无源 SAW 谐振频率读出。PL 完成激励、采样和回波能量处理，PS 完成射频配置、粗扫、选峰与精扫，上位机和 TFT 显示测量结果。项目组已完成样机上板测试，设计与实验内容见 `report/design_report.docx` 和同名 PDF。

## 开发环境

使用 Vivado/Vitis 2021.2、XC7Z035 对应综合与实现许可、Python 3.10+。构建暂存路径采用短的英文路径，例如 `C:/saw_build`；提交目录可以包含中文字符。

## 完整构建

在 `zynq-saw-system` 根目录打开终端并运行：

```text
python tools/verify_package.py
python tools/build_all.py --work-dir C:/saw_build
```

脚本先还原原生 PL 工程，完成综合、实现和硬件导出；再使用本轮生成的配对 XSA/bit 创建 PS 平台、BSP 与应用，编译 FSBL 并生成 BOOT.BIN。每轮使用独立工作区。

| 输出目录 | 文件内容 |
| --- | --- |
| `build/generated/pl/` | 新生成的 XSA、bit、实现报告和构建记录 |
| `build/generated/ps_from_pl/` | 基于新硬件的应用、FSBL、BOOT.BIN、BSP 和日志 |
| `build/generated/ps/` | 基于样机配对硬件描述重建的 PS 固件 |
| `board/firmware/` | 项目样机原有固件及配对硬件描述 |

## 单独构建 PS

```text
python tools/build_ps.py --work-dir C:/saw_build
```

默认硬件输入为 `board/firmware/top_module.xsa` 和 `top_module.bit`。也可以同时指定新的配对硬件：

```text
python tools/build_ps.py --work-dir C:/saw_build --xsa build/generated/pl/top_module.xsa --bit build/generated/pl/top_module.bit --output build/generated/ps_from_pl
```

应用 BSP 配置位于 `build/scripts/bsp_config.tcl`，采用 standalone 7.6、lwip211 1.6、xilffs 4.6、UART1、RAW API、DHCP 和 SD/eMMC。构建入口支持 `--vitis-home`、`--vivado-home` 指定安装目录。

## 启动与显示

将所选启动镜像以 `BOOT.BIN` 文件名放入启动介质，设置开发板 SD 启动模式，连接电源、射频链路和以太网。电脑运行：

```text
python src/upper_computer/server.py --open
```

浏览器地址为 `http://127.0.0.1:8080`。设备默认控制地址为 `192.168.2.10:4001`，结果通知端口为 `4002`。网页提供参数设置、粗扫、精扫、峰值显示和连续采集记录；TFT 显示本地频率与幅度。详细接口见 `docs/communication.md`。
