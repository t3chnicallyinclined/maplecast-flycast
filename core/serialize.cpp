// serialize.cpp : save states
#include "serialize.h"
#include "types.h"
#include "hw/aica/aica_if.h"
#include "hw/holly/sb.h"
#include "hw/flashrom/nvmem.h"
#include "hw/gdrom/gdrom_if.h"
#include "hw/maple/maple_cfg.h"
#include "hw/modem/modem.h"
#include "hw/pvr/pvr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mmr.h"
#include "reios/reios.h"
#include "hw/naomi/naomi.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/bba/bba.h"
#include "cfg/option.h"
#include "imgread/common.h"
#include "achievements/achievements.h"

// DC-SERIALIZE-AUDIT byte-diff harness instrumentation. Null in production.
// The harness sets this to a vector before calling dc_serialize, then reads
// the vector to bucket diffs by region name. See core/serialize.h.
thread_local std::vector<DcAuditMark>* dc_audit_marks = nullptr;

// Populated each time dc_serialize runs with marks active. Maps top-level
// subsystem names to their byte size in the blob. Used by dc_deserialize's
// MAPLECAST_SKIP_DESER_SUBSYS bisection helper.
thread_local std::vector<SubsysSize> dc_subsys_sizes;

static void recordSubsysSizes(const std::vector<DcAuditMark>& marks, size_t totalSize)
{
	dc_subsys_sizes.clear();
	for (size_t i = 0; i < marks.size(); i++) {
		bool isSubMark = strchr(marks[i].name, '.') != nullptr;
		size_t end = totalSize;
		if (isSubMark) {
			// Sub-marks: size to next ANY mark
			if (i + 1 < marks.size()) end = marks[i + 1].offset;
		} else {
			// Top-level: size to next top-level mark (skip sub-marks)
			for (size_t j = i + 1; j < marks.size(); j++) {
				if (strchr(marks[j].name, '.') == nullptr) {
					end = marks[j].offset;
					break;
				}
			}
		}
		dc_subsys_sizes.push_back({ marks[i].name, end - marks[i].offset });
	}
}

size_t dcs_lookup_subsys_size(const char* name)
{
	for (const auto& e : dc_subsys_sizes)
		if (strcmp(e.name, name) == 0) return e.size;
	return 0;
}

bool dcs_should_skip_subsys(const char* name)
{
	const char* env = std::getenv("MAPLECAST_SKIP_DESER_SUBSYS");
	if (!env) return false;
	// Comma-separated list: "aica,sh4,pvr"
	const char* p = env;
	while (*p) {
		const char* comma = strchr(p, ',');
		size_t n = comma ? (size_t)(comma - p) : strlen(p);
		if (n == strlen(name) && strncmp(p, name, n) == 0) {
			return true;
		}
		if (!comma) break;
		p = comma + 1;
	}
	return false;
}

#define lookupSubsysSize(name) dcs_lookup_subsys_size(name)
#define shouldSkipSubsys(name) dcs_should_skip_subsys(name)

#define MAYBE_SKIP(name, body) do { \
	if (shouldSkipSubsys(name)) { \
		size_t sz = lookupSubsysSize(name); \
		if (sz > 0) { \
			printf("[dc-deser] SKIPPING %s (%zu bytes)\n", name, sz); fflush(stdout); \
			deser.skip(sz); \
		} else { \
			printf("[dc-deser] requested skip of '%s' but size unknown — running normally\n", name); \
			fflush(stdout); \
			body; \
		} \
	} else { body; } \
} while (0)

void dc_serialize(Serializer& ser)
{
	dcs_mark(ser, "aica");
	aica::serialize(ser);

	dcs_mark(ser, "sb");
	sb_serialize(ser);

	dcs_mark(ser, "nvmem");
	nvmem::serialize(ser);

	dcs_mark(ser, "gdrom");
	gdrom::serialize(ser);

	dcs_mark(ser, "maple");
	mcfg_SerializeDevices(ser);

	dcs_mark(ser, "pvr");
	pvr::serialize(ser);

	dcs_mark(ser, "sh4");
	sh4::serialize(ser);

	dcs_mark(ser, "bba_flag");
	ser << config::EmulateBBA.get();
	if (config::EmulateBBA) {
		dcs_mark(ser, "bba");
		bba_Serialize(ser);
	}
	dcs_mark(ser, "modem");
	ModemSerialize(ser);

	dcs_mark(ser, "sh4_2");
	sh4::serialize2(ser);

	dcs_mark(ser, "libGDR");
	libGDR_serialize(ser);

	dcs_mark(ser, "naomi");
	naomi_Serialize(ser);

	dcs_mark(ser, "config_3");
	ser << config::Broadcast.get();
	ser << config::Cable.get();
	ser << config::Region.get();

	dcs_mark(ser, "naomi_cart");
	naomi_cart_serialize(ser);
	dcs_mark(ser, "reios");
	reios_serialize(ser);
	dcs_mark(ser, "achievements");
	achievements::serialize(ser);

	dcs_mark(ser, "END");
	// If the caller is recording marks (audit harness), also populate
	// the global subsystem-sizes table so dc_deserialize's bisection
	// helper can skip sub-subsystems by name.
	if (dc_audit_marks)
		recordSubsysSizes(*dc_audit_marks, ser.size());
	DEBUG_LOG(SAVESTATE, "Saved %d bytes", (u32)ser.size());
}

