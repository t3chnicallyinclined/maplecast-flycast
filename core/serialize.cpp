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
	DEBUG_LOG(SAVESTATE, "Saved %d bytes", (u32)ser.size());
}

void dc_deserialize(Deserializer& deser)
{
	DEBUG_LOG(SAVESTATE, "Loading state version %d", deser.version());

	aica::deserialize(deser);

	sb_deserialize(deser);

	nvmem::deserialize(deser);

	gdrom::deserialize(deser);

	mcfg_DeserializeDevices(deser);

	pvr::deserialize(deser);

	sh4::deserialize(deser);

	deser >> config::EmulateBBA.get();
	if (config::EmulateBBA)
		bba_Deserialize(deser);
	ModemDeserialize(deser);

	sh4::deserialize2(deser);

	libGDR_deserialize(deser);

	naomi_Deserialize(deser);

	deser >> config::Broadcast.get();
	verify(config::Broadcast >= 0 && config::Broadcast <= 4);
	deser >> config::Cable.get();
	verify(config::Cable >= 0 && config::Cable <= 3);
	deser >> config::Region.get();
	verify(config::Region >= 0 && config::Region <= 3);

	naomi_cart_deserialize(deser);
	reios_deserialize(deser);
	achievements::deserialize(deser);
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
