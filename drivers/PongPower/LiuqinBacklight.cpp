// SPDX-License-Identifier: BSD-2-Clause-Patent
// liuqin LCD brightness: the Kinetic KTZ8866 backlight driver at I2C address 0x11 on QUP0 SE1
// (the vendor's i2c@984000, pins gpio4/gpio5 "qup1"). It replaces PongBacklight.cpp on this board:
// pong dims an OLED with DSI DCS commands, liuqin's LCD panel is lit by the KTZ8866 and its 11-bit
// brightness lives in two registers. The CoreBrightness side (AppleARMBacklight conformance, the
// properties backboardd writes, fades) follows PongBacklight.cpp unchanged.
//
// Hardware facts:
// - KTZ8866: BL_BRT_LSB 0x04 holds brightness bits 2:0, BL_BRT_MSB 0x05 bits 10:3 (0..2047);
//   BL_EN 0x08 bit 6 is the backlight enable, bits 5:0 the current sinks. Linux
//   drivers/video/backlight/ktz8866.c writes exactly these for a brightness change (LSB, then MSB;
//   BL_EN bit 6 cleared at 0 and set again above it), and the mainline liuqin port runs it on this bus.
// - The ABL leaves the backlight on with its LCD bias, sinks and ramp configured; nothing here touches
//   those (no BL_CFG*, LCD_BIAS*, enable GPIO6 or ENP/ENN GPIO66/53).
// - GCC (Linux gcc-sm8450.c): QUPV3_WRAP0_S1 branch 0x27140 voted by 0x62008 bit 11, its RCG at
//   0x27148 (DFS status +0x14, perf levels +0x1c); the wrapper AHB branches 0x27004/0x27008 and core
//   votes are the ones drivers/LiuqinTouch uses for SE4.
// - The GENI engine init and the 19.2 MHz I2C timing table follow PongAudio.cpp's bus on QUP1 SE5;
//   100 kHz (clk_div 7, t_high 10, t_low 12, t_cycle 26) as Linux i2c-qcom-geni.c, the rate the
//   mainline port runs this chip at.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "GeniI2c.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" void PongBacklightStaticInit(void);
class LiuqinBacklight;

namespace {
LiuqinBacklight *gPublishedBacklight;
const OSMetaClass *gAppleBacklightClass;

constexpr uint64_t kGcc = 0x100000, kGccSize = 0x70000;
constexpr uint64_t kGpio = 0xf100000, kGpioSize = 0x7000;   // TLMM GPIO 0..6, 0x1000 apart
constexpr uint64_t kSe = 0x984000, kSeSize = 0x4000;
constexpr uint32_t kVote = 0x62008;
constexpr uint32_t kVoteBits = (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 11);
constexpr uint32_t kMAhbCbcr = 0x27004, kSAhbCbcr = 0x27008, kSe1Cbcr = 0x27140;
constexpr uint32_t kSe1Cmd = 0x27148, kSe1Cfg = 0x2714c, kSe1Dfsr = 0x2715c, kSe1PerfCfg = 0x27164;
constexpr uint32_t kClockOff = 1u << 31;
constexpr unsigned kSdaPin = 4, kSclPin = 5;
constexpr uint32_t kPinI2c = 3u | (1u << 2);                 // pull-up, function 1 (qup1), 2 mA
constexpr uint32_t kGeniCgc = 0x28, kGeniOutput = 0x24, kGeniForceDefault = 0x20;
constexpr uint32_t kSeDmaGeneral = 0xe30, kSeGsiEvent = 0xe18, kSeClkSel = 0x7c;
constexpr uint32_t kClkCfg100k = (7u << 4) | 1u, kScl100k = (10u << 20) | (12u << 10) | 26u;
constexpr uint8_t kAddress = 0x11, kDeviceId = 0x01, kBrtLsb = 0x04, kBrtMsb = 0x05, kBlEn = 0x08;
constexpr uint8_t kBlEnBit = 0x40;
constexpr uint32_t kMaximum = 2047, kMinimum = 8;

struct Window {
    IOMemoryMap *map;
    volatile uint8_t *va;
    bool open(uint64_t base, uint64_t size) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
        map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        OSSafeReleaseNULL(memory);
        va = map ? reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()) : nullptr;
        return va != nullptr;
    }
    uint32_t r(uint32_t offset) const { return *reinterpret_cast<volatile uint32_t *>(va + offset); }
    void w(uint32_t offset, uint32_t value) {
        *reinterpret_cast<volatile uint32_t *>(va + offset) = value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void close() { OSSafeReleaseNULL(map); va = nullptr; }
};
Window gSe;
uint32_t seRead(void *, uint32_t offset) { return gSe.r(offset); }
void seWrite(void *, uint32_t offset, uint32_t value) { gSe.w(offset, value); }