void dc_deserialize(Deserializer& deser)
{
	DEBUG_LOG(SAVESTATE, "Loading state version %d", deser.version());

	MAYBE_SKIP("aica",   aica::deserialize(deser));
	MAYBE_SKIP("sb",     sb_deserialize(deser));
	MAYBE_SKIP("nvmem",  nvmem::deserialize(deser));
	MAYBE_SKIP("gdrom",  gdrom::deserialize(deser));
	MAYBE_SKIP("maple",  mcfg_DeserializeDevices(deser));
	MAYBE_SKIP("pvr",    pvr::deserialize(deser));
	MAYBE_SKIP("sh4",    sh4::deserialize(deser));

	if (shouldSkipSubsys("bba_flag")) {
		size_t sz = lookupSubsysSize("bba_flag");
		printf("[dc-deser] SKIPPING bba_flag (%zu bytes)\n", sz); fflush(stdout);
		deser.skip(sz);
	} else {
		deser >> config::EmulateBBA.get();
	}
	if (config::EmulateBBA) {
		MAYBE_SKIP("bba", bba_Deserialize(deser));
	}
	MAYBE_SKIP("modem",  ModemDeserialize(deser));

	MAYBE_SKIP("sh4_2",  sh4::deserialize2(deser));

	MAYBE_SKIP("libGDR", libGDR_deserialize(deser));

	MAYBE_SKIP("naomi",  naomi_Deserialize(deser));

	if (shouldSkipSubsys("config_3")) {
		size_t sz = lookupSubsysSize("config_3");
		printf("[dc-deser] SKIPPING config_3 (%zu bytes)\n", sz); fflush(stdout);
		deser.skip(sz);
	} else {
		deser >> config::Broadcast.get();
		verify(config::Broadcast >= 0 && config::Broadcast <= 4);
		deser >> config::Cable.get();
		verify(config::Cable >= 0 && config::Cable <= 3);
		deser >> config::Region.get();
		verify(config::Region >= 0 && config::Region <= 3);
	}

	MAYBE_SKIP("naomi_cart",   naomi_cart_deserialize(deser));
	MAYBE_SKIP("reios",        reios_deserialize(deser));
	MAYBE_SKIP("achievements", achievements::deserialize(deser));
	sh4_sched_ffts();

	DEBUG_LOG(SAVESTATE, "Loaded %d bytes", (u32)deser.size());
}

Deserializer::Deserializer(const void *data, size_t limit, bool rollback)
	: SerializeBase(limit, rollback), data((const u8 *)data)
{
	if (!memcmp(data, "RASTATE\001", 8))
	{
		// RetroArch savestates now have several sections: MEM, ACHV, RPLY, etc.
		const u8 *p = this->data + 8;
		limit -= 8;
		while (limit > 8)
		{
			const u8 *section = p;
			u32 sectionSize = *(const u32 *)&p[4];
			p += 8;
			limit -= 8;
			if (!memcmp(section, "MEM ", 4))
			{
				// That's the part we're interested in
				this->data = p;
				this->limit = sectionSize;
				break;
			}
			sectionSize = (sectionSize + 7) & ~7;	// align to 8 bytes
			if (limit < sectionSize) {
				limit = 0;
				break;
			}
			p += sectionSize;
			limit -= sectionSize;
		}
		if (limit <= 8)
			throw Exception("Can't find MEM section in RetroArch savestate");
	}
	deserialize(_version);
	if (_version < V16)
		throw Exception("Unsupported version");
	if (_version > Current)
		throw Exception("Version too recent");

	if(_version >= V42 && settings.platform.isConsole())
	{
		u32 ramSize;
		deserialize(ramSize);
		if (ramSize != settings.platform.ram_size)
			throw Exception("Selected RAM Size doesn't match Save State");
	}
}

Serializer::Serializer(void *data, size_t limit, bool rollback)
	: SerializeBase(limit, rollback), data((u8 *)data)
{
	Version v = Current;
	serialize(v);
	if (settings.platform.isConsole())
		serialize(settings.platform.ram_size);
}
