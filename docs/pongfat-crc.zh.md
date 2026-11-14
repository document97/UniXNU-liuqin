# 改 boot 镜像必须重算 PONGFAT1 CRC32（liuqin 上机踩坑记录）

> 2026-11-14 深夜。这是**本轮最贵的一个坑**：所有「改了 boot 镜像却完全没反应」的现象，
> 包括「`pong_usb=1` 走不通」「内核入口没执行」「USB 直接消失」，一度全部归因错。
> 真因是固件在加载阶段就把 payload 静默丢了。任何接手的人先读这一节。

## 1. 现象

在 `liuqin-xnu-initial.img`（102,162,432 B）基础上改几个字节生成：

| 镜像 | 改动 |
|---|---|
| `liuqin-xnu-usbconsole.img` | 命令行 `pong_usb=0` → `pong_usb=1` |
| `liuqin-xnu-probe-blue.img` | 内核入口换成蓝带 + PSCI reset 探针 |
| `liuqin-xnu-probe-entry.img` / `probe-fixed.img` | 最小入口探针 |

上机表现完全一致：`fastboot boot` 返回 OKAY，屏幕可能有绿条，然后 USB 彻底断开，
`adb devices` / `fastboot devices` 全空，Windows 侧 `VID_18D1|VID_2717|VID_05C6` 也没有枚举。
当时误判为「内核入口没跑到」「USB 控制台不支持」。

## 2. 根因

`uefi-liuqin/Platforms/Nothing/pongPkg/Drivers/PongBootMemoryDxe/PongBootMemoryDxe.c`
的 `Publish()`：

```c
Fat = Header + PONG_FAT_HEADER_BYTES;          // PONG_FAT_HEADER_BYTES = 4096
Status = gBS->CalculateCrc32 ((VOID *)Fat, FatBytes, &Crc);
if (EFI_ERROR (Status) ||
    Crc != ReadUnaligned32 ((CONST UINT32 *)(Header + 24))) {
  continue;                                   // 静默丢弃整个 payload
}
```

`PONGFAT1` 头部（`PongInitrd.h`）布局：

| 偏移 | 类型 | 含义 |
|---|---|---|
| `+0` | 8 bytes | magic `PONGFAT1` |
| `+16` | u64 LE | `FatBytes`，CRC 覆盖区长度 |
| `+24` | u32 LE | 存放的 CRC32 |
| `+4096` | — | FAT 起点，也是 CRC 覆盖区起点 |

`kernel.macho`、boot args、ADT、initrd 全在 FAT 里，**改任何一个字节都让 CRC 失效**。
失败路径没有任何打印，所以从外部看和「内核跑飞了」一模一样。

`payload/pong-boot-manifest.json` 里 `kernel_sha384: null`、`loader_sha384: null`，
说明没有第二道签名/哈希校验要过 —— 唯一要修的就是这个 CRC32。

离线复核（对 5 个 v1 镜像算 CRC）：

```
liuqin-xnu-initial.img     stored=af2c52f0 calc=af2c52f0 OK
liuqin-xnu-usbconsole.img  stored=af2c52f0 calc=a982c8d6  CRC BROKEN
liuqin-xnu-probe-blue.img  stored=af2c52f0 calc=a1c49f04  CRC BROKEN
liuqin-xnu-probe-entry.img stored=af2c52f0 calc=e8d2587a  CRC BROKEN
liuqin-xnu-probe-fixed.img stored=af2c52f0 calc=137dcfad  CRC BROKEN
```

所有改过的镜像都还带着原版的 CRC，等于全被固件丢掉了。
**此前基于这些镜像得出的任何结论都作废，必须用 v2 重测。**

## 3. 修复与防呆

- `scripts/check-pongfat-crc.py`：校验任意镜像，支持 `--fix` 原地重算、`--json`、`--quiet`。
  `run-probe.ps1` 在 `fastboot boot` 之前调用它，CRC 不对直接拒绝启动。
- `builds/pkg-43581151943/patch-boot-image.py`：本地统一补丁工具（keystone 汇编入口探针、
  改 boot args、开 USB console），**最后一个动作永远是 `fix_crc()`**：

  ```python
  crc = zlib.crc32(memoryview(data)[fat_start:fat_start + size]) & 0xFFFFFFFF
  struct.pack_into("<I", data, head + 24, crc)
  ```

- 规则：**不要再用十六进制编辑器 / `dd` / 直接 `bytes.replace` 改 boot 镜像**。
  要么走 `patch-boot-image.py --out ...`，要么改完立刻跑
  `python scripts/check-pongfat-crc.py --fix IMAGE`。上机前再跑一次不带 `--fix` 的校验。

CRC 正确的 v2 镜像（`builds/pkg-43581151943/`）：

