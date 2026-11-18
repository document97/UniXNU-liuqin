// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8475/Pong frontend. Optional register/registry probes default off;
// the functional transport only issues read-only storage commands.
//
// Bring-up runs as a fixed sequence of stages, each owning one question and
// reporting through the same register snapshot:
//
//   validateDescription   does the board description match this SoC?
//   bringUpClocks         are the UFS clocks running, and can we fix them?
//   UfsHost::sample       is the controller answering at all?
//   UfsHost::enableController  did the loader leave it disabled?
//   probePhy              did the loader leave the PHY powered?
//   UfsHost::requestLink  is there a device on the other side of the link?
//
// No stage assumes the previous one worked, and every stage is bounded in
// time, so a failure leaves one readable snapshot instead of a half-applied
// sequence. Change one stage at a time; the others do not move.
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "../PongProbe.h"
extern "C" bool pong_ufs_transport_start(IOService *);

extern "C" {
volatile uint32_t native_ufs_probe_calls;
volatile uint32_t native_ufs_matched;
volatile uint32_t native_ufs_probe_requested;
volatile uint32_t native_ufs_mapped;
volatile uint32_t native_ufs_status;
volatile uint32_t pong_ufs_report[16];
extern volatile uint64_t armcpu_timer_interrupts;
}

