# UniXNU-liuqin

**简体中文** · 小米平板 6 Pro（代号 `liuqin`，高通 SM8475 / 骁龙 8+ Gen1）的 UniXNU 设备仓库。

本仓库从 [UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong)（Nothing Phone (2)，同为 SM8475）派生：
SoC 层的实现（CPU、GIC、UFS、PMIC 按键、Adreno 730、QCA6490、Darwin24 内核补丁和打包流程）沿用 pong，
只替换 liuqin 自己的板级数据。名字里带 `pong_` 的组件、脚本和内核参数指的是这套共享的 SM8475 实现，
不是 Nothing 的机型专有代码。pong 原来的说明保留在 [README.pong.md](README.pong.md)。

配套固件仓库：`uefi-liuqin`（uefi-pong 的 `liuqin` 分支，新增 `Platforms/Xiaomi/liuqinPkg`）。

> **状态：没有在真机上验证过任何一项。** 这里的内容是源码级移植和主机侧测试，
> 每一项的依据和待验证点都写在 [porting.toml](porting.toml) 里。

## 和 pong 不同的地方

| 项 | liuqin | 文件 |
| --- | --- | --- |
| 内存 | 8 GB 版，ABL 实测 5 段 DRAM；保留区取主线 DTS、原厂 uefiplat.cfg 和 pong 同 BSP 抓取的并集 | `configs/liuqin-memory-sm8475.json`，`scripts/gen-liuqin-memory.py` |
| 设备树 | `dts/liuqin.dts`（内存、保留区、触控、按键；UFS/PCIe0 与 pong 相同） | `dts/liuqin.dts` |
| 屏幕 | 1800×2880，双 DSI LCD，ABL 持续点亮；第一版走 GOP 帧缓冲、2 倍缩放 | `configs/liuqin-board.json` |
| 背光 | KTZ8866（i2c1 0x11），暂无驱动，沿用 ABL 的亮度 | 同上 |
| 触控 | Novatek NT36532，和 pong 同一条 SPI（spi@990000、IRQ gpio21），暂无驱动 | 同上 |
| 身份 | 直接用 IPSW 本身的 iPad7,11 / J171AP，iPad 界面 | 同上 |
| 内核配置 | 第一版只开启动必需的部分：ramoops 日志、电源/音频、GPU、Wi-Fi 先关 | `configs/liuqin_ipados18_defconfig` |
| UEFI | liuqinPkg：FD 0xA7100000/3 MiB、SimpleFb GOP、厂商模块可选 pong 或 liuqin 原厂 | uefi-liuqin |
| 启动 DTB | 原厂 vendor_boot 里的 “Cape SoC” 基础 DTB（ABL 按 msm-id/board-id 选择） | `tools/fetch-liuqin-stock.py` |

主线 DTS（GPL-2.0）不随仓库提交；重新生成内存表时用
`python3 scripts/gen-liuqin-memory.py --fetch`，它会把固定版本 fe81794b 的三个文件下载到被忽略的 `reference/mainline/`。

`scripts/make-pong-adt-ipados18.py` 新增 `--board`（或环境变量 `UNIXNU_SM8475_BOARD`），
从 `configs/<board>-board.json` 读取板级数据；不传时仍是 pong，输出不变。

## 分阶段计划

### 阶段 1：UEFI 和启动菜单（只用 `fastboot boot`，不写任何分区）

1. 准备一个 liuqin 的**完整 OTA 包**（小米 recovery 卡刷包，里面有 `payload.bin`）的下载链接或本地文件。
2. 在你 fork 的 `uefi-liuqin` 上运行 Actions → **liuqin UEFI**，填入 `ota_url`。
   `binaries` 先选 `pong`（公开的 SM8475 模块）；不行再选 `liuqin`（从 OTA 的 uefi 分区导入原厂模块）。
3. 下载产物里的 `Mu-liuqin.img`，平板进 fastboot 后：

   ```sh
   fastboot boot Mu-liuqin.img
   ```

4. **通过标准**：屏幕从小米 logo 切到 UniXNU 菜单（没有 payload 时菜单里没有可启动项）。
   出问题直接长按电源键重启，回到原系统。

本地也可以只提取原厂输入：

```sh
python3 tools/fetch-liuqin-stock.py --package <OTA zip 路径或 https 链接> --out ../liuqin-stock
```

### 阶段 2：内核到桌面

沿用 pong 的 `package.yml`（macOS runner）：内核、payload 和带 `PONGFAT1` 封装的启动镜像。
还需要做的：

- `scripts/finalize-pong-adt.py` 和 `package.yml` 改用 liuqin 的路径、8 GiB 内存、`--board liuqin` 和 Cape DTB；
- `board.toml` 的 `[userdata]` 换成**你这台平板** GPT 里 userdata 分区的起止 LBA（现在还是 pong 的值，安装脚本会比对 GPT，不一致就停下）；
- 仓库 secret `UNIXNU_UNIQUE_CHIP_ID`（8 位十六进制，只用于生成软件身份）。

**通过标准**：先能进 launchd，再到 SpringBoard（软件渲染，暂时没有触控）。

### 阶段 3：日常功能（向 6S Pro / sheng 看齐）

按顺序：NT36532 触控 → KTZ8866 背光 → GPU（Adreno 730，需要 liuqin 自己签名的 ZAP 固件）→
Wi-Fi（QCA6490，需要 liuqin 的板级数据）→ 音频（4 颗 CS35L41）→ 电池与充电。

## 风险提示

- 全程只用 `fastboot boot` 在内存里启动，不写 boot 分区；只有阶段 2 安装系统卷时才会写 **userdata**，会清空平板数据。
- 本仓库不包含 Apple、小米或高通的二进制。CI 产物里带有从 OTA 提取的厂商文件，下载后请删除对应的 run。
- 在非 Apple 硬件上运行 iPadOS 违反 Apple 软件许可协议，仅用于研究。

## 致谢

[UniXNU](https://github.com/UniXNU/UniXNU)、[UniXNU-pong](https://github.com/Chengdong-Yu/UniXNU-pong) / uefi-pong、
[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng)、[Project Silicium](https://github.com/Project-Silicium/Mu-Silicium)，
以及 liuqin 的前期资料：[1696108518/UniXNU-liuqin](https://github.com/1696108518/UniXNU-liuqin)、
[1696108518/iPadOS-liuqin](https://github.com/1696108518/iPadOS-liuqin)、
[yzddmr6/xiaomipad-6pro-mainline](https://github.com/yzddmr6/xiaomipad-6pro-mainline)。