| 镜像 | 用途 | PONGFAT1 CRC |
|---|---|---|
| `orig-repacked.img` | 空操作对照，与 CI 小包内基镜像逐字节相同（sha256 `8aa38fe2…`） | `af2c52f0` |
| `probe-blue-v2.img` | 内核入口画蓝带 + 5 秒 PSCI SYSTEM_RESET | `a1c49f04` |
| `usbconsole-v2.img` | `pong_usb=1`，重测 USB CDC 控制台 | `a982c8d6` |

### 3.1 有两个同名 `liuqin-xnu-initial.img`，别拿错

| 路径 | 来源 | sha256 前缀 | PONGFAT1 CRC | 说明 |
|---|---|---|---|---|
| `D:\liuqin-port\liuqin-xnu-initial.img` | 11-12 run `43190178662` | `00f714b2…` | `6ce33453` | **旧构建**，占位卷 UUID，挂不上根卷 |
| `builds\pkg-43581151943\boot\UniXNU\out\...\boot\liuqin-xnu-initial.img` | 11-14 run `43581151943`（`liuqin-package.7z` 解出） | `8aa38fe2…` | `af2c52f0` | **当前构建**，22H374 + 真实卷 UUID `B07DF049-…`，与 19.39 GiB userdata 配套 |

`patch-boot-image.py` 的 `ORIGINAL` 指向第二个，三个 v2 镜像也都基于第二个。
所以 `orig-repacked.img` 与**第二个**逐字节相同，与根目录那个**不是同一个**（`cmp` 在 byte 9 就不同）。
以后所有上机测试一律用 v2 / 第二个基镜像；根目录那份只留作 11-12 的存档。
离线复核结果：

```
liuqin-xnu-initial.img     OK  stored=6ce33453 calc=6ce33453  fat=100663296
orig-repacked.img          OK  stored=af2c52f0 calc=af2c52f0  fat=100663296
probe-blue-v2.img          OK  stored=a1c49f04 calc=a1c49f04  fat=100663296
usbconsole-v2.img          OK  stored=a982c8d6 calc=a982c8d6  fat=100663296
```

四个 v1 补丁镜像（`usbconsole` / `probe-blue` / `probe-entry` / `probe-fixed`）
用 `check-pongfat-crc.py` 复核全部 `CRC BROKEN`，与上面的推算值逐个吻合。

## 4. 为什么入口探针现在可信

`DarwinBoot24` 的 `Enter()` 调用链在
`uefi-liuqin/Platforms/Nothing/DarwinBootPkg/Driver/Entry.S`：

```
// x0=boot_args PA, x1=entry PA, x2=owned PA, x3=populated bytes,
// x4=frame buffer PA, x5=its size
DarwinEnter: ... 画顶部绿条 ... 清 dc civac ... 关 SCTLR M/C/I ... br x1
```

`DarwinEnter` 全程只用 `x2..x11`，`x0` / `x1` / `x3` 不变。因此 `br x1` 时：

- `x0` 仍是 `boot_args` 的**物理地址**（MMU 已关，可直接 dereference）；
- `boot_args` 的 `Video[0]` 在 `+0x28`（framebuffer PA）、`Video[2]` 在 `+0x38`（rowbytes）、
  `Video[4]` 在 `+0x48`（height）、`CommandLine` 在 `+0x6c`
  （`BootArgs.h` 的 `STATIC_ASSERT` 固定了这些偏移）。

探针画在绿条下面一带（跳过前 1/16），再按 `cntpct_el0` 延时约 5 秒，
用 `PSCI SYSTEM_RESET`（`0x84000009`，`smc #0`）复位。
**「蓝带 + 5 秒后自动重启」是内核入口确实执行过的硬证据**，比肉眼看屏幕可靠。

内核入口坐标（本镜像，不是上游脚本里那一版）：

| 项 | 值 |
|---|---|
| `kernel.macho` LC_UNIXTHREAD pc | `0xfffffff007fe24a0` |
| Mach-O 文件偏移 | `0xFDE4A0` |
| boot image 中 `kernel.macho` 基址 | `0x234000` |
| boot image 中入口偏移 | `0x12124A0` |
| 首指令 | `140006ef` = `b +0x1BBC` |
| 入口后 padding | 727 个连续 NOP / 2908 B，可作 probe cave |

注意：仓库自带的 `UniXNU/scripts/patch-xnu-entry-probe.py` 用的 VA 是另一版内核
（`0xFFFFFFF00B9D0xxx`），**不要直接套在 22H374 这颗内核上**。

还有一条汇编教训：手写编码时曾把 `mrs x1, cntfrq_el0` 写成 `0xD53BE041`
（实际是 cntvct 类），延时循环永远等不到。正确编码：