uint64_t nowNs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

// Clock the serial engine from the 19.2 MHz XO, the rate the I2C timing above assumes. Returns the
// DFS level to select in the engine (0 when the RCG is not in DFS mode), or -1.
int clocks(Window &gcc) {
    gcc.w(kVote, gcc.r(kVote) | kVoteBits);
    int level = 0;
    if (gcc.r(kSe1Dfsr) & 1u) {
        level = -1;
        for (int i = 0; i < 8; i++) {
            const uint32_t cfg = gcc.r(kSe1PerfCfg + 4u * (uint32_t)i);
            if (((cfg >> 8) & 7u) == 0 && ((cfg >> 12) & 3u) == 0 && (cfg & 31u) <= 1) { level = i; break; }
        }
        if (level < 0) { IOLog("LiuqinBacklight: no 19.2 MHz DFS level on QUP0 SE1\n"); return -1; }
    } else {
        gcc.w(kSe1Cfg, (gcc.r(kSe1Cfg) & ~0x371fu) | 1u);
        gcc.w(kSe1Cmd, gcc.r(kSe1Cmd) | 1u);
        for (int i = 0; i < 500 && (gcc.r(kSe1Cmd) & 1u); i++) IODelay(10);
        if (gcc.r(kSe1Cmd) & 1u) { IOLog("LiuqinBacklight: QUP0 SE1 clock does not take XO\n"); return -1; }
    }
    for (int i = 0; i < 100; i++) {
        if (!((gcc.r(kMAhbCbcr) | gcc.r(kSAhbCbcr) | gcc.r(kSe1Cbcr)) & kClockOff)) return level;
        IOSleep(1);
    }
    IOLog("LiuqinBacklight: QUP0 SE1 clocks stay off %x %x %x\n", gcc.r(kMAhbCbcr), gcc.r(kSAhbCbcr), gcc.r(kSe1Cbcr));
    return -1;
}
}

class LiuqinBacklight : public IOService {
    OSDeclareDefaultStructors(LiuqinBacklight)
    struct Op { uint8_t reg, value; bool read; };
    GeniI2c bus = {};
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    const OSMetaClass *appleClass = nullptr;
    bool published = false, stopped = false, busy = false;
    Op ops[4] = {};
    unsigned opCount = 0, opIndex = 0, failures = 0;
    uint8_t lsb = 0, msb = 0, enable = 0, deviceId = 0;
    uint32_t level = 0, sending = 0, from = 0, goal = 0;
    uint64_t deadline = 0, classDeadline = 0, fadeStart = 0, fadeDuration = 0;
    uint64_t request = 0;
    uint32_t requestedFade = 0, requests = 0, writes = 0;

