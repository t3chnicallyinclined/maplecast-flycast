/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "pvr.h"
#include "spg.h"
#include "pvr_regs.h"
#include "Renderer_if.h"
#include "ta.h"
#include "ta_ctx.h"
#include "serialize.h"
#include "pvr_mem.h"
#include "elan.h"

// ta.cpp
extern u8 ta_fsm[2049];	//[2048] stores the current state
extern u32 ta_fsm_cl;
extern u32 taRenderPass;
// pvr_regs.cpp
extern bool pal_needs_update;

namespace pvr
{

void reset(bool hard)
{
	Regs_Reset(hard);
	spg_Reset(hard);
	if (hard)
		rend_reset();
	tactx_Term();
	elan::reset(hard);
	if (hard)
	{
		ta_parse_reset();
		YUV_reset();
		taRenderPass = 0;
	}
}

void init()
{
	spg_Init();
	elan::init();
}

void term()
{
	tactx_Term();
	spg_Term();
	elan::term();
}

void serialize(Serializer& ser)
{
	dcs_mark(ser, "pvr.YUV");
	YUV_serialize(ser);

	dcs_mark(ser, "pvr.pvr_regs");
	ser << pvr_regs;

	dcs_mark(ser, "pvr.spg");
	spg_Serialize(ser);
	dcs_mark(ser, "pvr.rend");
	rend_serialize(ser);

	dcs_mark(ser, "pvr.ta_fsm");
	ser << ta_fsm[2048];
	ser << ta_fsm_cl;
	ser << taRenderPass;

	dcs_mark(ser, "pvr.taContext");
	SerializeTAContext(ser);

	// V60 — BaseTAParser host-side statics that dc_serialize previously
	// missed. Closes DC-SERIALIZE-AUDIT.md §2.1 priority-2 gap. Required
	// for the rollback ring's F.1 round-trip determinism test (A.4.b).
	dcs_mark(ser, "pvr.taParser");
	ta_vtx_serializeParserState(ser);

	dcs_mark(ser, "pvr.vram");
	if (!ser.rollback())
		vram.serialize(ser);
	dcs_mark(ser, "pvr.elan");
	elan::serialize(ser);
}

void deserialize(Deserializer& deser)
{
	YUV_deserialize(deser);

	deser >> pvr_regs;
	rend_updateFogTable();

	spg_Deserialize(deser);

	rend_deserialize(deser);

	deser >> ta_fsm[2048];
	deser >> ta_fsm_cl;
	if (deser.version() >= Deserializer::V29)
		deser >> taRenderPass;
	else
		taRenderPass = 0;
	DeserializeTAContext(deser);

	if (deser.version() >= Deserializer::V60)
		ta_vtx_deserializeParserState(deser);
	// V<60: BaseTAParser statics get reset to defaults (handled inside
	// ta_vtx via reset() at boot / on softreset). Acceptable for old
	// .state files but doesn't satisfy F.1 byte-equality.

	if (!deser.rollback())
		vram.deserialize(deser);
	elan::deserialize(deser);
	pal_needs_update = true;
}

}