```
mrs x1, cntfrq_el0 = 0xD53BE001
mrs x1, cntpct_el0 = 0xD53BE021
```

现在统一用 `keystone-engine` 汇编，不再手写机器码。

## 5. 上机测试顺序（CRC 正确版本）

1. 设备在 Android 且 `adb devices` 为 `device`（不是 `unauthorized`）。
2. `powershell -File builds\pkg-43581151943\run-probe.ps1 -Image probe-blue-v2.img`
   （脚本自己 `adb reboot bootloader` → `fastboot boot` → 监控 100 秒，无需按键组合）。
3. 判读：
   - **蓝带 + 约 5 秒后重启回 Android / fastboot** → 内核入口执行，`DarwinBoot` 交接没问题，
     下一步用 `usbconsole-v2.img` 抓内核日志。
   - **只有绿条、无蓝带、无重启** → 问题在 payload 加载 / 跳转地址 / cache 清洗 / ADT 内存窗口，
     **不要再怀疑 `pong_usb`**。
   - **绿条都没有** → 回到 UEFI 阶段一 `Mu-liuqin.img` 对照（`SHENG_VENDOR_HW=0` 应能出菜单）。
4. USB 控制台：另开一个终端跑
   `python scripts/capture-pong-usb-console-windows.py --wait 90 --seconds 25 --output logs\pong-console-1.bin --list-usb`。
   Linux/macOS 用原来的 `scripts/capture-pong-usb-console.py`。
   **优先级低于第 3 步**：`docs/USB-LOGGING-PLAN.zh.md` 明确写了 XNU 原生的 DWC3/CDC 实时日志驱动
   尚未实现（`drivers/` 里没有已接入的设备端模块），`pong_usb=1` 是否真能在这颗 22H374 内核上
   拉起 CDC 接口没有验证过。`usbconsole-v2.img` 只是把「CRC 损坏导致结论无效」这一条排除掉，
   **抓不到日志不能反推内核没跑**；判据以 entry probe 的蓝带 / PSCI 复位为准。
   抓日志前先跑 `--list --list-usb` 确认 Windows 是否真的枚举出了 COM 口（很多情况下只会看到
   `VID_18D1&PID_4EE7` 这种没有 CDC 接口的节点）。
5. 启动链确认后再 `fastboot flash userdata userdata.sparse.img` 写回 19.39 GiB 镜像
   —— recovery 里「清除数据」已经把 `/data` 格式化掉了，之前写的 SHNGROOT 不在了。

## 5.1 两轮实测（2026-11-14 09:00–09:08，CRC 正确版本）

`run-probe.ps1` 全自动完成：等 adb → `adb reboot bootloader` → `fastboot boot` → 监控，**全程无需按键组合**。

| 镜像 | probe 延时 | BOOT_SENT | adb 回到 `device` | 从 BOOT_SENT 起 |
|---|---|---|---|---|
| `probe-blue-v2.img` | 5 s | 09:01:03 | 09:01:37 | 34 s |
| `probe-blue-12s.img` | 12 s | 09:06:59 | 09:07:41 | 42 s |

两轮都在 `+5s` 看到 `fastboot=[???????????? fastboot]`（复位瞬间 bootloader 的临时枚举），
`+8s` USB 全断，随后 Android 冷启动完成、adb 以 `device`（不是 `unauthorized`）自动回来。
**延时从 5 s 调到 12 s，重启时刻后移 8 s（预期 7 s，差 1 s 是开机时间波动）** ——
复位确实由写进内核入口 cave 的 PSCI 代码触发，不是 watchdog，也不是偶发重启。
两个镜像只差延时段：`patch-boot-image.py --delay-seconds` 用 `cntfrq_el0` 算 tick，
已用 keystone 逐条核对（`mrs x4,cntfrq_el0 = d53be004`、`movz x5,#12 = d2800185`）。

**结论：`ABL → BootShim+UEFI FD → DarwinBoot → ExitBootServices → DarwinEnter(br x1) → 内核入口`
整条交接链是通的。** 卡点在内核入口**之后**（MMU 开启 / ADT 解析 / 内存映射 / 早期初始化），
不再是 payload 加载、跳转地址或 cache 清洗。上面 §5 的「只有绿条」分支已被实测排除。

### 计时探针：不依赖任何日志通道的定位方法

设备当前没有 root、`/sys/fs/pstore` 不可读、XNU CDC 控制台未实现、framebuffer 文字要人拍照。
但**「在代码某处放一个不同延时的 PSCI reset，看重启时刻」**是全自动且可量化的：
每次只在一个锚点放探针，若该锚点被执行就会在对应时刻复位并自动回 Android，
设备永远不会卡死、永远不需要长按电源。改的字节由 `patch-boot-image.py` 统一重算 CRC。

