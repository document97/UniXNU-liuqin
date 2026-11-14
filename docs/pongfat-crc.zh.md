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

## 6. 其它同时被推翻的判断

- `pong_usb=1` 「USB 控制台不支持」：当时镜像 CRC 是坏的，结论无效，需重测。
- 「内核入口没执行」：同上。
- `pstore` / `/proc` 读日志：设备 shell 权限不足（`/sys/fs/pstore` Permission denied），
  这条路线在没有 root 时不用再试；framebuffer 色带 + PSCI reset + USB CDC 是当前可用的三条通道。
- 「6 GB 版导致 `dram_size=0x200000000` 错配」：`adb` 读到 `MemTotal: 7722084 kB`，确认是 8 GB 版，排除。
