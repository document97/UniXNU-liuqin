# liuqin userdata 真实 extent（2026-11-14 量得）

结论：**`first_lba = 0x2A1600`（2,758,144），`last_lba = 0x1D143FA`（30,491,642），4 KiB 扇区，共 27,733,499 扇区 = 113,596,411,904 B = 105.795 GiB。**

已写进 `board.toml [userdata]`，并附带 `device = "/dev/block/sda35"`。这份文档是那两个值的推导依据，也给 `porting.toml` 的 storage area 做证据。

## 来源：同版本原厂线刷包，不是设备

设备侧读不到：本机所有 `/sys/class/block/*/start`、`/proc/partitions`、`/proc/cmdline` 对 shell 全部 `Permission denied`（见 `../device-info/adb-2026-11-12.txt` 那次采集）。但同版本线刷包里带着 GPT。

需要自备 **`liuqin_images_V14.0.9.0.TMYCNXM`**（小米平板 6 Pro 的 V14.0.9.0.TMYCNXM 官方线刷包，解开后约 7.2 GB），用到其中这几个文件：

```
images/gpt_both0.bin        45056 B   LUN 0（含主 GPT + 备份 GPT）
images/gpt_main0.bin        24576 B   LUN 0 的主 GPT，单独一份
images/gpt_both1..5.bin     45056 B   其余 5 个 LUN
```

这 6 个文件是 UFS 各 LUN 的 GPT，分区数 35 + 5 + 5 + 5 + 76 + 6 = **132**，与解锁后 `fastboot getvar all` 报的 132 个 `partition-size` 完全对应（变量表见 `../device-info/fastboot-getvar-2026-11-14.txt`，序列号已脱敏）。

## 为什么 LUN 0 就是 sda

- LUN 0 表有 35 个分区，最后一项（entry 34）正是 `userdata`，即第 35 号 → `/dev/block/by-name/userdata -> /dev/block/sda35`。
- LUN 4 表（76 个分区）的第 13 项是 `boot_a`（第 14 号）、第 24 项是 `vendor_boot_a`（第 25 号），对应 2026-11-12 adb 会话记录的 `boot_a = sde14`、`vendor_boot_a = sde25`。Android 按 LUN 顺序命名为 sda/sdb/…/sde，所以 **sda = LUN 0**，sde = LUN 4。

## 为什么末端要借 fastboot 补

LUN 0 表里 userdata 记成 `first_lba = 2758144`、`last_lba = 2758143`（= first − 1），这是**生长到分区末尾**的标记；同时该表头的 `alternate_lba` 只有 2758148，对应 10.5 GiB 的"模板盘"，不是本机实际容量（同一份 GPT 要供 128/256/512 GB 多个 SKU）。所以末端用解锁后实测的 `partition-size:userdata = 0x1A72DFB000` 补：

```
first_lba = 0x2A1600                        = 2,758,144   (1 MiB 对齐)
sectors   = 0x1A72DFB                       = 27,733,499
last_lba  = 0x2A1600 + 0x1A72DFB - 1 = 0x1D143FA = 30,491,642
```

## 四项互证

1. 该表 userdata 之上的 **34 个分区**，由 `first/last` 推出的尺寸与 `fastboot getvar all` 的 `partition-size` **逐字节相同**（解析脚本逐项比对，全部 `ok`）。
2. `gpt_main0.bin`（主 GPT 单独一份）给出同样的 `first_lba = 2758144`。
3. `gpt_both4.bin` 的 `boot_a` 大小 201,326,592 B，与原厂 `images/boot.img` 一致；条目号 13/24 与 `sde14`/`sde25` 吻合。
4. 反推 LUN 0 总量 = (last_lba + 1 + 34) × 4096 = **124.89 GB**，正是 128 GB 机型的可用容量；userdata 的 105.795 GiB 与 `df -k /data` 的 110,930,924 KiB（105.8 GiB）只差 3 MiB（f2fs 元数据）。

## 复核方式

```bash
python tools/parse-gpt.py <线刷包>/images/gpt_both0.bin \
  --fastboot device-info/fastboot-getvar-2026-11-14.txt --name userdata
```

脚本按 CRC32 自校验，能自动识别 512/4096 扇区与"仅表头"的文件。

## 教训

先前用「1 MiB 对齐 + 132 个分区顺序累加」算出的 `0x91400` 是错的：132 个分区分布在 6 个 LUN 上，`sda` 只承载其中 35 个。分 LUN 归档之前，任何累加推算都不成立。
