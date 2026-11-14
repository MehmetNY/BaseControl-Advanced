// EC register map for the Uniwill/Tongfang "GM5TG7x" family (Monster Tulpar T5 V21.7 GM5TG7Y,
// XMG Neo 15 M21 GM5TG7W, Eluktronics MAX-15, Mechrevo family). ONE place for every address.
//
// Evidence tiers (how much we trust a mapping on THIS device):
//   Verified     - observed on the owner's Tulpar T5 (canary reads matched, owner-observed writes)
//   Family       - documented for the same Uniwill EC family by independent sources
//                  (MechControl-RE IDY_7Y, tuxedo/uniwill Linux driver, HydroControl, OEM Control Center strings);
//                  BaseControl plausibility-probes these at start-up and disables the feature if the probe fails
//   Experimental - single source or inferred; hidden behind "Experimental controls" and never auto-applied
#pragma once
#include <cstdint>

namespace bc {
namespace reg {

enum class Ev : uint8_t { Verified = 0, Family = 1, Experimental = 2 };
enum class Access : uint8_t { ReadOnly = 0, ReadWrite = 1, Forbidden = 2 };

struct RegDef {
  uint16_t addr;
  const char* name;
  Access access;
  Ev ev;
  const char* note;
};

// ---- identity / status ---------------------------------------------------------------------------
inline constexpr uint16_t kProjectId = 0x0740;       // raw>>1 = project id (IDY family)
inline constexpr uint16_t kEcActive = 0x0741;        // EC "service active" byte (bit0 set = EC processes PL/fan/GPU writes)
inline constexpr uint16_t kEcVersion = 0x0454;       // EC firmware version (single byte)

// ---- sensors (read only) ---------------------------------------------------------------------------
inline constexpr uint16_t kCpuTemp = 0x043E;         // deg C
inline constexpr uint16_t kGpuTemp = 0x044F;         // deg C
inline constexpr uint16_t kCpuTempLimitRead = 0x0463;  // low 7 bits: CPU temperature-limit offset as stored by the EC
inline constexpr uint16_t kCpuFanRpmHi = 0x0464;     // RPM = (hi<<8)|lo
inline constexpr uint16_t kCpuFanRpmLo = 0x0465;
inline constexpr uint16_t kGpuFanRpmHi = 0x046C;
inline constexpr uint16_t kGpuFanRpmLo = 0x046D;
inline constexpr uint16_t kAcStatus = 0x0490;        // bit0 = AC connected
inline constexpr uint16_t kSsdTemp = 0x07D1;         // deg C
inline constexpr uint16_t kCpuFanDuty = 0x075B;      // raw/2 = percent
inline constexpr uint16_t kGpuFanDuty = 0x075C;

// ---- CPU power (EC path; same registers the OEM Control Center writes) -----------------------------
inline constexpr uint16_t kPl1 = 0x0783;             // W, direct (observed 120 on the Tulpar)
inline constexpr uint16_t kPl2 = 0x0784;             // W, direct (observed 120)
inline constexpr uint16_t kPl4 = 0x0785;             // observed raw 200 while the OEM UI showed 200 W -> raw == watts on this device
inline constexpr uint16_t kTccWrite = 0x0786;        // 0x80 | offset (bit7 = commit)
inline constexpr uint16_t kCtwa = 0x0788;            // mirrors cTGP increment (observed 15) - meaning not established
inline constexpr uint16_t kPlDefaultsGaming = 0x0730;  // 0x730..0x733 PL1/PL2/PL4/D (read only reference)
inline constexpr uint16_t kPlDefaultsOffice = 0x0734;  // 0x734..0x737
inline constexpr uint16_t kCpuTau = 0x0738;

// ---- GPU power -----------------------------------------------------------------------------------------
inline constexpr uint16_t kGpuCtrl = 0x0743;         // bit0 general, bit1 Dynamic Boost, bit2 cTGP (0x05 = cTGP on)
inline constexpr uint16_t kGpuCtgpOffset = 0x0744;   // cTGP increment over 125 W (0..25, step 5)
inline constexpr uint16_t kGpuTpp = 0x0745;          // DB total processing power target (observed 0)
inline constexpr uint16_t kGpuDbMax = 0x0746;        // DB max (observed 0)

// ---- fans -----------------------------------------------------------------------------------------------
inline constexpr uint16_t kFanMode = 0x0751;         // bit6 = fan boost (read-modify-write; bits 4/7 are EC mode bits)
inline constexpr uint16_t kFanFixedCpu = 0x0759;     // bit7 on + percent (EXPERIMENTAL)
inline constexpr uint16_t kFanFixedGpu = 0x075A;
inline constexpr uint16_t kFanSwitchSpeed = 0x0787;  // bit7 enable, bits0..6 x100 ms
inline constexpr uint16_t kFanCurveEnable = 0x07C5;  // bit7 curve enable
inline constexpr uint16_t kFanCurveLatch = 0x07C6;   // bit2 = custom-table mode flag (never touch bit6!)
inline constexpr uint16_t kFanTableCpuUp = 0x0F00;   // 16 entries each: up / down / duty
inline constexpr uint16_t kFanTableCpuDown = 0x0F10;
inline constexpr uint16_t kFanTableCpuDuty = 0x0F20;
inline constexpr uint16_t kFanTableGpuUp = 0x0F30;
inline constexpr uint16_t kFanTableGpuDown = 0x0F40;
inline constexpr uint16_t kFanTableGpuDuty = 0x0F50;
inline constexpr uint16_t kFanTableEnd = 0x0F5F;

// ---- EC feature bits (Uniwill driver definitions) -------------------------------------------------
inline constexpr uint16_t kFnLock = 0x074E;          // bit4
inline constexpr uint16_t kAcAutoBoot = 0x0726;      // bit3 = power on when AC is plugged; bit7 = EC power-management (PECI) loop on.
                                                     // Bits 0..2 are battery age-derating commands: NEVER touched (masked RMW only).
inline constexpr uint16_t kCustomProfile = 0x0727;   // bit6 = CUME (CPU management enable); bit7 = PL4D, PL4 raw is doubled (read only)
inline constexpr uint16_t kUsbPowerShare = 0x0767;   // bit4 (this byte is shared with RGB mode bits 5/7!)
inline constexpr uint16_t kBatteryMode = 0x07A6;     // mask 0x38: 0x08 high capacity, 0x18 balanced, 0x28 eco
inline constexpr uint16_t kBatterySoc = 0x04AB;      // %
inline constexpr uint16_t kBatCapDesignLo = 0x0402;  // LE16, mAh
inline constexpr uint16_t kBatCapFullLo = 0x0404;    // LE16, mAh
inline constexpr uint16_t kBatCycleLo = 0x04A6;      // LE16
inline constexpr uint16_t kBatRateLo = 0x0434;       // LE16 mA
inline constexpr uint16_t kBatRemainLo = 0x0436;     // LE16 mAh
inline constexpr uint16_t kBatVoltageLo = 0x0438;    // LE16 mV
inline constexpr uint16_t kBatTempLo = 0x04A2;       // LE16, 0.1 K

// ---- lighting (EC driven part) ---------------------------------------------------------------------
inline constexpr uint16_t kLightbarR = 0x0749;       // 0..0x24
inline constexpr uint16_t kLightbarG = 0x074A;
inline constexpr uint16_t kLightbarB = 0x074B;
inline constexpr uint16_t kKbdStatus = 0x078C;

// ---- bring-up registers: only ever written inside GuardedEc::InitScope with exactly these values ---------
inline constexpr uint16_t kBiosCtrl = 0x0706;        // 0x41 = BIOS control init complete
inline constexpr uint16_t kEcHandshake2 = 0x0708;    // 0x01
inline constexpr uint16_t kEcHandshake1 = 0x0709;    // 0x05 (written before 0x0708)
inline constexpr uint16_t kFeatureEnable = 0x0748;   // 0x81
inline constexpr uint16_t kOcpl = 0x07C7;            // 0x0C
inline constexpr uint16_t kPl4Double = 0x0727;       // bit7 (read)

// ---- registers that must NEVER be written by this application -----------------------------------------
inline constexpr uint16_t kNeverWrite[] = {
    0x0459, 0x045D, 0x045E, 0x045F, 0x047E, 0x047F, 0x04FE, 0x04FF, 0x06E6, 0x06E7, 0x06E8, 0x06E9, 0x06EA, 0x06EB, 0x06EC,
    0x077E, 0x077F, 0x0780 /* EC power-limit bypass - dangerous */, 0x0781, 0x07E8, 0x07FD, 0x07FE, 0x07FF, 0x0DFF,
};

inline constexpr bool IsNeverWrite(uint16_t a) {
  for (uint16_t x : kNeverWrite)
    if (x == a) return true;
  return false;
}

}  // namespace reg
}  // namespace bc