    static uint32_t clamped(uint32_t value) {
        if (value > kMaximum) value = kMaximum;
        if (value && value < kMinimum) value = kMinimum;
        return value;
    }
    void publishLevel() {
        OSDictionary *item = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(1);
        OSNumber *minimum = OSNumber::withNumber(0ULL, 32);
        OSNumber *maximum = OSNumber::withNumber(kMaximum, 32);
        OSNumber *value = OSNumber::withNumber(level, 32);
        if (item && parameters && minimum && maximum && value) {
            item->setObject("min", minimum); item->setObject("max", maximum); item->setObject("value", value);
            parameters->setObject("brightness", item); setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(item); OSSafeReleaseNULL(parameters);
        OSSafeReleaseNULL(minimum); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(value);
    }
    void fail(const char *reason) {
        __atomic_store_n(&stopped, true, __ATOMIC_RELEASE);
        timer->cancelTimeout();
        setProperty("PongBacklightError", reason);
        IOLog("LiuqinBacklight: %s; requests=%u writes=%u i2c done=%u failed=%u timeouts=%u error=%u status=%x\n",
              reason, __atomic_load_n(&requests, __ATOMIC_RELAXED), writes, bus.done, bus.failed, bus.timeouts,
              bus.error, bus.error_status);
    }
    void publishFirst() {
        gAppleBacklightClass = appleClass;
        __atomic_store_n(&gPublishedBacklight, this, __ATOMIC_RELEASE);
        published = true;
        setName("AppleARMBacklight");
        setProperty("NativeSandboxGrantProcess", "backboardd");
        setProperty("backlight-control", true); setProperty("built-in", true);
        setProperty("LiuqinBacklightDeviceId", deviceId, 8);
        publishLevel(); registerService();
        IOLog("LiuqinBacklight: KTZ8866 service ready, level=%u max=%u enable=%02x id=%02x\n", level, kMaximum, enable, deviceId);
    }
    bool startOp() {
        const Op &op = ops[opIndex];
        const int rc = op.read ? geni_i2c_start_read(&bus, op.reg) : geni_i2c_start_write(&bus, op.reg, op.value);
        if (rc) return false;
        busy = true;
        deadline = nowNs() + 20000000ULL;
        timer->setTimeoutMS(1);
        return true;
    }
    void queue(uint8_t reg, uint8_t value, bool read) { ops[opCount++] = Op{reg, value, read}; }
    void service() {
        if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) return;
        if (!appleClass) {
            // AppleARMPlatform registers AppleARMBacklight after the framebuffer starts. Never register its
            // private class name here: its real initializer would panic on the duplicate (PongBacklight.cpp).
            const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
            appleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
            OSSafeReleaseNULL(name);
            if (!appleClass) {
                if (nowNs() >= classDeadline) { fail("native AppleARMBacklight class unavailable"); return; }
                timer->setTimeoutMS(100); return;
            }
            IOLog("LiuqinBacklight: native AppleARMBacklight class ready\n");
        }
        const uint64_t latest = __atomic_exchange_n(&request, 0ULL, __ATOMIC_ACQUIRE);
        if (latest >> 63) {
            from = level; goal = clamped((latest >> 16) & 0xffff);
            fadeStart = nowNs(); fadeDuration = (latest & 0xffff) * 1000000ULL;
        }
        if (busy) {
            int result = geni_i2c_poll(&bus);
            if (result == GeniPending) {
                if (nowNs() < deadline) { timer->setTimeoutMS(1); return; }
                // Cancel, then abort, as the engine documents; each step gets its own short deadline.
                result = geni_i2c_timeout(&bus);
                deadline = nowNs() + 5000000ULL;
                if (result == GeniPending) { timer->setTimeoutMS(1); return; }
            }
            busy = false;
            if (result == GeniFailed) {
                opCount = opIndex = 0;
                if (!published) { fail("KTZ8866 did not answer at 0x11"); return; }
                // Keep the last level the chip took; a later request retries.
                if (++failures > 8) { fail("repeated KTZ8866 I2C failures"); return; }
                setProperty("PongBacklightError", "KTZ8866 write failed");
                from = goal = level; fadeDuration = 0;
                timer->setTimeoutMS(50); return;
            }
            const Op &op = ops[opIndex];
            if (op.read) {
                if (op.reg == kBrtLsb) lsb = bus.data;
                else if (op.reg == kBrtMsb) msb = bus.data;
                else if (op.reg == kBlEn) enable = bus.data;
                else if (op.reg == kDeviceId) deviceId = bus.data;
            } else if (op.reg == kBlEn) {
                enable = op.value;
            }
            if (++opIndex < opCount) {
                if (!startOp()) fail("I2C engine busy");
                return;
            }
            opCount = opIndex = 0;
            if (!published) {
                level = (enable & kBlEnBit) ? (((uint32_t)msb << 3) | (lsb & 7u)) : 0;
                if (level > kMaximum) level = kMaximum;
                from = goal = level;
                publishFirst();
            } else {
                level = sending; writes++; failures = 0;
                removeProperty("PongBacklightError");
                publishLevel();
                setProperty("LiuqinBacklightWrites", writes, 32);
            }
        }
        if (!published) {
            queue(kDeviceId, 0, true); queue(kBlEn, 0, true); queue(kBrtLsb, 0, true); queue(kBrtMsb, 0, true);
            if (!startOp()) fail("I2C engine not ready");
            return;
        }
        const uint64_t elapsed = nowNs() - fadeStart;
        uint32_t next = goal;
        if (fadeDuration && elapsed < fadeDuration)
            next = (uint32_t)((int64_t)from + ((int64_t)goal - from) * (int64_t)elapsed / (int64_t)fadeDuration);
        next = clamped(next);
        if (next != level) {
            sending = next;
            if (next && !(enable & kBlEnBit)) queue(kBlEn, enable | kBlEnBit, false);
            queue(kBrtLsb, (uint8_t)(next & 7u), false);
            queue(kBrtMsb, (uint8_t)(next >> 3), false);
            if (!next && (enable & kBlEnBit)) queue(kBlEn, enable & (uint8_t)~kBlEnBit, false);
            if (!startOp()) fail("I2C engine busy");
        } else if (next != goal) {
            timer->setTimeoutMS(16);
        }
    }
    static void tick(OSObject *owner, IOTimerEventSource *) { static_cast<LiuqinBacklight *>(owner)->service(); }
public:
    bool prepare() {
        // Board identity: the ADT node make-pong-adt-ipados18.py writes only for liuqin.
        IORegistryEntry *board = IORegistryEntry::fromPath("/arm-io/liuqin-touch", gIODTPlane);
        const bool liuqin = board != nullptr;
        OSSafeReleaseNULL(board);
        if (!liuqin) { IOLog("LiuqinBacklight: not a liuqin ADT\n"); return false; }
        Window gcc = {}, pins = {};
        if (!gcc.open(kGcc, kGccSize) || !pins.open(kGpio, kGpioSize)) { gcc.close(); pins.close(); return false; }
        const int dfs = clocks(gcc);
        if (dfs >= 0) { pins.w(kSdaPin * 0x1000u, kPinI2c); pins.w(kSclPin * 0x1000u, kPinI2c); }
        gcc.close(); pins.close();
        if (dfs < 0 || !gSe.open(kSe, kSeSize)) return false;
        const uint32_t proto = (gSe.r(GENI_FW_REVISION) >> 8) & 0xffu;
        if (proto != GENI_PROTO_I2C) {
            IOLog("LiuqinBacklight: QUP0 SE1 firmware protocol=%u; I2C firmware required\n", proto);
            gSe.close(); return false;
        }
        gSe.w(kGeniCgc, gSe.r(kGeniCgc) | 0x7fu); gSe.w(kSeDmaGeneral, gSe.r(kSeDmaGeneral) | 15u);
        gSe.w(kGeniOutput, 0x7f); gSe.w(kGeniForceDefault, 1); gSe.w(kSeGsiEvent, 0);
        gSe.w(kSeClkSel, (uint32_t)dfs); gSe.w(GENI_CLK_CFG, kClkCfg100k); gSe.w(GENI_SCL_COUNTERS, kScl100k);
        const GeniIo io = {seRead, seWrite, nullptr};
        uint32_t config = 0;
        if (geni_i2c_prepare(&bus, &io, kAddress, &config)) {
            IOLog("LiuqinBacklight: QUP0 SE1 not usable, config=%x\n", config);
            gSe.close(); return false;
        }
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, tick);
        if (!loop || !timer || loop->addEventSource(timer)) return false;
        // userland/ui_compat18.c matches this value to enable its CoreBrightness startup fallback.
        setProperty("PongPanel", "KTZ8866 LCD / liuqin QUP0 SE1");
        classDeadline = nowNs() + 30000000000ULL;
        timer->setTimeoutMS(1);
        return true;
    }
    IOReturn setProperties(OSObject *object) override {
        OSDictionary *properties = OSDynamicCast(OSDictionary, object);
        if (!properties) return kIOReturnBadArgument;
        if (__atomic_load_n(&stopped, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
        // Darwin24 also sends the optional calibrated current-table level: metadata, not a register write.
        if (properties->getCount() > 3 ||
            properties->getCount() != (properties->getObject("brightness") ? 1u : 0u) +
                                      (properties->getObject("FadePeriod") ? 1u : 0u) +
                                      (properties->getObject("backlight-level") ? 1u : 0u)) return kIOReturnUnsupported;
        OSNumber *brightness = OSDynamicCast(OSNumber, properties->getObject("brightness"));
        OSNumber *fade = OSDynamicCast(OSNumber, properties->getObject("FadePeriod"));
        OSNumber *calibrated = OSDynamicCast(OSNumber, properties->getObject("backlight-level"));
        if ((properties->getObject("brightness") && !brightness) ||
            (properties->getObject("FadePeriod") && !fade) ||
            (properties->getObject("backlight-level") && !calibrated)) return kIOReturnBadArgument;
        const uint64_t fixed = fade ? fade->unsigned64BitValue() : 0;
        const uint64_t ms = fade ? (fixed > 5u * 65536u ? 5000u : (fixed * 1000) >> 16) :
                                  __atomic_load_n(&requestedFade, __ATOMIC_RELAXED);
        __atomic_store_n(&requestedFade, (uint32_t)ms, __ATOMIC_RELAXED);
        if (!brightness) return kIOReturnSuccess;
        const int64_t value = (int64_t)brightness->unsigned64BitValue();
        const uint32_t target = value < 0 ? 0 : clamped(value > (int64_t)kMaximum ? kMaximum : (uint32_t)value);
        __atomic_fetch_add(&requests, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&request, (1ULL << 63) | ((uint64_t)target << 16) | ms, __ATOMIC_RELEASE);
        timer->setTimeoutUS(1); // the caller never waits on I2C or the work-loop gate
        return kIOReturnSuccess;
    }
    void free() override {
        if (__atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this)
            __atomic_store_n(&gPublishedBacklight, (LiuqinBacklight *)nullptr, __ATOMIC_RELEASE);
        if (timer) { timer->cancelTimeout(); if (loop) loop->removeEventSource(timer); }
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(loop);
        IOService::free();
    }
};

// As PongBacklight.cpp: expand the metaclass definition so the conformance getter is chosen in source.
LiuqinBacklight::MetaClass LiuqinBacklight::gMetaClass;
const OSMetaClass *const LiuqinBacklight::metaClass = &LiuqinBacklight::gMetaClass;
const OSMetaClass *const LiuqinBacklight::superClass = &IOService::gMetaClass;
LiuqinBacklight::LiuqinBacklight(const OSMetaClass *meta) : IOService(meta) {}
LiuqinBacklight::~LiuqinBacklight() {}
const OSMetaClass *LiuqinBacklight::getMetaClass() const {
    return __atomic_load_n(&gPublishedBacklight, __ATOMIC_ACQUIRE) == this ? gAppleBacklightClass : &gMetaClass;
}
OSMetaClassConstructorInit(LiuqinBacklight, IOService, )
OSDefineDefaultStructors(LiuqinBacklight, IOService)

// Called by MiniIOMobileFramebuffer once the display started (XNU_PONG_BACKLIGHT), as on pong.
extern "C" void pong_backlight_start(IOService *display) {
    static LiuqinBacklight *backlight;
    static bool registered;
    unsigned enabled = 1;
    PE_parse_boot_argn("pong_backlight", &enabled, sizeof(enabled));
    if (!enabled || backlight || !display) return;
    if (!registered) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) { IOLog("LiuqinBacklight: class registration unavailable\n"); return; }
        PongBacklightStaticInit();
        if (OSMetaClass::postModLoad(handle)) { IOLog("LiuqinBacklight: class registration failed\n"); return; }
        registered = true;
    }
    LiuqinBacklight *service = OSTypeAlloc(LiuqinBacklight);
    if (!service) return;
    if (!service->init() || !service->attach(display) || !service->prepare()) {
        IOLog("LiuqinBacklight: KTZ8866 unavailable; keep the brightness the boot loader set\n");
        if (service->getProvider()) service->detach(display);
        service->release(); return;
    }
    backlight = service; // the service and its I2C engine persist until reboot
}