namespace {
constexpr uint64_t kGccUfsPage = 0x00187000ULL;
constexpr uint64_t kUfsHost = 0x01d84000ULL;
constexpr uint64_t kUfsHostBytes = 0x3000ULL;
// SM8475 moves the QMP PHY to 0x1d80000 (sm8475.dtsi: phy@1d80000, 0x2000);
// 0x1d87000 is where sm8450 puts it, one page past the end of this host window.
// The block is laid out in v6 groups: serdes at 0, PCS at 0x400, TX at 0x1000,
// RX at 0x1200. SM8475 selects qmp_ufs_offsets_v6, so the older
// 0x400/0x600/0xc00 grouping names the wrong blocks -- 0x400 is PCS, not TX.
constexpr uint64_t kUfsPhyPage = 0x01d80000ULL;
constexpr uint64_t kUfsPhyBytes = 0x2000ULL;
constexpr unsigned kPhySerdes = 0x000;
constexpr unsigned kPhyPcs = 0x400;
constexpr unsigned kPhyTx = 0x1000;
constexpr unsigned kPhyRx = 0x1200;
// PCS registers, offsets relative to kPhyPcs (phy-qcom-qmp-pcs-ufs-v6.h).
constexpr unsigned kPcsStart = 0x000;
constexpr unsigned kPcsPowerDown = 0x004;
constexpr unsigned kPcsSwReset = 0x008;
constexpr unsigned kPcsReady = 0x1a8;
constexpr uint32_t kPcsReadyBit = 1U << 0;
constexpr uint32_t kUfsStreamId = 0xe0;

// GDSC: bit31 is PWR_ON, bit30 is SW_COLLAPSE. CBCR bit0 is CLK_OFF, so a gate
// that is running reads zero there. The same page holds the block reset the
// board description gives the controller (GCC_UFS_PHY_BCR, 0x87000); while it
// is asserted the whole host window ignores writes.
constexpr unsigned kGdscOffset = 0x004;
constexpr uint32_t kGdscPowerOn = 1U << 31;
constexpr uint32_t kGdscSwCollapse = 1U << 30;
constexpr uint32_t kGateClockOff = 1U;
constexpr unsigned kBcrOffset = 0x000;
constexpr uint32_t kBcrAsserted = 1U;

// The PHY reference clock lives in a different GCC page (0x9c000) than the
// controller's clock gates, and no UFS gate covers it. Without it the PHY PLL
// never locks, so READY stays low however the rest of the sequence is ordered.
constexpr uint64_t kUfsClkrefPage = 0x0019c000ULL;
constexpr uint64_t kUfsClkrefBytes = 0x1000ULL;
constexpr unsigned kUfsClkrefOffset = 0x000;

// UIC commands share one doorbell: the arguments are staged, the opcode is
// written last, and completion arrives as one bit in the interrupt status
// register. Polling it keeps the call bounded even when nothing is listening.
// Arguments are named after the UFSHCI registers they live in: the result code
// comes back in ARG_2 and the DME attribute value in ARG_3.
constexpr unsigned kUicCommand = 0x90;
constexpr unsigned kUicArg1 = 0x94;
constexpr unsigned kUicArg2 = 0x98;
constexpr unsigned kUicArg3 = 0x9c;
constexpr uint32_t kUicDone = 1U << 10;
constexpr uint32_t kDmeLinkStartup = 0x16;

// Interrupt status bit2 is UIC error; HCS bit3 says the UIC accepts a command.
// A loader that tears UFS down leaves the UIC error latched while HCS keeps
// reporting the device present, because nobody clears either on the way out.
constexpr uint32_t kIsUicError = 1U << 2;
constexpr uint32_t kHcsDevicePresent = 1U << 0;
constexpr uint32_t kHcsUicReady = 1U << 3;
constexpr unsigned kUicErrorDataLink = 0x3c;
constexpr unsigned kInterruptStatus = 0x20;

enum ReportIndex : unsigned {
  ReportGdsc, ReportAxi, ReportAhb, ReportTx, ReportRx0, ReportRx1,
  ReportUnipro, ReportIce, ReportAux, ReportAggregate,
  ReportCapability, ReportVersion, ReportInterruptStatus, ReportHostStatus,
  ReportHostEnable, ReportStream
};

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

static uint32_t uicLinkStartup (uintptr_t host)
{
  write32 (host, kUicArg1, 0);
  write32 (host, kUicArg2, 0);
  write32 (host, kUicArg3, 0);
  write32 (host, 0x20, kUicDone);            // drop a stale completion
  write32 (host, kUicCommand, kDmeLinkStartup);
  for (unsigned spin = 0; spin < 20000; ++spin) {
    if (read32 (host, 0x20) & kUicDone) {
      write32 (host, 0x20, kUicDone);
      return read32 (host, kUicArg2);        // result code in the low byte
    }
    IODelay (10);
  }
  return 0xffffffffU;                        // never completed
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = strlen (needle);
  for (size_t at = 0; at + wanted < length; ++at) {
    if ((at == 0 || data[at - 1] == '\0') && data[at + wanted] == '\0' &&
        !memcmp (data + at, needle, wanted)) return true;
  }
  return false;
}

static bool compatible (IOService *provider)
{
  OSObject *property = provider->getProperty ("compatible");
  if (OSData *data = OSDynamicCast (OSData, property)) {
    const char *bytes = static_cast<const char *> (data->getBytesNoCopy ());
    return bytes && (contains (bytes, data->getLength (), "qcom,sm8475-ufshc") ||
                     contains (bytes, data->getLength (), "qcom,sm8450-ufshc"));
  }
  if (OSString *string = OSDynamicCast (OSString, property)) {
    const char *bytes = string->getCStringNoCopy ();
    return bytes && (!strcmp (bytes, "qcom,sm8475-ufshc") ||
                     !strcmp (bytes, "qcom,sm8450-ufshc"));
  }
  return false;
}

static uintptr_t mapPhysical (uint64_t physical, uint64_t bytes,
                              OSSharedPtr<IOMemoryMap> &mapping)
{
  OSSharedPtr<IOMemoryDescriptor> memory (
      IOMemoryDescriptor::withPhysicalAddress (physical, bytes, kIODirectionInOut), OSNoRetain);
  if (!memory) return 0;
  mapping = OSSharedPtr<IOMemoryMap> (
      memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  return mapping ? static_cast<uintptr_t> (mapping->getVirtualAddress ()) : 0;
}

static unsigned serviceCount (const char *name)
{
  OSSharedPtr<OSDictionary> matching (IOService::serviceMatching (name), OSNoRetain);
  OSSharedPtr<OSIterator> iterator (matching ? IOService::getMatchingServices (matching.get()) : nullptr, OSNoRetain);
  unsigned count = 0;
  while (iterator && iterator->getNextObject ()) ++count;
  return count;
}

// Run outside the matching thread. Waking from this sleep also demonstrates
// that the scheduler and the architectural timer can make forward progress.
static void registrationReport (void *, wait_result_t)
{
  IOLog ("PONG REGISTRY: observer started, waiting for scheduler tick\n");
  PongProbeRecord (1, "timer-scheduler", PongProbeRunning, "waiting for one-second sleep");
  uint64_t before = armcpu_timer_interrupts;
  IOSleep (1000);
  uint64_t after = armcpu_timer_interrupts;
  IOLog ("PONG REGISTRY: timer IRQ before=%llu after=%llu\n", before, after);
  PongProbeRecord (1, "timer-scheduler", after > before ? PongProbePass : PongProbeFail,
                   after > before ? "sleep returned and timer IRQ advanced" : "sleep returned without timer IRQ growth", after - before);
  unsigned platform = serviceCount ("ArmDTPlatform"), cpu = serviceCount ("ArmCpu");
  unsigned controller = serviceCount ("IOCPUInterruptController"), nvram = serviceCount ("IODTNVRAM");
  unsigned ufs = serviceCount ("NativeUfs"), media = serviceCount ("IOMedia");
  IOLog ("PONG REGISTRY: platform=%u cpu=%u cpu-intc=%u nvram=%u ufs-probe=%u\n",
         platform, cpu, controller, nvram, ufs);
  PongProbeRecord (2, "core-registry", platform && cpu && controller && nvram ? PongProbePass : PongProbeFail,
                   "platform/cpu/intc/nvram counts packed as bytes",
                   ((uint64_t)platform << 24) | ((uint64_t)cpu << 16) | ((uint64_t)controller << 8) | nvram);
  IOLog ("PONG REGISTRY: block-device=%u media=%u; diagnostic-only mode skips block transport\n",
         serviceCount ("IOBlockStorageDevice"), media);
  PongProbeRecord (3, "root-media", media ? PongProbePass : PongProbeSkip,
                   media ? "published IOMedia observed" : "no block transport; waiting for real root media", media);
}

static bool probeFailure (const char *reason, uint64_t detail = 0)
{
  native_ufs_status |= 0x80000000U;
  PongProbeRecord (0, "ufs-registers", PongProbeFail, reason, detail);
  return false;
}

// ---------------------------------------------------------------- clocks ---
//
// Offsets are from upstream gcc-sm8450.c, which also matches qcom,sm8475-gcc.
// Each gate names the slot its value lands in, so the snapshot and the
// power-up loop can never disagree about which register is which.
struct Gate {
  const char *name;
  unsigned offset;
  ReportIndex report;
};

const Gate kClockGates[] = {
  {"axi", 0x018, ReportAxi},       {"ahb", 0x020, ReportAhb},
  {"tx", 0x024, ReportTx},         {"rx0", 0x028, ReportRx0},
  {"rx1", 0x0c0, ReportRx1},       {"unipro", 0x064, ReportUnipro},
  {"ice", 0x06c, ReportIce},       {"aux", 0x0a4, ReportAux},
  {"aggr", 0x0d4, ReportAggregate},
};

static void sampleClocks (uintptr_t gcc)
{
  pong_ufs_report[ReportGdsc] = read32 (gcc, kGdscOffset);
  for (const auto &gate : kClockGates)
    pong_ufs_report[gate.report] = read32 (gcc, gate.offset);
}

// Same CBCR semantic as the gates below: bit0 is CLK_OFF, so clearing it turns
// the reference clock on. gcc_ufs_0_clkref_en uses clk_branch2_ops with
// halt_check BRANCH_HALT, so the bit coming back as zero is the whole proof.
static void bringUpReferenceClock (uintptr_t clkref)
{
  if (!clkref) {
    IOLog ("PONG_UFS: reference clock page not mapped\n");
    return;
  }
  uint32_t value = read32 (clkref, kUfsClkrefOffset);
  if (!(value & kGateClockOff)) {
    IOLog ("PONG_UFS: reference clock already on %08x\n", value);
    return;
  }
  write32 (clkref, kUfsClkrefOffset, value & ~kGateClockOff);
  for (unsigned spin = 0; spin < 1000; ++spin) {
    if (!(read32 (clkref, kUfsClkrefOffset) & kGateClockOff)) break;
    IODelay (10);
  }
  IOLog ("PONG_UFS: reference clock CLK_OFF cleared %08x -> %08x\n",
         value, read32 (clkref, kUfsClkrefOffset));
}

// GCC_UFS_PHY_BCR (gcc-sm8450.c reset map, 0x87000 bit0) is the block reset the
// board description hands the controller. While it is asserted the block reads
// back as zero throughout -- collapsed GDSC, every gate "off" -- which is
// indistinguishable from a controller the loader tore down. So it is read and
// reported first, and released before any state below is judged.
static void releaseBlockReset (uintptr_t gcc)
{
  uint32_t bcr = read32 (gcc, kBcrOffset);
  if (!(bcr & kBcrAsserted)) {
    IOLog ("PONG_UFS: BCR already released %08x\n", bcr);
    return;
  }
  write32 (gcc, kBcrOffset, bcr & ~kBcrAsserted);
  for (unsigned spin = 0; spin < 1000; ++spin) {
    if (!(read32 (gcc, kBcrOffset) & kBcrAsserted)) break;
    IODelay (10);
  }
  IOLog ("PONG_UFS: BCR was asserted %08x -> %08x\n", bcr, read32 (gcc, kBcrOffset));
}

// A loader that leaves UFS running hands over live clocks. One that tears the
// controller down hands over the GDSC collapsed and CLK_OFF set on the gates
// that feed the host -- which reads back as a window full of zeroes. Fix both
// here, and judge the result on the re-read state, never on the handover state.
static void bringUpClocks (uintptr_t gcc, uintptr_t clkref)
{
  // The PHY multiplies this reference into its own PLL; without it READY never
  // rises no matter how the rest of the sequence is ordered, so it goes first.
  bringUpReferenceClock (clkref);
  releaseBlockReset (gcc);
  sampleClocks (gcc);
  IOLog ("PONG_UFS: GCC before gdsc=%08x axi=%08x ahb=%08x aggr=%08x\n",
         pong_ufs_report[ReportGdsc], pong_ufs_report[ReportAxi],
         pong_ufs_report[ReportAhb], pong_ufs_report[ReportAggregate]);

  uint32_t gdsc = pong_ufs_report[ReportGdsc];
  if (!(gdsc & kGdscPowerOn)) {
    write32 (gcc, kGdscOffset, gdsc & ~kGdscSwCollapse);
    for (unsigned spin = 0; spin < 1000; ++spin) {
      if (read32 (gcc, kGdscOffset) & kGdscPowerOn) break;
      IODelay (10);
    }
    IOLog ("PONG_UFS: GDSC collapsed (%08x), power-up -> %08x\n",
           gdsc, read32 (gcc, kGdscOffset));
  }

  for (const auto &gate : kClockGates) {
    uint32_t value = read32 (gcc, gate.offset);
    if (!(value & kGateClockOff)) continue;
    write32 (gcc, gate.offset, value & ~kGateClockOff);
    for (unsigned spin = 0; spin < 1000; ++spin) {
      if (!(read32 (gcc, gate.offset) & kGateClockOff)) break;
      IODelay (10);
    }
    IOLog ("PONG_UFS: clock %s CLK_OFF cleared %08x -> %08x\n",
           gate.name, value, read32 (gcc, gate.offset));
  }

  sampleClocks (gcc);
}

static bool clocksPowered ()
{
  return (pong_ufs_report[ReportGdsc] & kGdscPowerOn) != 0;
}

static bool clockBusesRunning ()
{
  return !(pong_ufs_report[ReportAxi] & kGateClockOff) &&
         !(pong_ufs_report[ReportAhb] & kGateClockOff) &&
         !(pong_ufs_report[ReportAggregate] & kGateClockOff);
}

// ------------------------------------------------------------ controller ---
//
// The host window is read as one unit so the snapshot, the log line and the
// registry properties all describe the same moment.
struct UfsHost {
  uintptr_t base;
  uint32_t capability;
  uint32_t version;
  uint32_t interruptStatus;
  uint32_t status;
  uint32_t hce;

  void sample ()
  {
    capability = read32 (base, 0x00);
    version = read32 (base, 0x08);
    interruptStatus = read32 (base, 0x20);
    status = read32 (base, 0x30);
    hce = read32 (base, 0x34);
  }

  // A controller the loader left disabled stops every later step silently, so
  // the write is read back instead of assumed to have taken.
  void enableController ()
  {
    if (hce & 1U) return;
    uint32_t before = hce;
    write32 (base, 0x34, before | 1U);
    for (unsigned spin = 0; spin < 1000; ++spin) {
      if (read32 (base, 0x34) & 1U) break;
      IODelay (10);
    }
    hce = read32 (base, 0x34);
    status = read32 (base, 0x30);
    IOLog ("PONG_UFS: HCE %08x -> %08x hcs=%08x\n", before, hce, status);
  }

  // A handover can leave the device marked present while the link is dead. The
  // loader pulls the clocks and the block reset on its way out, the UniPro layer
  // latches a data-link error, and HCS keeps reporting DP=1 because nothing
  // clears it -- so presence alone is not proof that a command will ever reach
  // the device. A latched UIC error is: clear it and bring the link up again
  // instead of trusting the handover.
  void requestLink ()
  {
    if (!(hce & 1U)) return;
    uint32_t latched = read32 (base, kInterruptStatus) & kIsUicError;
    uint32_t dataLink = read32 (base, kUicErrorDataLink);
    bool degraded = latched || dataLink;
    if ((status & kHcsDevicePresent) && !degraded) return;
    if (degraded) {
      // IS.UE is re-latched by a live UIC error source. Clearing only the
      // summary bit leaves UECDL (replay timeout) armed, so LINKSTARTUP
      // fails and the controller keeps refusing the request doorbell.
      // UECPA/UECDL/UECN/UECT/UECDME are write-one-to-clear; write back
      // exactly what was read so no concurrent error is acknowledged.
      for (unsigned offset = 0x38; offset <= 0x48; offset += 4)
        write32 (base, offset, read32 (base, offset));
      write32 (base, kInterruptStatus, kIsUicError);
    }
    for (unsigned spin = 0; spin < 1000 && !(read32 (base, 0x30) & kHcsUicReady); ++spin)
      IODelay (10);
    uint32_t result = uicLinkStartup (base);
    for (unsigned spin = 0; spin < 1000 && !(read32 (base, 0x30) & kHcsDevicePresent); ++spin)
      IODelay (10);
    IOLog ("PONG_UFS: DME_LINKSTARTUP(%s) result=%08x hcs=%08x dl=%08x->%08x ue=%08x\n",
           degraded ? "recover" : "absent", result, read32 (base, 0x30),
           dataLink, read32 (base, kUicErrorDataLink),
           read32 (base, kInterruptStatus) & kIsUicError);
    status = read32 (base, 0x30);
    interruptStatus = read32 (base, kInterruptStatus);
  }

  void publish ()
  {
    pong_ufs_report[ReportCapability] = capability;
    pong_ufs_report[ReportVersion] = version;
    pong_ufs_report[ReportInterruptStatus] = interruptStatus;
    pong_ufs_report[ReportHostStatus] = status;
    pong_ufs_report[ReportHostEnable] = hce;
  }
};

// The PHY answers three different questions from three places: the PCS start
// and power-down controls say whether anyone ever programmed it, READY_STATUS
// says whether a clock is actually running through it, and the TX/RX blocks say
// whether the lane tables were loaded. Zeroes in the first pair mean the next
// step has to program the PHY from scratch rather than nudge a live one.
static void probePhy (uintptr_t phy)
{
  if (!phy) return;
  uint32_t ready = read32 (phy, kPhyPcs + kPcsReady);
  IOLog ("PONG_UFS: PHY start=%08x pwrdn=%08x reset=%08x ready=%08x(%s)\n",
         read32 (phy, kPhyPcs + kPcsStart), read32 (phy, kPhyPcs + kPcsPowerDown),
         read32 (phy, kPhyPcs + kPcsSwReset), ready,
         (ready & kPcsReadyBit) ? "ready" : "not ready");
  IOLog ("PONG_UFS: PHY serdes=%08x tx=%08x rx=%08x\n",
         read32 (phy, kPhySerdes), read32 (phy, kPhyTx), read32 (phy, kPhyRx));
}

// ------------------------------------------------------------- reporting ---

static void dumpSnapshot (bool powered, bool buses)
{
  IOLog ("PONG_UFS READ ONLY\n"
         "gdsc=%08x axi=%08x ahb=%08x aggr=%08x\n"
         "tx=%08x rx0=%08x rx1=%08x unipro=%08x\n"
         "ice=%08x aux=%08x powered=%u buses=%u sid=%x\n"
         "cap=%08x ver=%08x is=%08x hcs=%08x hce=%08x\n",
         pong_ufs_report[ReportGdsc], pong_ufs_report[ReportAxi],
         pong_ufs_report[ReportAhb], pong_ufs_report[ReportAggregate],
         pong_ufs_report[ReportTx], pong_ufs_report[ReportRx0],
         pong_ufs_report[ReportRx1], pong_ufs_report[ReportUnipro],
         pong_ufs_report[ReportIce], pong_ufs_report[ReportAux], powered, buses,
         pong_ufs_report[ReportStream], pong_ufs_report[ReportCapability],
         pong_ufs_report[ReportVersion], pong_ufs_report[ReportInterruptStatus],
         pong_ufs_report[ReportHostStatus], pong_ufs_report[ReportHostEnable]);
}

// Refuse a board description from a different Qualcomm generation. These
// windows are deliberately validated before the first MMIO read.
static bool validateDescription (IOService *provider)
{
  struct Window { const char *name; uint64_t expected; };
  const Window windows[] = {
    {"pong,ufs-hc-base", kUfsHost}, {"pong,ufs-hc-size", kUfsHostBytes},
    {"pong,gcc-ufs-page", kGccUfsPage},
    {"pong,smmu-base", 0x15000000}, {"pong,smmu-size", 0x100000}
  };
  for (const auto &window : windows) {
    OSData *value = OSDynamicCast (OSData, provider->getProperty (window.name));
    uint64_t physical = 0;
    if (!value || value->getLength () != sizeof (physical))
      return probeFailure (window.name);
    memcpy (&physical, value->getBytesNoCopy (), sizeof (physical));
    if (physical != window.expected) return probeFailure (window.name, physical);
  }
  struct Cell { const char *name; uint32_t expected; };
  const Cell cells[] = {{"pong,smmu-stream-id", kUfsStreamId}, {"pong,ufs-spi", 265},
                        {"pong,smmu-context-banks", 78}, {"pong,smmu-match-groups", 120}};
  for (const auto &cell : cells) {
    OSData *value = OSDynamicCast (OSData, provider->getProperty (cell.name));
    uint32_t observed = 0;
    if (!value || value->getLength () != sizeof (observed))
      return probeFailure (cell.name);
    memcpy (&observed, value->getBytesNoCopy (), sizeof (observed));
    if (observed != cell.expected) return probeFailure (cell.name, observed);
  }
  return true;
}
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
public:
  IOService *probe (IOService *, SInt32 *) override;
  bool start (IOService *) override;

private:
  void publishReport (unsigned mode);
  bool publishDiagnostic ();
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  ++native_ufs_probe_calls;
  if (!compatible (provider)) return nullptr;
  native_ufs_matched = 1;
  if (score) *score = 1000;
  return IOService::probe (provider, score);
}

void NativeUfs::publishReport (unsigned mode)
{
  setProperty ("PongProbeOnly", mode == 2);
  setProperty ("PongUfsStatus", static_cast<unsigned long long> (native_ufs_status), 32);
  // The snapshot is a volatile array; setProperty copies it into an OSData.
  uint32_t registers[16];
  for (unsigned index = 0; index < 16; ++index) registers[index] = pong_ufs_report[index];
  setProperty ("PongUfsRegisterSnapshot", registers,
               static_cast<unsigned int> (sizeof (registers)));
}

bool NativeUfs::publishDiagnostic ()
{
  registerService ();
  IOLog ("PONG_UFS: diagnostic service registered; continue IOKit, no block media published\n");
  PongProbeRecord (0, "ufs-registers", PongProbePass,
                   "registers read; diagnostic service published",
                   pong_ufs_report[ReportVersion]);
  thread_t thread = nullptr;
  kern_return_t result = kernel_thread_start (registrationReport, nullptr, &thread);
  if (result == KERN_SUCCESS) thread_deallocate (thread);
  else PongProbeRecord (1, "timer-scheduler", PongProbeFail, "observer thread creation failed",
                        static_cast<uint64_t> (result));
  return true;
}

bool NativeUfs::start (IOService *provider)
{
  unsigned mode = 0;
  PE_parse_boot_argn ("native-ufs-probe", &mode, sizeof (mode));
  native_ufs_probe_requested = mode;
  unsigned enabled = 1;
  PE_parse_boot_argn ("native-ufs", &enabled, sizeof (enabled));
  if (!enabled || !compatible (provider)) return false;
  IOLog("PONG_UFS BUILD smmu-cap-v2: optional-probe=%u; transport=%s\n", mode,
        mode == 2 ? "disabled by diagnostic mode" : "read-only");
  if (mode == 2) PongProbeRecord (0, "ufs-registers", PongProbeRunning, "validate SM8475 DT and read registers");
  if (!IOService::start (provider)) return probeFailure ("IOService start failed");
  if (!validateDescription (provider)) return false;
  IOLog ("PONG_UFS: matched SM8475, validate firmware clocks\n");
  native_ufs_status = 1;

  OSSharedPtr<IOMemoryMap> gccMap, clkrefMap, hostMap, phyMap;
  uintptr_t gcc = mapPhysical (kGccUfsPage, 0x1000, gccMap);
  if (!gcc) return probeFailure ("GCC map failed");
  uintptr_t clkref = mapPhysical (kUfsClkrefPage, kUfsClkrefBytes, clkrefMap);
  native_ufs_mapped = 1;
  native_ufs_status = 2;
  pong_ufs_report[ReportStream] = kUfsStreamId;

  bringUpClocks (gcc, clkref);
  bool powered = clocksPowered ();
  bool buses = clockBusesRunning ();

  UfsHost host = {};
  if (powered && buses) {
    IOLog ("PONG_UFS: clocks active, read host state\n");
    host.base = mapPhysical (kUfsHost, kUfsHostBytes, hostMap);
    if (!host.base) return probeFailure ("host map failed");
    native_ufs_status = 3;
    host.sample ();
    host.enableController ();
    probePhy (mapPhysical (kUfsPhyPage, kUfsPhyBytes, phyMap));
    host.requestLink ();
    host.publish ();
    native_ufs_status = 4;
  }

  dumpSnapshot (powered, buses);
  if (!powered || !buses)
    return probeFailure ("power/clocks unavailable; host access skipped");
  if (!host.version || host.version == 0xffffffffU || !(host.hce & 1U))
    return probeFailure ("host disabled or invalid version", host.hce);

  publishReport (mode);
  if (mode != 2) {
    if (!pong_ufs_transport_start (this)) return false;
    registerService ();
    return true;
  }
  return publishDiagnostic ();
}