**已备好 stage-2 镜像 `probe-mmu-v1.img`**（CRC `abaa967c`，sha256 `b0a2d8b3…`），下一轮直接跑：

- 全内核只有两处 `msr sctlr_el1, x0`（`d5181000`）：Mach-O `0xFDE424`（入口之前）与
  `0xFE0358`（入口之后）。后者即 `_start_first_cpu+0x2fc` 的 MMU 使能点，
  boot image 偏移 `0x234000 + 0xFE0358 = 0x1214358`。
- 入口后有 **727 个连续 NOP / 2908 B**（Mach-O `0xFDE4A4..0xFDF000`、
  image `0x12124A4..0x1213000`），cave 取 `image+0x12124E0`。
- `patch-boot-image.py --hook-offset 0xFE0358 --delay-seconds 20` 把 MMU 点那条指令换成
  `b 0x17fff862`（delta `-7800 B / -1950` 条指令，在 ±128 MB 内），跳进 cave 跑
  **纯延时 + PSCI reset**（不画带：到这一步 `x0` 已不是 boot_args，别再依赖 framebuffer）。
- 静态复核：入口仍是 `140006ef`（内核正常执行到 hook）、hook 处 `d5181000 → 17fff862`、
  cave 首字 `d53be001` = `mrs x1,cntfrq_el0`、`d2800282` = `movz x2,#20`、末尾 `d4000003` = `smc #0`，
  全镜像只改了 18 个 word，CRC 正确。

判读：**约 20 s 后自动复位** = 内核跑到了 MMU 开启前；**不复位 / 黑屏** = 卡在
`_start_first_cpu` 更早的位置（此时需要长按电源约 10 s 回 Android，再换更靠前的锚点）。
后续可把 hook 逐步前移/后移，用不同延时当「阶段指纹」二分定位，全程只靠重启时刻，
不需要 root、pstore、CDC 或屏幕拍照。

## 5.2 「重启后默认仅充电」不影响 adb

用户观察到重启后 USB 模式回到「仅充电」。实测属性：

```
sys.usb.config           adb
persist.sys.usb.config   adb      ← 持久属性，重启不丢
init.svc.adbd            running
adb_enabled              1
usb_charging             true     ← 「仅充电」只关掉 MTP
```

adb 是**独立的 USB function**，由 `persist.sys.usb.config=adb` 固定，与 MTP 是否开启无关。
两轮 PSCI 复位后 adb 都自动以 `device` 回来，没动过 USB 模式。**保持「仅充电」即可**，
切成「传输文件」反而变成 `mtp,adb`、多一个接口、增加枚举波动。

真正会断 adb 的三种情况（都不是 USB 模式问题）：
1. 清数据 / 恢复出厂 → `/data/misc/adb/adb_keys` 被删 → `unauthorized`，需在平板上点「允许 USB 调试」；
2. 开发者选项被重置 → `adb_enabled=0`；
3. 设备压根没进 Android（黑屏 / UEFI / DarwinBoot）→ 与 USB 模式无关。

`run-probe.ps1` 已加自动识别：等待时区分 `unauthorized` / 「USB 在但无 adb」/ 全断，
并在 Android 就绪时打印 `persist.sys.usb.config` 与 `adb_enabled` 做防呆。

### USB CDC 抓取的实测边界

11-14 09:06 那轮同时守候了 `capture-pong-usb-console-windows.py --wait 130`：
**没有任何 Pong CDC COM 口出现**，`--list-usb` 只看到 `VID_18D1&PID_4EE7`。
这符合预期 —— 该轮镜像是 `pong_usb=0`（`probe-blue-12s`），且 `docs/USB-LOGGING-PLAN.zh.md`
写明 XNU 原生 DWC3/CDC 日志驱动尚未实现。下一步要单独用 `usbconsole-v2.img` 验证
`pong_usb=1` 是否真能拉起 CDC interface；注意那个镜像没有 PSCI reset，可能卡住需要一次长按电源。

## 6. 其它同时被推翻的判断

- `pong_usb=1` 「USB 控制台不支持」：当时镜像 CRC 是坏的，结论无效，需重测。
- 「内核入口没执行」：同上。
- `pstore` / `/proc` 读日志：设备 shell 权限不足（`/sys/fs/pstore` Permission denied），
  这条路线在没有 root 时不用再试；framebuffer 色带 + PSCI reset + USB CDC 是当前可用的三条通道。
- 「6 GB 版导致 `dram_size=0x200000000` 错配」：`adb` 读到 `MemTotal: 7722084 kB`，确认是 8 GB 版，排除。
