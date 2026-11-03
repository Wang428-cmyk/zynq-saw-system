# PL 与 PS 完整构建流程

## 工具与入口

环境为 Vivado/Vitis 2021.2、XC7Z035 综合与实现许可、Python 3.10+。在工程根目录运行：

```text
python tools/build_all.py --work-dir C:/saw_build
```

完整入口依次调用 PL 和 PS 构建器。原生 Vivado ZIP 被还原到独立英文目录，随后完成 IP/BD 输出生成、综合、实现、bitstream 和 XSA 导出；PS 构建使用本轮导出的配对硬件描述。

## 分步构建

```text
python tools/build_pl.py --work-dir C:/saw_build
python tools/build_ps.py --work-dir C:/saw_build --xsa build/generated/pl/top_module.xsa --bit build/generated/pl/top_module.bit --output build/generated/ps_from_pl
```

可用 `--vivado-home` 或 `--vitis-home` 指定安装目录。每次调用创建新暂存目录并发布日志、输出文件及 SHA-256 构建记录。

## PL 工程生成

PL 以 `src/pl/vivado_project.zip` 为输入，顶层为 `Top_Module`，器件为 `xc7z035ffg676-2`。脚本显式打开并验证顶层 `System.bd`，传播接口参数，重新生成 BD 和 SmartConnect 输出，并以 Vivado 2021 流程建立综合与实现运行。暂存工程采用全局 BD 综合。原生 ZIP、RTL 与 XDC 保持原有内容。

输出目录为 `build/generated/pl/`，包含 `top_module.xsa`、`top_module.bit`、综合/实现资源报告、DRC 报告及完整工具日志。

## PS 平台与启动镜像

XSCT 创建 Cortex-A9 平台及 standalone 应用域，应用固定 BSP 配置，导入原生 C 源码和链接脚本，编译应用与 FSBL，并调用 Bootgen 生成启动镜像。XSA 内嵌 bitstream 与外部 bitstream 先进行哈希配对检查。

输出目录为 `build/generated/ps_from_pl/`，包含应用 ELF、FSBL、BOOT.BIN、BIF、BSP 配置、硬件描述及构建记录。直接运行 `build_ps.py` 使用 `board/firmware/` 中的样机配对硬件描述，输出在 `build/generated/ps/`。

## 构建记录

完整工程已完成从原生源码到 PL 硬件导出及配对 PS 启动镜像的构建。`build_receipt.json` 记录工具、输入和输出哈希；`sim/results/rebuild/` 汇总多轮构建、跨目录重建、应用可加载代码核对和相对路径 BIF 检查结果。项目组已完成样机上板测试，实验及功能说明见当前技术报告。
