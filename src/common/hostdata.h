// Pure helpers behind the Device info page: memory modules from SMBIOS, name clean-up and the parsers for the drive health data the
// Windows storage layer hands over as raw bytes. No windows.h here, so everything is unit-tested on any host.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../mgp/core/smbios.h"
#include "hw_iface.h"
#include "kv.h"

namespace bc {

// Text from firmware or a drive: control characters become spaces, runs of blanks collapse, the separators the snapshot encoding uses
// ('|' and ';') become spaces, and the result is cut to `maxLen` bytes on a character boundary.
std::string CleanText(const std::string& s, size_t maxLen = 64);

// SMBIOS memory-type code -> "DDR4" / "LPDDR5" ... ("" for codes that do not name a memory generation).
std::string MemTypeName(int smbiosTypeCode);

// Some firmware stores the JEDEC manufacturer id ("80CE") instead of a name. Known ids become the vendor name; placeholders such as
// "Unknown" or "Undefined" become ""; anything else is returned cleaned but unchanged.
std::string MemMakerName(const std::string& raw);

// The populated slots of an SMBIOS table, in table order (empty slots are left out).
std::vector<MemModule> MemModulesFromSmbios(const mgp::SmbiosInfo& si);

// Display title of a drive: the model without a trailing capacity token that is shown separately ("WD Green SN3000 2TB" -> "WD Green SN3000").
std::string StorageTitle(const std::string& model);

// Drive name from the vendor and product strings of the Windows device descriptor. The generic vendors "NVMe", "ATA" and "SCSI" carry no
// information and are dropped; a real vendor is put in front unless the product already starts with it.
std::string DriveModelName(const std::string& vendor, const std::string& product);

// Windows property STORAGE_DEVICE_DESCRIPTOR.BusType -> "NVMe" / "SATA" ... ("" for buses that are not internal drives: USB, SD, virtual ...).
std::string StorageBusName(int busType);

// NVMe SMART / health information log page (02h, 512 bytes). Fills temperature, wear, spare, warnings, power cycles, power-on hours and
// data written; fields the log does not carry stay at -1. Returns false when the buffer is too short.
bool ParseNvmeHealth(const uint8_t* log, size_t len, StorageDrive* d);

// Windows STORAGE_TEMPERATURE_DATA_DESCRIPTOR (property 52): the composite temperature in degrees Celsius, or -1.
int ParseStorageTemperature(const uint8_t* buf, size_t len);

// Marketing-style drive capacity from the byte count Windows reports (decimal units): 2000398934016 -> "2 TB", 512110190592 -> "512 GB",
// 1920383410176 -> "1.9 TB". "" when the size is unknown.
std::string FormatDriveSize(uint64_t bytes);

// Plain-text description of this laptop for a bug report: model, firmware, OS, processor, graphics, memory modules and drive models.
// Built from a snapshot; it never contains the computer name, a user name, serial numbers or any other identifier.
std::string DeviceSummary(const KV& snap);

}  // namespace bc
