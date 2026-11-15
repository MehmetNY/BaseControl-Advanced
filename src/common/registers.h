// EC register map for the Uniwill/Tongfang "GM5TG7x" family (Monster Tulpar T5 V21.7 GM5TG7Y,
// XMG Neo 15 M21 GM5TG7W, Eluktronics MAX-15, Mechrevo family). ONE place for every address.
//
// Control Center Advanced READS these registers: the live values every slider follows, and the check that a change made through the vendor's
// service arrived. The only registers it ever writes are the two cTGP registers (kGpuCtrl, kGpuCtgpOffset), and only through the GPU power
// engine; see writeguard.cpp. The notes below say what is known about each register; the mappings come from the owner's measurements on the
// Tulpar T5, the vendor's own program, and independent open-source work on the same embedded-controller family (see NOTICE.txt). Anything that
// is read is plausibility-checked at start-up, and the feature that depends on it is switched off when the check fails.
#pragma once
#include <cstdint>

namespace bc {
namespace reg {

// ---- identity / status ---------------------------------------------------------------------------
inline constexpr uint16_t kProjectId = 0x0740;       // raw>>1 = project id (IDY family)
inline constexpr uint16_t kEcActive = 0x0741;        // EC "service active" byte (bit0 set = EC processes PL/fan/GPU writes)

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

// ---- CPU power (read only): the vendor's service writes these, this program shows them and checks that a change arrived ----
inline constexpr uint16_t kPl1 = 0x0783;             // W, direct (observed 120 on the Tulpar)
inline constexpr uint16_t kPl2 = 0x0784;             // W, direct (observed 120)
inline constexpr uint16_t kPl4 = 0x0785;             // observed raw 200 while the OEM UI showed 200 W -> raw == watts on this device
inline constexpr uint16_t kTccWrite = 0x0786;        // 0x80 | offset (bit7 = commit)
inline constexpr uint16_t kCtwa = 0x0788;            // mirrors cTGP increment (observed 15) - meaning not established

// ---- GPU power -----------------------------------------------------------------------------------------
inline constexpr uint16_t kGpuCtrl = 0x0743;         // bit0 general, bit1 Dynamic Boost, bit2 cTGP (0x05 = cTGP on)
inline constexpr uint16_t kGpuCtgpOffset = 0x0744;   // cTGP increment over 125 W (0..25, step 5)

// ---- fans -----------------------------------------------------------------------------------------------
inline constexpr uint16_t kFanMode = 0x0751;         // bit6 = fan boost (read only here; bits 4/7 are EC mode bits)
inline constexpr uint16_t kFanFixedCpu = 0x0759;     // bit7 on + percent: set when another program holds the fan at a fixed speed
inline constexpr uint16_t kFanFixedGpu = 0x075A;
inline constexpr uint16_t kFanSwitchSpeed = 0x0787;  // bit7 enable, bits0..6 x100 ms
inline constexpr uint16_t kFanCurveEnable = 0x07C5;  // bit7 curve enable
inline constexpr uint16_t kFanCurveLatch = 0x07C6;   // bit2 = custom-table mode flag (bit6 must never be touched by anything)
inline constexpr uint16_t kFanTableCpuUp = 0x0F00;   // 16 entries each: up / down / duty
inline constexpr uint16_t kFanTableCpuDown = 0x0F10;
inline constexpr uint16_t kFanTableCpuDuty = 0x0F20;
inline constexpr uint16_t kFanTableGpuUp = 0x0F30;
inline constexpr uint16_t kFanTableGpuDown = 0x0F40;
inline constexpr uint16_t kFanTableGpuDuty = 0x0F50;

// ---- EC feature bits (Uniwill driver definitions) -------------------------------------------------
inline constexpr uint16_t kFnLock = 0x074E;          // bit4
inline constexpr uint16_t kAcAutoBoot = 0x0726;      // bit3 = power on when AC is plugged; bit7 = EC power-management (PECI) loop on.
                                                     // Bits 0..2 are battery age-derating commands: nothing here ever touches them.
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
