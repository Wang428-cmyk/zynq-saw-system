# 软件构建与验证记录

## 构建流程

本工程以 Vitis/XSCT 2021.2 从原生 PS 源码归档建立平台、BSP 和应用，生成应用 ELF、FSBL 与 BOOT.BIN。主入口为：

```text
python tools/build_ps.py --work-dir C:/saw_build
```

完整 PL→PS 入口为 `python tools/build_all.py --work-dir C:/saw_build`。输入、输出及工具路径均在构建记录中列出。

## 固定配置

处理器为 `ps7_cortexa9_0`，应用域采用 standalone 7.6、lwip211 1.6、xilffs 4.6。网络使用 RAW API 和 DHCP，控制台采用 UART1，存储使用 SD/eMMC FatFs。完整参数位于 `build/scripts/bsp_config.tcl`，生成配置为 `application_bsp.mss`。BSP 参考资料及来源在 `build/reference/`。

## 验证内容

1. 在包含中文和括号的提交路径启动构建，并在独立英文目录生成平台、BSP 与启动镜像。
2. 将工程打包并解压到另一目录，自动发现 Vitis 后完成跨目录重建。
3. 使用显式 XSA/bit 参数构建，验证硬件配对与输出发布。
4. 对应用执行 `arm-none-eabi-objcopy -O binary`，核对多轮可加载代码和数据。
5. 通过上位机 11 项自动化测试、会话审计 Skill 3 项自动化测试。
6. 在发布目录以相对路径 BIF 重新运行 Bootgen，核对生成的 BOOT.BIN。
7. 使用新生成的 PL XSA/bit 构建 PS，并核对两端硬件文件哈希。

多轮应用可加载代码/数据 SHA-256 为 `3313cfede6eddf2ab6fc60624ef8530e7aedd561d0bdd50f38b2e0aee9f7f74b`。日志和比较记录保存在 `sim/results/rebuild/`。

## 构建产物

`build/generated/ps/` 对应样机配对硬件，`build/generated/ps_from_pl/` 对应本轮 PL 导出硬件。各目录包含应用 ELF、FSBL、BOOT.BIN、BIF、BSP 配置、XSA/bit 和构建记录。原型固件在 `board/firmware/`。

项目组已完成样机上板测试，项目功能、操作演示及实验数据见 `report/design_report.docx` 与同名 PDF。
