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
#include "emulator.h"
#include "types.h"
#include "stdclass.h"
#include "cfg/option.h"
#include "hw/aica/aica_if.h"
#include "imgread/common.h"
#include "hw/naomi/naomi_cart.h"
#include "reios/reios.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/flashrom/nvmem.h"
#include "cheats.h"
#include "audio/audiostream.h"
#include "debug/gdb_server.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/arm7/arm7_rec.h"
#include "network/ggpo.h"
#include "network/maplecast.h"
#ifndef MAPLECAST_HEADLESS_BUILD
#include "network/maplecast_stream.h"
#endif
#include "network/maplecast_telemetry.h"
#include "network/maplecast_input_server.h"
#include "network/maplecast_audio.h"
#include "network/maplecast_mirror.h"
#include "network/maplecast_rollback.h"
#include "network/maplecast_oracle_hook.h"   // generic-probe v2 no-restart live reload
#include "network/maplecast_predictor.h"
#include "network/mc_readtrace.h"             // STEP 2 read-set delta trace (dynarec->interp flip)
#include "network/replay_reader.h"
#include "network/replay_writer.h"
#include "network/maplecast_control_ws.h"
#include "network/maplecast_replica_live.h"
#include "network/maplecast_player.h"
#include "network/maplecast_replica.h"
#include "network/maplecast_state_replica.h"
#include "network/maplecast_input_sink.h"
#include "network/maplecast_evdev_input.h"
#ifdef _WIN32
#include "windows/rawinput_gamepad.h"
#endif
#include "ui/settings_window.h"
#include "hw/maple/maple_cfg.h"
#include <cstdlib>
#include <string>
#include <thread>
#include "network/ice.h"
#include "hw/mem/mem_watch.h"
#include "network/net_handshake.h"
#include "network/naomi_network.h"
#include "serialize.h"
#include "hw/pvr/pvr.h"
#include "profiler/fc_profiler.h"
#include "oslib/storage.h"
#include "wsi/context.h"
#include <chrono>
#ifndef LIBRETRO
#include "ui/gui.h"
#endif
#include "hw/sh4/sh4_interpreter.h"
#include "hw/sh4/dyna/ngen.h"
#include "oslib/i18n.h"

settings_t settings;

// A2 run-ahead: true once the run-ahead loop owns the rollback ring — gates the vblank
// auto-save (its divergent frame numbering evicted the loop's slot before the first rewind:
// "target 1 older than ring tail 22", local trace 2026-07-12).
static bool mc_runaheadArmed = false;
constexpr char const *BIOS_TITLE = "Dreamcast BIOS";

static void loadSpecialSettings()
{
	std::string& prod_id = settings.content.gameId;
	NOTICE_LOG(BOOT, "Game ID is [%s]", prod_id.c_str());

	if (settings.platform.isConsole())
	{
		// Tony Hawk's Pro Skater 2
		if (prod_id == "T13008D 05" || prod_id == "T13006N"
				// Tony Hawk's Pro Skater 1
				|| prod_id == "T40205N"
				// Tony Hawk's Skateboarding
				|| prod_id == "T40204D 50"
				// Skies of Arcadia
				|| prod_id == "MK-51052"
				// Eternal Arcadia (JP)
				|| prod_id == "HDR-0076"
				// Flag to Flag (US)
				|| prod_id == "MK-51007"
				// Super Speed Racing (JP)
				|| prod_id == "HDR-0013"
				// Yu Suzuki Game Works Vol. 1
				|| prod_id == "6108099"
				// L.O.L
				|| prod_id == "T2106M"
				// Miss Moonlight
				|| prod_id == "T18702M"
				// Tom Clancy's Rainbow Six (US)
				|| prod_id == "T40401N"
				// Tom Clancy's Rainbow Six incl. Eagle Watch Missions (EU)
				|| prod_id == "T-45001D05"
				// Jet Grind Radio (US)
				|| prod_id == "MK-51058"
				// JSR (JP)
				|| prod_id == "HDR-0078"
				// JSR (EU)
				|| prod_id == "MK-5105850"
				// Worms World Party (US)
				|| prod_id == "T22904N"
				// Worms World Party (EU)
				|| prod_id == "T7016D  50"
				// Shenmue (US)
				|| prod_id == "MK-51059"
				// Shenmue (EU)
				|| prod_id == "MK-5105950"
				// Shenmue (JP)
				|| prod_id == "HDR-0016"
				// Izumo
				|| prod_id == "T46902M"
				// Cardcaptor Sakura
				|| prod_id == "HDR-0115"
				// Grandia II (US)
				|| prod_id == "T17716N"
				// Grandia II (EU)
				|| prod_id == "T17715D"
				// Grandia II (JP)
				|| prod_id == "T4503M"
				// Canvas: Sepia Iro no Motif
				|| prod_id == "T20108M"
				// Kimi ga Nozomu Eien
				|| prod_id == "T47101M"
				// Pro Mahjong Kiwame D
				|| prod_id == "T16801M"
				// Yoshia no Oka de Nekoronde...
				|| prod_id == "T18704M"
				// Tamakyuu (a.k.a. Tama-cue)
				|| prod_id == "T20133M"
				// Sakura Taisen 1
				|| prod_id == "HDR-0072"
				// Sakura Taisen 3
				|| prod_id == "HDR-0152"
				// Hundred Swords
				|| prod_id == "HDR-0124"
				// Musapey's Choco Marker
				|| prod_id == "T23203M"
				// Sister Princess Premium Edition
				|| prod_id == "T27802M"
				// Sentimental Graffiti
				|| prod_id == "T20128M"
				// Sentimental Graffiti 2
				|| prod_id == "T20104M"
				// Kanon
				|| prod_id == "T20105M"
				// Aikagi
				|| prod_id == "T20130M"
				// AIR
				|| prod_id == "T20112M"
				// Cool Boarders Burrrn (JP)
				|| prod_id == "T36901M"
				// Castle Fantasia - Seima Taisen (JP)
				|| prod_id == "T46901M"
				// Silent Scope (US)
				|| prod_id == "T9507N"
				// Silent Scope (EU)
				|| prod_id == "T9505D"
				// Silent Scope (JP)
				|| prod_id == "T9513M"
				// Pro Pinball - Trilogy (EU)
				|| prod_id == "T30701D 50"
				// Jikkyo Powerful Pro Yakyu
				|| prod_id == "T9507M")
		{
			INFO_LOG(BOOT, "Enabling RTT Copy to VRAM for game %s", prod_id.c_str());
			config::RenderToTextureBuffer.override(true);
		}
		// Cosmic Smash
		if (prod_id == "HDR-0176" || prod_id == "RDC-0057")
		{
			INFO_LOG(BOOT, "Enabling translucent depth multipass for game %s", prod_id.c_str());
			config::TranslucentPolygonDepthMask.override(true);
		}
		// Extra Depth Scaling
		if (prod_id == "MK-51182")			// NHL 2K2
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(1e8f);
		}
		else if (prod_id == "T-8109N"		// Re-Volt (US, EU, JP)
				|| prod_id == "T8107D  50"
				|| prod_id == "T-8101M"
				|| prod_id ==  "DR001")		// Sturmwind
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(100.f);
		}
		else if (prod_id == "T15110N"		// Test Drive V-Rally
				|| prod_id == "T15105D 50")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(0.1f);
		}
		else if (prod_id == "T-8116N"		// South Park Rally
				|| prod_id == "T-8112D-50")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(1000.f);
		}
		else if (prod_id == "T1247M")		// Capcom vs. SNK - Millennium Fight 2000 Pro
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(10000.f);
		}

		std::string areas(ip_meta.area_symbols, sizeof(ip_meta.area_symbols));
		bool region_usa = areas.find('U') != std::string::npos;
		bool region_eu = areas.find('E') != std::string::npos;
		bool region_japan = areas.find('J') != std::string::npos;
		if (region_usa || region_eu || region_japan)
		{
			switch (config::Region)
			{
			case 0: // Japan
				if (!region_japan)
				{
					NOTICE_LOG(BOOT, "Japan region not supported. Using %s instead", region_usa ? "USA" : "Europe");
					config::Region.override(region_usa ? 1 : 2);
				}
				break;
			case 1: // USA
				if (!region_usa)
				{
					NOTICE_LOG(BOOT, "USA region not supported. Using %s instead", region_eu ? "Europe" : "Japan");
					config::Region.override(region_eu ? 2 : 0);
				}
				break;
			case 2: // Europe
				if (!region_eu)
				{
					NOTICE_LOG(BOOT, "Europe region not supported. Using %s instead", region_usa ? "USA" : "Japan");
					config::Region.override(region_usa ? 1 : 0);
				}
				break;
			case 3: // Default
				if (region_usa)
					config::Region.override(1);
				else if (region_eu)
					config::Region.override(2);
				else
					config::Region.override(0);
				break;
			}
		}
		else
			WARN_LOG(BOOT, "No region specified in IP.BIN");
		if (config::Cable <= 1 && (!ip_meta.supportsVGA()
				|| prod_id == "T-12504N"	// Caesar's Palace (NTSC)
				|| prod_id == "12502D-50"))	// Caesar's Palace (PAL)
		{
			NOTICE_LOG(BOOT, "Game doesn't support VGA. Using TV Composite instead");
			config::Cable.override(3);
		}
		if (config::Cable == 2 &&
				(prod_id == "T40602N"	 // Centipede
				|| prod_id == "T9710N"   // Gauntlet Legends (US)
				|| prod_id == "MK-51152" // World Series Baseball 2K2
				|| prod_id == "T-9701N"	 // Mortal Kombat Gold (US)
				|| prod_id == "T1203N"	 // Street Fighter Alpha 3 (US)
				|| prod_id == "T1203M"	 // Street Fighter Zero 3 (JP)
				|| prod_id == "T13002N"	 // Vigilante 8 (US)
				|| prod_id == "T13003N"	 // Toy Story 2 (US)
				|| prod_id == "T1209N"	 // Gigawing (US)
				|| prod_id == "T1208M"	 // Gigawing (JP)
				|| prod_id == "T1235M"   // Vampire Chronicle for Matching Service
				|| prod_id == "T22901N"  // Roadsters (US)
				|| prod_id == "T28202M"))// Shin Nihon Pro Wrestling 4
		{
			NOTICE_LOG(BOOT, "Game doesn't support RGB. Using TV Composite instead");
			config::Cable.override(3);
		}
		if (prod_id == "T7001D  50"	// Jimmy White's 2 Cueball
			|| prod_id == "T40505D 50"	// Railroad Tycoon 2 (EU)
			|| prod_id == "T18702M"		// Miss Moonlight
			|| prod_id == "T0019M"		// KenJu Atomiswave DC Conversion
			|| prod_id == "T0020M"		// Force Five Atomiswave DC Conversion
			|| prod_id == "HDR-0187"	// Fushigi no Dungeon - Fuurai no Shiren Gaiden - Onna Kenshi Asuka Kenzan!
			|| prod_id == "T15104D 50"	// Slave Zero (PAL)
			|| prod_id == "MK-51152")	// World Series Baseball 2K2
		{
			NOTICE_LOG(BOOT, "Forcing real BIOS");
			config::UseReios.override(false);
		}
		else if (prod_id == "T17708N"	// Stupid Invaders (US)
			|| prod_id == "T17711D"		// Stupid Invaders (EU)
			|| prod_id == "T46509M"		// Suika (JP)
			|| prod_id == "T36901M")	// Cool Boarders Burrrn (JP)
		{
			NOTICE_LOG(BOOT, "Forcing HLE BIOS");
			config::UseReios.override(true);
		}
		if (prod_id == "T-9707N"		// San Francisco Rush 2049 (US)
			|| prod_id == "MK-51146"	// Sega Smash Pack - Volume 1
			|| prod_id == "T-9702D-50"	// Hydro Thunder (PAL)
			|| prod_id == "T41601N"		// Elemental Gimmick Gear (US)
			|| prod_id == "T-8116N"		// South Park Rally (US)
			|| prod_id == "T1206N")		// JoJo's Bizarre Adventure (US)
		{
			NOTICE_LOG(BOOT, "Forcing NTSC broadcasting");
			config::Broadcast.override(0);
		}
		else if (prod_id == "T-9709D-50"	// San Francisco Rush 2049 (EU)
			|| prod_id == "T-8112D-50"		// South Park Rally (EU)
			|| prod_id == "T7014D  50"		// Super Runabout (EU)
			|| prod_id == "T10001D 50"		// MTV Sport - Skateboarding (PAL)
			|| prod_id == "MK-5101050"		// Snow Surfers
			|| prod_id == "12502D-50")		// Caesar's Palace (PAL)
		{
			NOTICE_LOG(BOOT, "Forcing PAL broadcasting");
			config::Broadcast.override(1);
		}
		if (prod_id == "T1102M"				// Densha de Go! 2
				|| prod_id == "T00000A"		// The Ring of the Nibelungen (demo, hack)
				|| prod_id == "T15124N 00"	// Worms Pinball (prototype)
				|| prod_id == "T9503M"		// Eisei Meijin III
				|| prod_id == "T5202M"		// Marionette Company
				|| prod_id == "T5301M")		// World Neverland Plus
		{
			NOTICE_LOG(BOOT, "Forcing Full Framebuffer Emulation");
			config::EmulateFramebuffer.override(true);
		}
		if (prod_id == "T-8102N")		// TrickStyle (US)
		{
			NOTICE_LOG(BOOT, "Forcing English Language");
			config::Language.override(1);
		}
		if (prod_id == "T-9701N"			// Mortal Kombat (US)
				|| prod_id == "T9701D")		// Mortal Kombat (EU)
		{
			NOTICE_LOG(BOOT, "Disabling Native Depth Interpolation");
			config::NativeDepthInterpolation.override(false);
		}
		// Per-pixel transparent layers
		int layers = 0;
		if (prod_id == "MK-51011"			// Time Stalkers (US)
				|| prod_id == "MK-5101153")	// Time Stalkers (EU)
			layers = 72;
		else if (prod_id == "T13001N"		// Blue Stinger (US)
				|| prod_id == "HDR-0003"	// Blue Stinger (JP)
				|| prod_id == "T13001D-05"	// Blue Stinger (EU)
				|| prod_id == "T13001D 18")	// Blue Stinger (DE)
			layers = 80;
		else if (prod_id == "T2102M"		// Panzer Front
				|| prod_id == "T-8118N"		// Spirit of Speed (US)
				|| prod_id == "T-8117D-50"	// Spirit of Speed (EU)
				|| prod_id == "T13002N"		// Vigilante 8 (US)
				|| prod_id == "T13002D")	// Vigilante 8 (EU)
			layers = 64;
		else if (prod_id == "T2106M")		// L.O.L. Lack of Love
			layers = 48;
		else if (prod_id == "T1212M")		// Gaiamaster - Kessen! Seikioh Densetsu
			layers = 96;
		else if (prod_id == "T-9707N"		// San Francisco Rush 2049 (US)
				|| prod_id == "T-9709D-50"	// San Francisco Rush 2049 (EU)
				|| prod_id == "T17721N"		// Conflict Zone (US)
				|| prod_id == "T46604D")	// Conflict Zone (EU)
			layers = 152;
		else if (prod_id == "MK-51033"		// ECCO the Dolphin (US)
				|| prod_id == "MK-5103350"	// ECCO the Dolphin (EU)
				|| prod_id == "HDR-0103")	// ECCO the Dolphin (JP)
			layers = 96;
		else if (prod_id == "T40203N")		// Draconus: Cult of the Wyrm
			layers = 80;
		else if (prod_id == "T40212N"		// Soldier of Fortune (US)
				|| prod_id == "T17726D 50")	// Soldier of Fortune (EU)
			layers = 86;
		else if (prod_id == "T44102N")		// BANG! Gunship Elite
			layers = 100;
		else if (prod_id == "T12502N"		// MDK 2 (US)
				|| prod_id == "T12501D 50")	// MDK 2 (EU)
			layers = 200;
		else if (prod_id == "T9708D  50")	// Army Men
			layers = 173;
		else if (prod_id == "MK-51038"		// Zombie Revenge (US)
				|| prod_id == "MK-5103850"	// Zombie Revenge (EU)
				|| prod_id == "HDR-0026"	// Zombie Revenge (JP)
				|| prod_id == "36801N"		// Fighting Force 2 (US)
				|| prod_id == "36802D 80"	// Fighting Force 2 (PAL, en-fr)
				|| prod_id == "36802D 18")	// Fighting Force 2 (PAL, de)
			layers = 116;
		else if (prod_id == "T15112N")		// Demolition Racer (US)
			layers = 44;
		else if (prod_id == "T1208N"		// Tech Romancer (US)
				|| prod_id == "T7009D50")	// Tech Romancer (EU)
			layers = 56;
		if (layers != 0) {
			NOTICE_LOG(BOOT, "Forcing %d transparent layers", layers);
			config::PerPixelLayers.override(layers);
		}
		if (prod_id == "HDR-0113"			// Power Smash
				|| prod_id == "HDR-0091")	// Pro Yakyuu Team de Asobou Net!
		{
			NOTICE_LOG(BOOT, "Forcing DCNet use");
			config::UseDCNet.override(true);
		}
	}
	else if (settings.platform.isArcade())
	{
		if (prod_id == "COSMIC SMASH IN JAPAN")
		{
			INFO_LOG(BOOT, "Enabling translucent depth multipass for game %s", prod_id.c_str());
			config::TranslucentPolygonDepthMask.override(true);
		}
		if (prod_id == "BEACH SPIKERS JAPAN"
				|| prod_id == "CHOCO MARKER"
				|| prod_id == "LOVE AND BERRY USA VER1.003"		// lovebero
				|| prod_id == "LOVE AND BERRY USA VER2.000")	// lovebery
		{
			INFO_LOG(BOOT, "Enabling RTT Copy to VRAM for game %s", prod_id.c_str());
			config::RenderToTextureBuffer.override(true);
		}
		if (prod_id == "RADIRGY NOA")
		{
			INFO_LOG(BOOT, "Disabling Free Play for game %s", prod_id.c_str());
			config::ForceFreePlay.override(false);
		}
		if (prod_id == "VIRTUAL-ON ORATORIO TANGRAM") {
			INFO_LOG(BOOT, "Forcing Japan region for game %s", prod_id.c_str());
			config::Region.override(0);
		}
		if (prod_id == "CAPCOM VS SNK PRO  JAPAN")
		{
			INFO_LOG(BOOT, "Enabling Extra depth scaling for game %s", prod_id.c_str());
			config::ExtraDepthScale.override(10000.f);
		}
	}
}

void Emulator::dc_reset(bool hard)
{
	if (hard)
	{
		NetworkHandshake::term();
		memwatch::unprotect();
		memwatch::reset();
	}
	sh4_sched_reset(hard);
	pvr::reset(hard);
	aica::reset(hard);
	getSh4Executor()->Reset(true);
	mem_Reset(hard);
	// MapleCast: VRAM/PVR state about to be wiped — tell clients to resync.
	maplecast_mirror::requestSyncBroadcast();
}

static void setPlatform(int platform)
{
	if (VRAM_SIZE != 0)
		addrspace::unprotectVram(0, VRAM_SIZE);
	elan::ERAM_SIZE = 0;
	switch (platform)
	{
	case DC_PLATFORM_DREAMCAST:
		settings.platform.ram_size = config::RamMod32MB ? 32_MB : 16_MB;
		settings.platform.vram_size = 8_MB;
		settings.platform.aram_size = 2_MB;
		settings.platform.bios_size = 2_MB;
		settings.platform.flash_size = 128_KB;
		break;
	case DC_PLATFORM_NAOMI:
		settings.platform.ram_size = 32_MB;
		settings.platform.vram_size = 16_MB;
		settings.platform.aram_size = 8_MB;
		settings.platform.bios_size = 2_MB;
		settings.platform.flash_size = 32_KB;	// battery-backed ram
		break;
	case DC_PLATFORM_NAOMI2:
		settings.platform.ram_size = 32_MB;
		settings.platform.vram_size = 16_MB; // 2x16 MB VRAM, only 16 emulated
		settings.platform.aram_size = 8_MB;
		settings.platform.bios_size = 2_MB;
		settings.platform.flash_size = 32_KB;	// battery-backed ram
		elan::ERAM_SIZE = 32_MB;
		break;
	case DC_PLATFORM_ATOMISWAVE:
		settings.platform.ram_size = 16_MB;
		settings.platform.vram_size = 8_MB;
		settings.platform.aram_size = 2_MB;
		settings.platform.bios_size = 128_KB;
		settings.platform.flash_size = 128_KB;	// sram
		break;
	case DC_PLATFORM_SYSTEMSP:
		settings.platform.ram_size = 32_MB;
		settings.platform.vram_size = 16_MB;
		settings.platform.aram_size = 8_MB;
		settings.platform.bios_size = 2_MB;
		settings.platform.flash_size = 128_KB;	// sram
		break;
	default:
		die("Unsupported platform");
		break;
	}
	settings.platform.system = platform;
	settings.platform.ram_mask = settings.platform.ram_size - 1;
	settings.platform.vram_mask = settings.platform.vram_size - 1;
	settings.platform.aram_mask = settings.platform.aram_size - 1;
	addrspace::initMappings();
}

void Emulator::init()
{
	if (state != Uninitialized)
	{
		verify(state == Init);
		return;
	}
	// Default platform
	setPlatform(DC_PLATFORM_DREAMCAST);

	libGDR_init();
	pvr::init();
	aica::init();
	mem_Init();
	reios_init();

	// the recompiler may start generating code at this point and needs a fully configured machine
#if FEAT_SHREC != DYNAREC_NONE
	recompiler = Get_Sh4Recompiler();
	recompiler->Init();
	if(config::DynarecEnabled)
		INFO_LOG(DYNAREC, "Using Recompiler");
	else
#endif
		INFO_LOG(INTERPRETER, "Using Interpreter");
	interpreter = Get_Sh4Interpreter();
	interpreter->Init();
	state = Init;
}

Sh4Executor *Emulator::getSh4Executor()
{
#if FEAT_SHREC != DYNAREC_NONE
	static const bool _useInterpreter = std::getenv("MAPLECAST_USE_INTERPRETER") != nullptr;
	if(config::DynarecEnabled && !_useInterpreter)
		return recompiler;
	else
#endif
		return interpreter;
}

int getGamePlatform(const std::string& filename)
{
	if (settings.naomi.slave)
		// Multiboard slave
		return DC_PLATFORM_NAOMI;

	if (filename.empty())
		// Dreamcast BIOS
		return DC_PLATFORM_DREAMCAST;

	std::string extension = get_file_extension(filename);
	if (extension.empty())
		return DC_PLATFORM_DREAMCAST;	// unknown
	if (extension == "zip" || extension == "7z")
		return naomi_cart_GetPlatform(filename.c_str());
	if (extension == "bin" || extension == "dat" || extension == "lst")
		return DC_PLATFORM_NAOMI;

	return DC_PLATFORM_DREAMCAST;
}

void Emulator::loadGame(const char *path, LoadProgress *progress)
{
	init();

	// Mirror client OR state-replica with no ROM: skip ROM/BIOS loading entirely.
	// Hardware is initialized to a blank DC state; the server sends the real game
	// state via WebSocket (SYNC for mirror, MCSV savestate for state-replica).
	const bool isMirrorNoRom  = std::getenv("MAPLECAST_MIRROR_CLIENT")   && (path == nullptr || strlen(path) == 0);
	const bool isReplicaNoRom = std::getenv("MAPLECAST_STATE_REPLICA")   && (path == nullptr || strlen(path) == 0);
	if (isMirrorNoRom || isReplicaNoRom)
	{
		const char* tag = isMirrorNoRom ? "MIRROR" : "state-replica";
		printf("[%s] No ROM — hardware init only, game state arrives from server\n", tag);
		settings.content.path.clear();
		settings.content.fileName.clear();
		settings.content.title = isMirrorNoRom ? "MapleCast Mirror" : "MapleCast Replica";
		setPlatform(DC_PLATFORM_DREAMCAST);
		mem_map_default();
		config::Settings::instance().reset();
		config::Settings::instance().load(false);
		dc_reset(true);
		nvmem::loadHle();
		gdr::initDrive("");

		// Fire Event::Start so gamepad mappings load (load_system_mappings).
		// Without this, input_mapper stays null and all buttons are silently dropped.
		EventManager::event(Event::Start);

		state = Loaded;
		if (isMirrorNoRom) {
			// Mirror client calls start() here; state-replica lets main.cpp call it.
			start();
		}
		return;
	}

	try {
		DEBUG_LOG(BOOT, "Loading game %s", path == nullptr ? "(nil)" : path);

		if (path != nullptr && strlen(path) > 0)
		{
			settings.content.path = path;
			if (settings.naomi.slave) {
				settings.content.fileName = path;
			}
			else
			{
				hostfs::FileInfo info = hostfs::storage().getFileInfo(settings.content.path);
				settings.content.fileName = info.name;
				if (settings.content.title.empty())
					settings.content.title = get_file_basename(info.name);
			}
		}
		else
		{
			settings.content.path.clear();
			settings.content.fileName.clear();
		}

		setPlatform(getGamePlatform(settings.content.fileName));
		mem_map_default();

		config::Settings::instance().reset();
		config::Settings::instance().load(false);
		dc_reset(true);
		memset(&settings.network.md5, 0, sizeof(settings.network.md5));

		if (settings.platform.isConsole())
		{
			if (settings.content.path.empty())
			{
				// Boot BIOS
				if (!nvmem::loadFiles())
					throw FlycastException(strprintf(i18n::T("No BIOS file found in %s"), hostfs::getFlashSavePath("", "").c_str()));
				gdr::initDrive("");
			}
			else
			{
				std::string extension = get_file_extension(settings.content.path);
				if (extension != "elf")
				{
					if (gdr::initDrive(settings.content.path))
					{
						loadGameSpecificSettings();
						printf("[bios-debug] UseReios cfg=%d; trying nvmem::loadFiles()...\n",
							(int)config::UseReios);
						bool filesOk = nvmem::loadFiles();
						printf("[bios-debug] loadFiles() returned %s\n", filesOk ? "true" : "false");
						if (config::UseReios || !filesOk)
						{
							nvmem::loadHle();
							printf("[bios-debug] ── LOADED REIOS HLE (no real BIOS active) ──\n");
							NOTICE_LOG(BOOT, "Did not load BIOS, using reios");
							if (!config::UseReios && config::UseReios.isReadOnly())
								os_notify(i18n::T("This game requires a real BIOS"), 15000);
						}
						else
						{
							printf("[bios-debug] ── REAL DC BIOS LOADED (no REIOS hooks in RAM) ──\n");
						}
					}
					else
					{
						// Content load failed. Boot the BIOS
						settings.content.path.clear();
						if (!nvmem::loadFiles())
							throw FlycastException(i18n::Ts("This media cannot be loaded"));
						gdr::initDrive("");
					}
				}
				else
				{
					// Elf only supported with HLE BIOS
					nvmem::loadHle();
					gdr::initDrive("");
				}
			}
			if (settings.content.path.empty())
				settings.content.title = BIOS_TITLE;

			if (progress)
				progress->progress = 1.0f;
		}
		else if (settings.platform.isArcade())
		{
			nvmem::loadFiles();
			naomi_cart_LoadRom(settings.content.path, settings.content.fileName, progress);
			loadGameSpecificSettings();
			// Reload the BIOS in case a game-specific region is set
			naomi_cart_LoadBios(path);
		}
		if (!settings.naomi.slave)
		{
			mcfg_DestroyDevices();
			mcfg_CreateDevices();
			if (settings.platform.isNaomi())
				// Must be done after the maple devices are created and EEPROM is accessible
				naomi_cart_ConfigureEEPROM();
		}
#ifdef USE_RACHIEVEMENTS
		// RA probably isn't expecting to travel back in the past so disable it
		if (config::GGPOEnable)
			config::EnableAchievements.override(false);
		// Hardcore mode disables all cheats, under/overclocking, load state, lua and forces dynarec on
		settings.raHardcoreMode = config::EnableAchievements && config::AchievementsHardcoreMode
			&& !NaomiNetworkSupported();
#endif
		cheatManager.reset(settings.content.gameId);
		if (cheatManager.isWidescreen())
		{
			os_notify(i18n::T("Widescreen cheat activated"), 2000);
			config::ScreenStretching.override(134);	// 4:3 -> 16:9
		}
		// reload settings so that all settings can be overridden
		loadGameSpecificSettings();
		NetworkHandshake::init();
		settings.input.fastForwardMode = false;
		EventManager::event(Event::Start);
		if (!settings.content.path.empty())
		{
#ifndef LIBRETRO
			// MAPLECAST_REPLAY_IN — open the .mcrec FIRST so its embedded
			// savestate is written to the slot's .state file BEFORE the
			// autoload check below fires dc_loadstate. Force AutoLoadState=on
			// so the state restore actually runs, regardless of cfg.
			bool _replayOpened = false;
			bool _replayRestoredInMemory = false;
			if (const char* inPath = std::getenv("MAPLECAST_REPLAY_IN")) {
				printf("[autoload-debug] MAPLECAST_REPLAY_IN=%s — opening before autoload\n", inPath);
				if (maplecast_replay::openReplay(inPath)
				    && maplecast_replay::loadStartSavestate()) {
					_replayOpened = true;
					const uint32_t fv = maplecast_replay::formatVersion();
					// All currently-supported formats (V5+) restore via the
					// rollback ring's in-memory path; the slot file is never
					// touched. AutoLoadState forced off so a stale slot file
					// can't overwrite the in-memory restore.
					_replayRestoredInMemory = true;
					config::AutoLoadState.override(false);
					printf("[autoload-debug] MAPLECAST_REPLAY_IN — V%u in-memory restore done, AutoLoadState disabled\n", fv);

					// Optional seek-to-frame: replaces the slot's .state with
					// the nearest sidecar checkpoint at or before this frame.
					// Lets long replays start mid-match instead of playing
					// through leading idle. Falls through silently if the
					// sidecar isn't present or the target is before the
					// first checkpoint.
					if (const char* seekStr = std::getenv("MAPLECAST_REPLAY_SEEK")) {
						uint64_t seekFrame = (uint64_t)std::strtoull(seekStr, nullptr, 10);
						printf("[autoload-debug] MAPLECAST_REPLAY_SEEK=%llu — attempting checkpoint seek\n",
						       (unsigned long long)seekFrame);
						maplecast_replay::seekToFrame(seekFrame);
					}
				} else {
					printf("[autoload-debug] MAPLECAST_REPLAY_IN — open or savestate-write failed\n");
				}
			}
			// Diagnostic: we want to know whether auto-load is actually firing in
			// headless mode, since savestate auto-load is the workaround for the
			// MVC2 attract-mode SH4 reset crash.
			//
			// ── CONSTRAINT (root-caused 2026-07-06): the autoload savestates were
			// captured under REIOS (HLE BIOS). Their low-RAM BIOS work area is
			// 0xFF filler and their syscall vectors point at reios trampolines
			// (8C0000B0.. → 8C001000/02/04/06/08 — verified live via control-WS
			// ram_read). The current rig boots the REAL DC BIOS, so ANY guest
			// entry into the BIOS reset path (attract-mode reboot, or the
			// A+B+X+Y+Start soft-reset chord) executes the 0xFF filler at
			// RAM+0x10 via P2 → illegal opcode 0xFFFF with SR.BL=1 →
			//   [SH4-FAULT] BLOCKED exception expEvn=0x180 epc=AC000010
			//   → "Fatal: SH4 exception when blocked", unrecoverable crash-loop.
			// Consequences:
			//   1. Autoload MUST succeed (dc_loadstate fails SILENTLY — void
			//      return, WARN_LOG only) and the guest must NEVER reach the
			//      reset path. The input server neutralizes the soft-reset
			//      chord (maplecast_input_server.cpp updateSlot,
			//      MAPLECAST_ALLOW_SOFT_RESET=1 opts out).
			//   2. A "boot crashes without MAPLECAST_REPLICA_LIVE" report
			//      (2026-07-06) was A/B-tested and did NOT reproduce: boots are
			//      healthy with and without REPLICA_LIVE. The observed fault had
			//      this exact signature and is input-triggered (a stray
			//      synthetic-input stream on udp:7100 during the A/B), not an
			//      init-order dependency.
			//   3. Long-term fix if the reset path is ever needed: re-capture
			//      the training savestate from a REAL-BIOS boot chain.
			printf("[autoload-debug] path='%s' GGPO=%d AutoLoad=%d NaomiNet=%d multiboard=%d slot=%d\n",
				settings.content.path.c_str(),
				(int)config::GGPOEnable,
				(int)config::AutoLoadState,
				(int)NaomiNetworkSupported(),
				(int)settings.naomi.multiboard,
				(int)config::SavestateSlot);
			if (config::GGPOEnable)
				dc_loadstate(-1);
			else if (config::AutoLoadState && !NaomiNetworkSupported() && !settings.naomi.multiboard)
				dc_loadstate(config::SavestateSlot);
			// Headless override: even if AutoLoadState is off, auto-load on
			// MAPLECAST_HEADLESS_AUTOLOAD=1 so the operator can opt in via env
			// without fighting the config file.
			else if (std::getenv("MAPLECAST_HEADLESS_AUTOLOAD")) {
				printf("[autoload-debug] MAPLECAST_HEADLESS_AUTOLOAD=1 — forcing load\n");
				dc_loadstate(config::SavestateSlot);
			}

			// MAPLECAST_REPLAY_IN — activate playback now that dc_loadstate
			// above has restored the embedded savestate. The actual openReplay
			// + loadStartSavestate ran BEFORE the autoload check above so the
			// state file was on disk when dc_loadstate fired.
			if (_replayOpened) {
				double speed = 1.0;
				if (const char* s = std::getenv("MAPLECAST_REPLAY_SPEED"))
					speed = atof(s);
				maplecast_replay::startPlayback(speed);
			}

			// MAPLECAST_REPLAY_OUT recording start. Capturing the savestate
			// here — same lifecycle moment as MAPLECAST_REPLAY_IN restores
			// it — guarantees record/replay state alignment. Capturing
			// earlier (input_server::init) made frame 1 differ between
			// record and replay due to intermediate init-state drift.
			//
			// Hotkey-trigger path (control-WS record_start): the WS
			// endpoint dc_savestate'd the user's mid-game state to slot
			// REPLAY_SLOT, set NextRecordPath, then emu.stop+start. We
			// consume the path here. The autoload's dc_loadstate above
			// restored from slot 0 (the configured autoload slot), so we
			// re-load from REPLAY_SLOT to put SH4 back at the user's
			// mid-game position before recording activates. This
			// preserves the V2 invariant: dc_loadstate fires before
			// recording, at the autoload lifecycle boundary.
			std::string outPath;
			bool fromHotkey = false;
			if (maplecast_replay::hasNextRecordPath()) {
				outPath = maplecast_replay::consumeNextRecordPath();
				fromHotkey = true;
				printf("[autoload-debug] hotkey record start — restoring slot %d (mid-game state) before recording\n",
				       99 /* MAPLECAST_REPLAY_SLOT */);
				dc_loadstate(99);
			} else if (const char* envPath = std::getenv("MAPLECAST_REPLAY_OUT")) {
				outPath = envPath;
				printf("[autoload-debug] MAPLECAST_REPLAY_OUT — starting recording at autoload point\n");
			}
			if (!outPath.empty()) {
				maplecast_replay::StartParams sp;
				sp.out_path = outPath;
				if (const char* p1 = std::getenv("MAPLECAST_REPLAY_P1_NAME"))    sp.p1_name = p1;
				if (const char* p2 = std::getenv("MAPLECAST_REPLAY_P2_NAME"))    sp.p2_name = p2;
				if (const char* sid = std::getenv("MAPLECAST_REPLAY_SERVER_ID")) sp.server_id = sid;
				if (const char* rh = std::getenv("MAPLECAST_REPLAY_ROM_HASH"))   sp.rom_hash_hex = rh;
				maplecast_replay::start(sp);
				if (fromHotkey)
					printf("[replay] hotkey-triggered recording active — output: %s\n", outPath.c_str());
			}

			// Per-match continuous recording (server-side automatic
			// capture with retention). Independent of MAPLECAST_REPLAY_OUT
			// above — both can coexist if for some reason an operator
			// wants both. See replay_writer.h::initMatchRecording.
			if (std::getenv("MAPLECAST_RECORD_MATCHES")) {
				std::string recDir;
				if (const char* d = std::getenv("MAPLECAST_RECORDINGS_DIR"))
					recDir = d;
				if (recDir.empty()) {
					// Dev default: <repo>/recordings/. The headless build's
					// cwd is typically the repo root or the build dir, so
					// fall back to a relative path that will resolve under
					// the binary's working directory.
					recDir = "recordings";
				}
				int retentionDays = 7;
				if (const char* r = std::getenv("MAPLECAST_RECORD_RETENTION_DAYS"))
					retentionDays = std::atoi(r);
				maplecast_replay::initMatchRecording(recDir, retentionDays);
			}
#endif
		}

		if (progress)
		{
#ifdef GDB_SERVER
			if (config::GDB && config::GDBWaitForConnection)
				progress->label = "Waiting for debugger...";
			else
#endif
				progress->label = i18n::T("Starting...");
		}

		state = Loaded;
	} catch (...) {
		state = Error;
		throw;
	}
}

void Emulator::runInternal()
{
	runner.init();

	try {
		if (singleStep)
		{
			getSh4Executor()->Step();
			singleStep = false;
		}
		else if (stepRangeTo != 0)
		{
			while (Sh4cntx.pc >= stepRangeFrom && Sh4cntx.pc < stepRangeTo)
				getSh4Executor()->Step();

			stepRangeFrom = 0;
			stepRangeTo = 0;
		}
		else
		{
			do {
				resetRequested = false;

				getSh4Executor()->Run();

				if (resetRequested)
				{
					nvmem::saveFiles();
					dc_reset(false);
					if (!restartCpu())
						resetRequested = false;
				}
			} while (resetRequested);
		}
	} catch (...) {
		runner.term();
		throw;
	}
}

void Emulator::unloadGame()
{
	try {
		stop();
	} catch (...) { }
	if (state == Loaded || state == Error)
	{
#ifndef LIBRETRO
		if (state == Loaded && config::AutoSaveState && !settings.content.path.empty()
				&& !settings.naomi.multiboard && !config::GGPOEnable && !NaomiNetworkSupported())
			gui_saveState(false);
#endif
		try {
			dc_reset(true);
		} catch (const FlycastException& e) {
			ERROR_LOG(COMMON, "%s", e.what());
		}
		// Flush the VMU files to disk
		mcfg_DestroyDevices(true);
		config::Settings::instance().reset();
		config::Settings::instance().load(false);
		settings.content.path.clear();
		settings.content.gameId.clear();
		settings.content.fileName.clear();
		settings.content.title.clear();
		settings.platform.system = DC_PLATFORM_DREAMCAST;
		custom_texture.terminate();
		state = Init;
		EventManager::event(Event::Terminate);
	}
}

void Emulator::term()
{
	unloadGame();
	// Stop the gated render-replica-live WS thread (no-op when it was never armed).
	maplecast_replica_live::shutdown();
	if (state == Init)
	{
		debugger::term();
		if (interpreter != nullptr)
		{
			interpreter->Term();
			delete interpreter;
			interpreter = nullptr;
		}
		if (recompiler != nullptr)
		{
			recompiler->Term();
			delete recompiler;
			recompiler = nullptr;
		}
		custom_texture.terminate();	// lr: avoid deadlock on exit (win32)
		reios_term();
		aica::term();
		pvr::term();
		mem_Term();
		libGDR_term();
		ice::term();

		state = Terminated;
	}
	addrspace::release();
}

void Emulator::stop()
{
	if (state != Running)
		return;
	// Avoid race condition with GGPO restarting the sh4 for a new frame
	if (config::GGPOEnable)
		NetworkHandshake::term();
	{
		const std::lock_guard<std::mutex> _(mutex);
		// must be updated after GGPO is stopped since it may run some rollback frames
		state = Loaded;
		getSh4Executor()->Stop();
	}
	if (config::ThreadedRendering)
	{
		rend_cancel_emu_wait();
		try {
			checkStatus(true);
		} catch (const FlycastException& e) {
			WARN_LOG(COMMON, "%s", e.what());
			throw e;
		}
		nvmem::saveFiles();
		EventManager::event(Event::Pause);
	}
	else
	{
#ifdef __ANDROID__
		// defer stopping audio until after the current frame is finished
		// normally only useful on android due to multithreading
		stopRequested = true;
#else
		TermAudio();
		nvmem::saveFiles();
		EventManager::event(Event::Pause);
#endif
	}
}

// Called on the emulator thread for soft reset
void Emulator::requestReset()
{
	resetRequested = true;
	if (config::GGPOEnable)
		NetworkHandshake::term();
	getSh4Executor()->Stop();
	// MapleCast: A+B+X+Y+Start writes SB_SFRES=0x7611 → soft reset → here.
	// Tell the mirror server to push a fresh SYNC so all browser clients
	// reset their renderer state instead of trying to limp along with stale
	// post-reset VRAM/PVR.
	maplecast_mirror::requestSyncBroadcast();
}

void loadGameSpecificSettings()
{
	if (settings.platform.isConsole())
	{
		reios_disk_id();
		settings.content.gameId = trim_trailing_ws(std::string(ip_meta.product_number, sizeof(ip_meta.product_number)));
		// in case there is a null character followed by garbage, which happens
		settings.content.gameId = settings.content.gameId.c_str();

		if (settings.content.gameId.empty())
			return;
	}

	// Default per-game settings
	loadSpecialSettings();

	config::Settings::instance().setGameId(settings.content.gameId);
	custom_texture.init();

	// Reload per-game settings
	config::Settings::instance().load(true);

	if (config::GGPOEnable || settings.raHardcoreMode)
		config::Sh4Clock.override(200);
	if (settings.raHardcoreMode)
	{
		config::WidescreenGameHacks.override(false);
		config::DynarecEnabled.override(true);
	}
}

void Emulator::step()
{
	// FIXME single thread is better
	singleStep = true;
	start();
	stop();
}

void Emulator::stepRange(u32 from, u32 to)
{
	stepRangeFrom = from;
	stepRangeTo = to;
	start();
	stop();
}

void Emulator::loadstate(Deserializer& deser, bool lightweight)
{
	// A2 run-ahead: LIGHTWEIGHT restore skips the dynarec/texture flushes. The rewind goes back
	// exactly 1 frame, so the SH4 blocks compiled during this tick and the decoded textures are
	// STILL VALID (identical code/VRAM); ResetCache+bm_Reset every tick emptied the JIT cache and
	// the NEXT (hidden) leg re-compiled every block cold = the 80ms/tick thief (profiler 2026-07-12).
	// Genuine self-modifying code is still caught by the dynarec's own SMC page-write invalidation.
	if (!lightweight)
	{
		if (!custom_texture.preloaded())
		{
			custom_texture.terminate();
			custom_texture.init();
		}
#if FEAT_AREC == DYNAREC_JIT
		aica::arm::recompiler::flush();
#endif
	}
	mmu_flush_table();
#if FEAT_SHREC != DYNAREC_NONE
	if (!lightweight)
		bm_Reset();
#endif
	memwatch::unprotect();
	memwatch::reset();

	dc_deserialize(deser);

	mmu_set_state();
	if (!lightweight)
		getSh4Executor()->ResetCache();
	EventManager::event(Event::LoadState);
}

void Emulator::setNetworkState(bool online)
{
	if (settings.network.online != online)
	{
		settings.network.online = online;
		DEBUG_LOG(NETWORK, "Network state %d", online);
		if (online && settings.platform.isConsole()
				&& config::Sh4Clock != 200)
		{
			config::Sh4Clock.override(200);
			getSh4Executor()->ResetCache();
		}
		EventManager::event(Event::Network);
	}
	settings.input.fastForwardMode &= !online;
}

void EventManager::registerEvent(Event event, Callback callback, void *param)
{
	unregisterEvent(event, callback, param);
	auto& vector = callbacks[static_cast<size_t>(event)];
	vector.push_back(std::make_pair(callback, param));
}

void EventManager::unregisterEvent(Event event, Callback callback, void *param)
{
	auto& vector = callbacks[static_cast<size_t>(event)];
	auto it = std::find(vector.begin(), vector.end(), std::make_pair(callback, param));
	if (it != vector.end())
		vector.erase(it);
}

void EventManager::broadcastEvent(Event event)
{
	auto& vector = callbacks[static_cast<size_t>(event)];
	for (auto& pair : vector)
		pair.first(event, pair.second);
}

void Emulator::run()
{
	verify(state == Running);
	// Replica / player client frame gate (non-threaded path). See the
	// threaded path in start() for the full explanation. The two are
	// mutually exclusive by env-var contract.
	if (maplecast_replica::active())
	{
		while (state == Running && !maplecast_replica::frameGate())
			std::this_thread::sleep_for(std::chrono::microseconds(250));
		if (state != Running) return;
	}
	else if (maplecast_player::active())
	{
		while (state == Running && !maplecast_player::frameGate())
			std::this_thread::sleep_for(std::chrono::microseconds(250));
		if (state != Running) return;
	}
	// State-replica injection: inject the server's GSTA into RAM before the
	// SH4 frame runs (non-threaded path). See the threaded path for details.
	if (maplecast_state_replica::active())
	{
		if (!maplecast_state_replica::frameInject()) {
			// No-ROM MCSV wait: yield to the render loop for one frame so the
			// window stays responsive while the SH4 is held idle. Return here
			// so runInternal() is NOT called (SH4 would fire reios_boot without
			// a GDI and throw "Failed to locate bootfile").
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
			return;
		}
	}
	startTime = sh4_sched_now64();
	renderTimeout = false;
	if (!singleStep && stepRangeTo == 0)
	getSh4Executor()->Start();
	try {
		// === A2 RUN-AHEAD depth=1 (MAPLECAST_RUNAHEAD=1, default OFF; kill-list 2026-07-12) ===
		// MVC2's internal input lag is exactly 1 frame (measured), so the PREVIEW frame T+1's
		// pixels are fully determined by the input latched in frame T — publishing T+1 and
		// rewinding is mispredict-free by construction. Cycle: hidden authoritative T ->
		// rollback-ring save -> preview T+1 (publishes) -> ring rewind to T. Save/rewind reuse
		// the byte-exact F.2-audited rollback machinery (NOT naive dc_deserialize — misses 8
		// resets; NOT loadstate/frame — dynarec flush storm). INCOMPATIBLE with .mcrec record /
		// lockstep tape consumers (1-frame-per-tick assumption) — operator flag, default OFF.
		static const bool _raWant = [](){ const char* e = std::getenv("MAPLECAST_RUNAHEAD");
			return e && e[0] && e[0] != '0'; }();
		static bool _raReady = false, _raTried = false;
		if (_raWant && !_raTried) {
			_raTried = true;
			_raReady = maplecast_rollback::init();
			printf("[RUNAHEAD] %s\n", _raReady ? "ARMED depth=1 (preview publish + ring rewind)"
			                                   : "rollback init FAILED - disabled");
			fflush(stdout);
		}
		if (_raReady && maplecast_mirror::isServer()) {
			extern std::atomic<bool> mc_runaheadPreviewLeg;
			static uint64_t _raF = 0;
			_raF++;
			maplecast_mirror::setSuppressPublish(true);
			runInternal();                                   // authoritative frame T (hidden; ctx recycles at QueueRender)
			maplecast_mirror::setSuppressPublish(false);
			maplecast_rollback::saveFrame(_raF);
			mc_runaheadPreviewLeg.store(true, std::memory_order_relaxed);
			startTime = sh4_sched_now64();
			renderTimeout = false;
			try {
				runInternal();                               // preview T+1 (publishes)
			} catch (...) {
				mc_runaheadPreviewLeg.store(false, std::memory_order_relaxed);
				throw;
			}
			mc_runaheadPreviewLeg.store(false, std::memory_order_relaxed);
			if (!maplecast_rollback::rewindToFrame(_raF)) {
				// Rewind failed: authoritative track is now T+1 (the preview). Continuity is
				// preserved (it WAS a valid frame) but disable run-ahead — don't compound.
				printf("[RUNAHEAD] rewind FAILED at frame %llu - disabling\n",
					(unsigned long long)_raF);
				fflush(stdout);
				_raReady = false;
			}
		} else {
			runInternal();
		}
		if (ggpo::active())
			ggpo::nextFrame();
	} catch (const std::exception& e) {
		ERROR_LOG(COMMON, "Exception: %s", e.what());
		setNetworkState(false);
		state = Error;
		getSh4Executor()->Stop();
		EventManager::event(Event::Pause);
		throw;
	}
}

void Emulator::start()
{
	if (state == Running)
		return;
	if (state != Loaded) {
		WARN_LOG(COMMON, "Unexpected emu state %d", state);
		return;
	}
	state = Running;
	SetMemoryHandlers();

	// Direct-input bypass: skip SDL's gamepad event queue (~1-3ms savings).
	// Linux: evdev direct read on a SCHED_FIFO thread.
	// Windows: XInput direct-poll on a TIME_CRITICAL thread.
	// Both opt-in via env (MAPLECAST_EVDEV_INPUT / MAPLECAST_RAWINPUT).
#ifdef __linux__
	maplecast_evdev_input::init();
#elif defined(_WIN32)
	maplecast_rawinput::init();
#endif

	// Arcade mode: mimic the Naomi cabinet's zero-buffer display path.
	// VSync OFF + SwapInterval(0) = immediate present, no frame buffering.
	// Matches the CRT behavior where scanlines are visible as they're drawn.
	if (std::getenv("MAPLECAST_ARCADE_MODE") || std::getenv("MAPLECAST_LOW_LATENCY")) {
		config::VSync.override(false);
		printf("[arcade-mode] VSync OFF, SwapInterval(0) — zero display buffer latency\n");
		fflush(stdout);
	}

	// SH4Recomp: dump fully loaded SH4 memory to disk
	if (const char* dump_dir = getenv("SH4RECOMP_DUMP"))
	{
		char path[512];
		snprintf(path, sizeof(path), "%s/sh4_ram_16mb.bin", dump_dir);
		FILE* f = fopen(path, "wb");
		if (f) {
			fwrite(&mem_b[0], 1, 16 * 1024 * 1024, f);
			fclose(f);
			printf("[sh4recomp] Dumped 16MB RAM → %s\n", path);
		}
		snprintf(path, sizeof(path), "%s/sh4_vram_8mb.bin", dump_dir);
		f = fopen(path, "wb");
		if (f) {
			extern RamRegion vram;
			fwrite(&vram[0], 1, 8 * 1024 * 1024, f);
			fclose(f);
			printf("[sh4recomp] Dumped 8MB VRAM → %s\n", path);
		}
		snprintf(path, sizeof(path), "%s/dump_info.txt", dump_dir);
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "entry_point=0x0c021000\n");
			fprintf(f, "ram_base=0x0c000000\n");
			fprintf(f, "ram_size=0x01000000\n");
			fprintf(f, "vram_base=0x04000000\n");
			fprintf(f, "vram_size=0x00800000\n");
			fclose(f);
			printf("[sh4recomp] Metadata → %s\n", path);
		}
		fflush(stdout);
	}

	// MapleCast server stack
	if (std::getenv("MAPLECAST"))
	{
		int port = 7100;
		const char* portEnv = std::getenv("MAPLECAST_PORT");
		if (portEnv) port = std::atoi(portEnv);

		// Local input_server (UDP listener on 7100/7101 + browser-WS bridge).
		// Required on the production server. Optional on a desktop mirror
		// client — it's only useful if the user has a hardware NOBD stick
		// on their LAN sending UDP, or wants browsers to connect to their
		// machine. Most desktop clients don't need it; skipping saves a
		// listener, a tape publisher, and the Windows Firewall prompt on
		// first launch.
		const bool skipLocalServer = std::getenv("MAPLECAST_CLIENT_NO_LOCAL_SERVER") != nullptr;
		if (!skipLocalServer) {
			maplecast_input::init(port);
		} else {
			printf("[maplecast] MAPLECAST_CLIENT_NO_LOCAL_SERVER=1 — skipping local input_server (no UDP:%d listener, no tape publisher)\n", port);
			fflush(stdout);
		}
		maplecast_audio::init();
		maplecast_telemetry::init();

#if !defined(MAPLECAST_HEADLESS_BUILD) && !defined(MAPLECAST_CLIENT_ONLY_BUILD)
		if (std::getenv("MAPLECAST_STREAM"))
		{
			int streamPort = 7200;
			const char* sp = std::getenv("MAPLECAST_STREAM_PORT");
			if (sp) streamPort = std::atoi(sp);
			maplecast_stream::init(streamPort);
		}
#endif
	}

	if (config::GGPOEnable && config::ThreadedRendering)
		config::EmulateFramebuffer.override(false);
	setupPtyPipe();

	// Mirror server: captures TA commands + memory diffs to shared memory + WebSocket
	if (std::getenv("MAPLECAST_MIRROR_SERVER"))
	{
		maplecast_mirror::initServer();

		// Phase 1 A.4 rollback ring — SHELVED 2026-05-09. See
		// docs/ROLLBACK-SHELVED.md for the architectural decision: GGPO-
		// style rollback doesn't fit MapleCast's central-server model
		// (single SH4 = no desync by design; 10ms E2E latency already
		// below human-perception threshold). The code is kept intact
		// and opt-in; the ring + predictor only init when the env var
		// is set, which is reserved for: F.2 audit-determinism CI gate,
		// future re-enablement if we pivot to client-side predictor.
		if (std::getenv("MAPLECAST_ROLLBACK_RING")) {
			printf("[rollback] WARNING: MAPLECAST_ROLLBACK_RING=1 set — this is SHELVED experimental work, not production. See docs/ROLLBACK-SHELVED.md.\n");
			fflush(stdout);
			if (!maplecast_rollback::init())
				printf("[rollback] init failed — ring disabled for this session\n");
			// A.5 — comparator + rollback trigger. Same gate: only
			// initializes when the ring is available, since the
			// predictor's mismatch path requires a working rollback.
			maplecast_predictor::init();
		}

		// Control WebSocket — loopback-bound JSON command channel for
		// /overlord admin operations (savestate hot-load, soft reset,
		// status query). Same lifecycle as the mirror server because
		// it manipulates the same emulator state.
		// See docs/WORKSTREAM-OVERLORD.md Phase A.
		int controlPort = 7211;
		if (const char* cpEnv = std::getenv("MAPLECAST_CONTROL_PORT"))
			controlPort = std::atoi(cpEnv);
		maplecast_control_ws::init(controlPort);

		// Render-Replica LIVE feed (Phase 4c) — GATED, READ-ONLY loopback WS that
		// streams the MCRR render read-set so a browser can drive the off-SH4
		// render_frame() on the live game (SH4 stays authoritative). Self-gates on
		// env MAPLECAST_REPLICA_LIVE: unset ⇒ no thread, no capture, zero overhead
		// (byte-identical to today's prod binary). Loopback :7212 (override
		// MAPLECAST_REPLICA_LIVE_PORT); nginx fronts a wss path. The per-frame
		// capture piggybacks the existing determinism-safe oracle hook
		// (mc_oracle_charPassCapture). See docs/RENDER-REPLICA-RECORDING-FORMAT.md.
		maplecast_replica_live::init();
	}

	// Replica client (MAPLECAST_REPLICA): GGPO-spectator-style replay
	// engine. Receives authoritative per-frame inputs from the headless
	// server's tape, runs the local SH4 in lockstep, uses the existing
	// state-sync STAT envelope to bootstrap mid-match. Catchup loop steps
	// the SH4 with rendering disabled, the same trick GGPO uses for
	// rollback fast-forward. See core/network/maplecast_replica.cpp.
	//
	// MUTUALLY EXCLUSIVE with the SHELVED MAPLECAST_PLAYER_CLIENT below
	// (whose frameGate has a desync bug — it fast-forwards a counter
	// without running the SH4, which diverges the local state from the
	// server's). Replica wins if both are set.
	const bool replicaActive = maplecast_replica::init();

	// State-replica client (MAPLECAST_STATE_REPLICA): server-authoritative
	// state INJECTION. Runs the full local SH4 + renderer (this is a NORMAL
	// render build, not MAPLECAST_HEADLESS), loads the same savestate, and each
	// frame injects the server's GSTA into RAM before the SH4 frame runs — so
	// the game's own draw code renders the server's truth. NOT lockstep, NOT
	// the tape-replay maplecast_replica above. See maplecast_state_replica.h.
	const bool stateReplicaActive = maplecast_state_replica::init();
	if (stateReplicaActive)
	{
		// Run SINGLE-THREADED, exactly like the mirror client (below). The
		// threaded path spawns an async Flycast-emu thread inside start() that
		// races the SH4 recompiler / addrspace (p_sh4rcb) init still completing
		// on the main thread → bm_GetCode null-derefs p_sh4rcb on the very first
		// frame. Single-threaded runs run() synchronously from mainui_loop's
		// emu.render() AFTER all init + renderer setup is done — no race. (The
		// full-mirror build accidentally avoided the crash only because its
		// SYNC-wait stalled the first frame ~1s, letting init finish.)
		config::ThreadedRendering.override(false);
		printf("[state-replica] ThreadedRendering=off — single-threaded run loop (no emu-thread init race)\n");

		// Wire input sink — send local gamepad to the server's input port (7100).
		// FREEZE keeps the local SH4 from simulating (it only renders injected state),
		// so inputs are only meaningful server-side. Host is embedded in
		// MAPLECAST_STATE_REPLICA ("host:port"); strip the port suffix for the sink.
		if (!std::getenv("MAPLECAST_SPECTATE")) {
			std::string sinkHost = std::getenv("MAPLECAST_STATE_REPLICA");
			if (auto c = sinkHost.find(':'); c != std::string::npos) sinkHost.resize(c);
			int sinkSlot = std::getenv("MAPLECAST_PLAYER_SLOT") ? std::atoi(std::getenv("MAPLECAST_PLAYER_SLOT")) : 0;
			maplecast_input_sink::init(sinkHost.c_str(), sinkSlot);
#ifdef _WIN32
			maplecast_rawinput::init();
#elif defined(__linux__)
			maplecast_evdev_input::init();
#endif
			printf("[state-replica] input pipeline wired: sink → %s slot=%d\n", sinkHost.c_str(), sinkSlot);
		}
	}
	(void)stateReplicaActive;

	// Replica needs the gamepad → server input pipeline wired (the local
	// SH4 doesn't read its own gamepad; inputs come from the server's
	// authoritative tape after the round-trip). The MAPLECAST_MIRROR_CLIENT
	// block below wires this, but replica intentionally skips that block
	// (replica renders from local SH4, mirror-client renders from wire TA).
	// So we wire input_sink + raw-input directly here for replica mode.
	if (replicaActive)
	{
		// Perf overrides for replica mode. Local SH4 + renderer + audio is
		// CPU-heavy, especially on Windows. DSP audio in particular runs
		// on the AICA thread and competes with the SH4. Disable for replica
		// (server's audio still streams via maplecast_audio if connected).
		// MAPLECAST_REPLICA_KEEP_DSP=1 opt-in for users who want full DSP.
		if (!std::getenv("MAPLECAST_REPLICA_KEEP_DSP")) {
			config::DSPEnabled.override(false);
			printf("[replica] perf: DSPEnabled=off (set MAPLECAST_REPLICA_KEEP_DSP=1 to keep)\n");
		}

		const char* sinkHost = std::getenv("MAPLECAST_REPLICA");
		// Strip optional :port suffix — input sink wants bare host.
		std::string hostOnly = sinkHost ? sinkHost : "127.0.0.1";
		auto colon = hostOnly.find(':');
		if (colon != std::string::npos) hostOnly.resize(colon);

		int sinkSlot = 0;
		if (const char* s = std::getenv("MAPLECAST_PLAYER_SLOT"))
			sinkSlot = std::atoi(s);
		maplecast_input_sink::init(hostOnly.c_str(), sinkSlot);

#ifdef __linux__
		maplecast_evdev_input::init();
#elif defined(_WIN32)
		maplecast_rawinput::init();
#endif
		printf("[replica] input pipeline wired: sink → %s slot=%d\n",
		       hostOnly.c_str(), sinkSlot);
	}

	// SHELVED player client: runs full SH4 locally, but inputs are
	// replayed from the server's frame-stamped tape via the bespoke
	// frameGate path. Kept compiled for diagnostic comparison; do NOT
	// add features here. Safe to init before mirror client detection
	// because the env vars are mutually exclusive by design.
	if (!replicaActive)
		maplecast_player::init();

	// Mirror client: receives TA deltas, renders only (no CPU)
	if (std::getenv("MAPLECAST_MIRROR_CLIENT"))
	{
		// Client-optimized defaults — renderer only, no CPU, no game logic.
		// Do NOT override rendering features (fog, modifier volumes, mipmaps)
		// — they must match the server's output for visual fidelity.
		config::MaxThreads.override(std::max(1, (int)std::thread::hardware_concurrency() / 2));
		config::DSPEnabled.override(false);
		config::ThreadedRendering.override(false);  // client render loop is single-threaded

		// Force the PulseAudio backend on Linux mirror clients. SDL2's
		// "auto" path prefers the sdl2 backend, which on a PipeWire host
		// opens its own 44.1 → 48 kHz stateless resampler (SDL_ConvertAudio),
		// producing audible crackle at callback boundaries. The "pulse"
		// backend goes straight to PipeWire's native stateful resampler
		// and sounds clean. MAPLECAST_AUDIO_BACKEND env var overrides this
		// for users who prefer alsa / libao / sdl2 explicitly. (Note: the
		// backend slug is literally "pulse" — not "pulseaudio" — see the
		// ctor at audiobackend_pulseaudio.cpp:35.)
#if defined(USE_PULSEAUDIO) && !defined(_WIN32) && !defined(__APPLE__)
		if (const char* ab = std::getenv("MAPLECAST_AUDIO_BACKEND"))
			config::AudioBackend.override(ab);
		else
			config::AudioBackend.override("pulse");
		printf("[MIRROR] client audio backend → %s\n", config::AudioBackend.get().c_str());
#endif

		maplecast_mirror::initClient();

		// Control WS for the settings HTML page (same as the server's
		// overlord channel, but on the local machine for client settings).
		{
			// PORT-COLLISION FIX: the native GSTA client is commonly co-located with a headless
			// server on the SAME box, and the headless ALSO binds a control WS on :7211 (it starts
			// first, so it OWNS 7211 -> the client's init(7211) fails to bind and the browser panel
			// ends up talking to the HEADLESS control WS, which has no hud_get/hud_set -> "unknown
			// command"). So a GSTA client defaults its control WS to :7221 (override with
			// MAPLECAST_CONTROL_PORT). Plain (non-GSTA) mirror clients keep :7211.
			bool isGsta = std::getenv("MAPLECAST_GSTA_CLIENT")
			           && std::getenv("MAPLECAST_GSTA_CLIENT")[0]
			           && std::getenv("MAPLECAST_GSTA_CLIENT")[0] != '0';
			int controlPort = isGsta ? 7221 : 7211;
			if (const char* cp = std::getenv("MAPLECAST_CONTROL_PORT"))
				controlPort = std::atoi(cp);
			if (maplecast_control_ws::init(controlPort)) {
				if (isGsta)
					printf("[GSTA] render-debug panel: open web/gsta-render-debug.html?port=%d  (ws://localhost:%d)\n",
						controlPort, controlPort);
				else
					printf("[MIRROR] settings page: file://%s/web/client-settings.html?port=%d\n",
						std::getenv("PWD") ? std::getenv("PWD") : ".", controlPort);
			}
		}

		// Input sink: send local gamepad events to the server.
		// Reads MAPLECAST_SERVER_HOST for the target (same as mirror client).
		// initClientWebSocket may have setenv()'d this from hub discovery,
		// so we read AFTER that runs.
		//
		// Phase 6: if MAPLECAST_SPECTATE=1, skip the input sink entirely —
		// this is a read-only spectator (no gamepad routed to the server).
		// Useful for tournament broadcasters + multi-match grid viewing.
		if (!std::getenv("MAPLECAST_SPECTATE"))
		{
			const char* sinkHost = std::getenv("MAPLECAST_SERVER_HOST");
			if (!sinkHost) sinkHost = "127.0.0.1";
			int sinkSlot = 0;
			if (const char* s = std::getenv("MAPLECAST_PLAYER_SLOT"))
				sinkSlot = std::atoi(s);
			maplecast_input_sink::init(sinkHost, sinkSlot);

			// Direct gamepad bypass — Linux: evdev. Windows: XInput.
			// ~1-3ms lower latency than SDL's poll thread + event queue.
			// Enable via MAPLECAST_EVDEV_INPUT / MAPLECAST_RAWINPUT.
#ifdef __linux__
			maplecast_evdev_input::init();
#elif defined(_WIN32)
			maplecast_rawinput::init();
#endif

			// Phase 2: if hub discovery picked a runner-up server, wire it
			// up as the hot-standby for input failover. The input sink
			// keeps a UDP socket open to it; if the primary stops echoing
			// for >100ms, sends instantly swap to the standby.
			const std::string& backup = maplecast_mirror::clientBackupServerHost();
			if (!backup.empty()) {
				maplecast_input_sink::setBackupServer(backup.c_str());
			}
		}
		else
		{
			printf("[MIRROR] === SPECTATE MODE === read-only, no gamepad forwarded\n");
		}

		state = Loaded;
		printf("[MIRROR] === CLIENT MODE === CPU stopped, renderer-only, %d texture threads\n",
			(int)config::MaxThreads);
	}

	// NATIVE GSTA CLIENT (MAPLECAST_GSTA_CLIENT=1): it does NOT set MAPLECAST_MIRROR_CLIENT
	// (it runs its own initGstaClient via maplecast_mirror::initClient), so the client-side
	// control WS above never started. Start it here so the browser render-debug panel
	// (web/gsta-render-debug.html) can drive the live gsta_render_debug globals over :7211.
	if (std::getenv("MAPLECAST_GSTA_CLIENT")
	    && std::getenv("MAPLECAST_GSTA_CLIENT")[0]
	    && std::getenv("MAPLECAST_GSTA_CLIENT")[0] != '0'
	    && !std::getenv("MAPLECAST_MIRROR_CLIENT"))
	{
		int controlPort = 7211;
		if (const char* cp = std::getenv("MAPLECAST_CONTROL_PORT"))
			controlPort = std::atoi(cp);
		if (maplecast_control_ws::init(controlPort)) {
			printf("[GSTA] render-debug panel: open web/gsta-render-debug.html?port=%d  (ws://localhost:%d)\n",
				controlPort, controlPort);
		}
	}

	// Mirror client writes directly to VRAM/RAM — don't re-protect after unprotect.
	// State-replica: the local SH4 is the SOLE renderer and must read/write VRAM
	// freely. The memwatch PROT_NONE is the mirror-SERVER's dirty-page-detection
	// mechanism (fault -> record -> ship); a replica ships nothing, so installing
	// it makes the local game's VRAM access segfault. Run like a normal flycast.
	if (!maplecast_mirror::isClient() && !maplecast_state_replica::active())
		memwatch::protect();
	else if (maplecast_state_replica::active())
		printf("[state-replica] memwatch VRAM protection NOT installed — local SH4 renders freely\n");

	if (config::ThreadedRendering)
	{
		printf("[emulator] Emulator::start → ThreadedRendering path, spawning Flycast-emu thread\n");
		fflush(stdout);
		const std::lock_guard<std::mutex> lock(mutex);
		getSh4Executor()->Start();
		threadResult = std::async(std::launch::async, [this] {
				ThreadName _("Flycast-emu");
				printf("[emulator] Flycast-emu thread running, about to call InitAudio()\n");
				fflush(stdout);

				// SCHED_FIFO for the SH4 emulator thread — consistent frame
				// timing is critical. Priority 40: below input (55) so input
				// never starves, but above normal threads.
#ifdef __linux__
				{
					struct sched_param sp{};
					sp.sched_priority = 40;
					if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0) {
						printf("[emulator] Flycast-emu → SCHED_FIFO priority 40\n"); fflush(stdout);
					} else {
						printf("[emulator] SCHED_FIFO not granted for emu thread (errno=%d)\n", errno); fflush(stdout);
					}
				}
#endif

				InitAudio();

				try {
					while (state == Running || singleStep || stepRangeTo != 0)
					{
						// Replica / lockstep player-client frame gate. If
						// we're in replica or player mode and the tape
						// hasn't caught up, block here until the next
						// packet arrives. Short spin — the rx thread is
						// pushing into the queues at UDP speed, so
						// worst-case latency is roughly one network RTT.
						// Replica and player are mutually exclusive by
						// env-var contract.
						if (maplecast_replica::active())
						{
							while (state == Running && !maplecast_replica::frameGate())
							{
								std::this_thread::sleep_for(std::chrono::microseconds(250));
							}
							if (state != Running) break;
						}
						else if (maplecast_player::active())
						{
							while (state == Running && !maplecast_player::frameGate())
							{
								// Yield for ~250us per spin. Tighter than
								// a sleep(0) so we don't miss a 16ms frame
								// deadline, looser than a busy-loop so we
								// don't pin a core.
								std::this_thread::sleep_for(std::chrono::microseconds(250));
							}
							if (state != Running) break;
						}
						// State-replica injection: write the server's GSTA into
						// RAM at the TOP of the frame so the SH4's draw code (run
						// by runInternal() below) renders the server's truth.
						// Stalls until the first GSTA arrives.
						if (maplecast_state_replica::active())
						{
							while (state == Running && !maplecast_state_replica::frameInject())
								std::this_thread::sleep_for(std::chrono::microseconds(250));
							if (state != Running) break;
						}
						startTime = sh4_sched_now64();
						renderTimeout = false;
						// === A2 RUN-AHEAD depth=1 (THREADED loop — the path prod actually runs;
						// mirror of the Emulator::run() version, same contract/comments there) ===
						{
							static const bool _raWant = [](){ const char* e = std::getenv("MAPLECAST_RUNAHEAD");
								return e && e[0] && e[0] != '0'; }();
							static bool _raReady = false, _raTried = false;
							if (_raWant && !_raTried) {
								_raTried = true;
								_raReady = maplecast_rollback::init();
								mc_runaheadArmed = _raReady;   // gates the vblank auto-save (divergent numbering)
								printf("[RUNAHEAD] %s (threaded loop)\n", _raReady ? "ARMED depth=1"
								                                                   : "rollback init FAILED - disabled");
								fflush(stdout);
							}
							if (_raReady && maplecast_mirror::isServer()) {
								// A2 v2 (local-rig trace 2026-07-12): runInternal() is RUN-UNTIL-STOPPED —
								// v1 wrapped it and never cycled (1695 SRs all hidden, 0 publishes = all
								// 4 dark rounds). Step frame legs with the predict primitive: arm a
								// one-shot Stop consumed at the next display STARTRENDER so each
								// runInternal() returns after exactly one leg; Start() re-arms the SH4.
								extern std::atomic<bool> mc_runaheadPreviewLeg;
								static uint64_t _raF = 0;
								// number our saves AFTER anything already in the ring (boot-time auto-saves
								// used a different counter — "target 1 older than ring tail 22")
								{ uint64_t mr = maplecast_rollback::mostRecentSaved();
								  _raF = (mr == UINT64_MAX || mr == 0) ? _raF + 1 : mr + 1; }
								// A2 PROF: per-leg stopwatch — names the thief in the 92ms/tick (budget ~24ms).
								auto _raT0 = std::chrono::steady_clock::now();
								maplecast_mirror::setSuppressPublish(true);
								maplecast_mirror::raArmStepStop();
								runInternal();                                   // -> stops at SR(T): hidden leg
								getSh4Executor()->Start();
								auto _raT1 = std::chrono::steady_clock::now();
								maplecast_mirror::setSuppressPublish(false);
								maplecast_rollback::saveFrame(_raF);
								auto _raT2 = std::chrono::steady_clock::now();
								mc_runaheadPreviewLeg.store(true, std::memory_order_relaxed);
								startTime = sh4_sched_now64();
								renderTimeout = false;
								maplecast_mirror::raArmStepStop();
								try {
									runInternal();                               // -> stops at SR(T+1): preview publishes
								} catch (...) {
									mc_runaheadPreviewLeg.store(false, std::memory_order_relaxed);
									throw;
								}
								getSh4Executor()->Start();
								auto _raT3 = std::chrono::steady_clock::now();
								mc_runaheadPreviewLeg.store(false, std::memory_order_relaxed);
								bool _raRewindOk = maplecast_rollback::rewindToFrame(_raF, /*lightweight=*/true);
								auto _raT4 = std::chrono::steady_clock::now();
								{
									static uint64_t _pH=0,_pS=0,_pP=0,_pR=0,_pN=0;
									auto us=[](auto a,auto b){ return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(b-a).count(); };
									_pH+=us(_raT0,_raT1); _pS+=us(_raT1,_raT2); _pP+=us(_raT2,_raT3); _pR+=us(_raT3,_raT4);
									if (++_pN % 600 == 0) {
										// integer µs only — a float varargs quirk printed 'inf' on MSVC
										printf("[RUNAHEAD-PROF] avg/tick us: hidden=%llu save=%llu preview=%llu rewind=%llu total=%llu (n=%llu)\n",
											(unsigned long long)(_pH/600),(unsigned long long)(_pS/600),
											(unsigned long long)(_pP/600),(unsigned long long)(_pR/600),
											(unsigned long long)((_pH+_pS+_pP+_pR)/600),(unsigned long long)_pN);
										fflush(stdout);
										_pH=_pS=_pP=_pR=0;
									}
								}
								if (!_raRewindOk) {
									printf("[RUNAHEAD] rewind FAILED at frame %llu - disabling\n",
										(unsigned long long)_raF);
									fflush(stdout);
									_raReady = false;
								}
							} else {
								runInternal();
							}
						}

						// Rollback ring stop-callback-restart: if vblank
						// called Stop() because a rewind is pending, runInternal
						// returned with SH4 paused. Execute deferred rewind from
						// safe context, then restart SH4 and skip the break check.
						// vblank() called rend_cancel_emu_wait() right after
						// Stop() to wake any pending render waits — but the
						// in-flight slice may have enqueued more work between
						// Stop and runInternal's actual return. Drain again
						// here before loadstate touches dynarec caches.
						if (maplecast_rollback::pendingRollback()) {
							rend_cancel_emu_wait();
							maplecast_rollback::executePendingRewind();
							getSh4Executor()->Start();
							continue;
						}
						// GENERIC PROBE v2 — no-restart live reload (SH4-THREAD apply
						// half). vblank() Stop()'d the SH4 because the render-thread
						// watcher flagged a config change, so runInternal() has returned
						// and the SH4 is fully PAUSED here on the emu thread — the SAME
						// proven-safe context the rollback deferred-rewind above uses for
						// bm_Reset/ResetCache. Re-parse the config and, if the armed-PC set
						// may have changed, flush the SH4 block cache so the recompiler
						// re-runs mc_isHookedPC against the new probe set on subsequently-
						// compiled blocks (re-injecting the block-entry GenCall at the new
						// PCs). ResetCache() must NOT run inside a compiled block nor race
						// the emu thread — both satisfied here. Then Start()+continue to
						// RESUME the SH4 (a bare Stop() would otherwise fall through to the
						// break check and terminate the emu thread). No-op when nothing
						// changed / the probe is disabled, so prod is unaffected.
						if (maplecast_oracle_hook::mc_probeApplyReload()) {
							getSh4Executor()->ResetCache();
							getSh4Executor()->Start();
							continue;
						}
						// STEP 2 read-set trace: vblank Stop()'d for the flip. SH4 is paused
						// here — safe to flip DynarecEnabled=false + ResetCache so the next
						// runInternal() executes under the interpreter (readt chokepoint) and
						// the trace arms at driver 0x8C030858. One-shot; no-op after.
						if (mc_readtrace::applyFlip()) {
							getSh4Executor()->ResetCache();
							getSh4Executor()->Start();
							continue;
						}
						// In replica mode we're not using GGPO, and
						// ggpo::nextFrame() returns false when no GGPO
						// session is active (_endOfFrame is never set),
						// which would break out of the emu loop after
						// one frame. Skip the check entirely.
						// Same gating applies when the rollback ring is
						// active: vblank Stop() fires whenever a rewind is
						// pending, making runInternal() return mid-loop.
						// Without this guard the unconditional break would
						// terminate the SH4 thread on the very first rewind.
						// GGPO sidesteps this by setting _endOfFrame=true
						// every vblank (ggpo.cpp:1076); we gate the break.
						if (!maplecast_replica::active()
								&& !maplecast_rollback::active()
								&& !ggpo::nextFrame())
							break;
					}
					TermAudio();
				} catch (...) {
					setNetworkState(false);
					getSh4Executor()->Stop();
					TermAudio();
					throw;
				}
		});
	}
	else
	{
		stopRequested = false;
		InitAudio();
	}

	EventManager::event(Event::Resume);
}

bool Emulator::checkStatus(bool wait)
{
	try {
		std::unique_lock<std::mutex> lock(mutex);
		if (threadResult.valid())
		{
            auto localResult = threadResult;
			lock.unlock();
			if (wait) {
				localResult.wait();
			}
			else {
				auto result = localResult.wait_for(std::chrono::seconds(0));
				if (result == std::future_status::timeout)
					return true;
			}
			localResult.get();
		}
		return false;
	} catch (...) {
		EventManager::event(Event::Pause);
		state = Error;
		throw;
	}
}

bool Emulator::render()
{
	FC_PROFILE_SCOPE;

	if (!config::ThreadedRendering)
	{
		if (stopRequested)
		{
			stopRequested = false;
			TermAudio();
			nvmem::saveFiles();
			EventManager::event(Event::Pause);
			return false;
		}
		if (state != Running)
			return false;
		run();
		// TODO if stopping due to a user request, no frame has been rendered
		return !renderTimeout;
	}
	if (!checkStatus())
		return false;
	if (state != Running)
		return false;
	return rend_single_frame(true); // FIXME stop flag?
}

void Emulator::vblank()
{
	EventManager::event(Event::VBlank);
	runner.execTasks();

	// Self-test: dual mode dc_serialize audit. Run with
	// MAPLECAST_SELFTEST_DESERIALIZE=1 (NO MAPLECAST_ROLLBACK_RING).
	//
	// 1. save+save audit: serialize twice in a row with NO state
	//    mutation between. If diff != 0, dc_serialize reads non-
	//    deterministic state. (Verified: diff=0 — dc_serialize is pure.)
	// 2. save+deserialize+continue: after the audit, deserialize the
	//    first blob and let SH4 keep running. Confirms dc_deserialize
	//    doesn't break SH4 forward progress.
	if (std::getenv("MAPLECAST_SELFTEST_DESERIALIZE")) {
		static int _selfTestFrame = 0;
		if (++_selfTestFrame == 600) {
			static std::vector<uint8_t> bufA(40 * 1024 * 1024);
			static std::vector<uint8_t> bufB(40 * 1024 * 1024);
			Serializer serA(bufA.data(), bufA.size(), false);
			dc_serialize(serA);
			size_t sizeA = serA.size();
			Serializer serB(bufB.data(), bufB.size(), false);
			dc_serialize(serB);
			size_t sizeB = serB.size();
			size_t commonSize = std::min(sizeA, sizeB);
			uint64_t diffBytes = 0;
			for (size_t i = 0; i < commonSize; i++)
				if (bufA[i] != bufB[i]) diffBytes++;
			printf("[selftest] save+save: sizeA=%zu sizeB=%zu diff=%llu (pure if 0)\n",
			       sizeA, sizeB, (unsigned long long)diffBytes); fflush(stdout);
		}
	}


	// Phase 1 A.4 rollback ring — capture SH4 + page-delta state at the
	// frame boundary. vblank() is called synchronously from SH4 execution
	// (the dynarec hits the vblank interrupt), so this runs on the SAME
	// thread that owns memwatch's PageMaps — no cross-thread race with the
	// fault handler. GGPO uses this same hook (ggpo::endOfFrame() below)
	// for the same reason. Done unconditionally on vblank, not gated on
	// the renderTimeout/threaded checks below — those affect "should we
	// signal end-of-frame to the rend thread", which is orthogonal to
	// "should we capture rollback state."
	// GATE ADD (Test B): continuous per-frame game-state hash log, gated on
	// MAPLECAST_GSHASH_LOG. Runs every vblank independent of the rollback ring.
	{
		static const char* _gshashLogPath = std::getenv("MAPLECAST_GSHASH_LOG");
		if (_gshashLogPath)
			maplecast_rollback::gshashLogTick(_gshashLogPath);
		static const char* _lagProbePath = std::getenv("MAPLECAST_LAGPROBE");
		if (_lagProbePath)
			maplecast_rollback::lagProbeTick(_lagProbePath);
		// A2 RUN-AHEAD GATE (kill-list 2026-07-12, MAPLECAST_RUNAHEAD_MEASURE=1): run-ahead
		// depth=1 needs save + 2x emulate + load inside 16.67ms. This measures the save leg
		// (dc_serialize into a reusable buffer) per frame ON THIS HARDWARE. avg+max logged
		// every 600f. If avg > ~4ms on the 2-vCPU prod box, A2 doesn't fit — kill it cheaply.
		{
			static const bool _raMeasure = std::getenv("MAPLECAST_RUNAHEAD_MEASURE") != nullptr;
			if (_raMeasure) {
				static std::vector<u8> _raBuf;
				static uint64_t _raN = 0, _raUsTot = 0, _raUsMax = 0;
				auto t0 = std::chrono::steady_clock::now();
				if (_raBuf.empty()) { Serializer szm; dc_serialize(szm); _raBuf.resize(szm.size() + (1u<<20)); }
				Serializer szr(_raBuf.data(), _raBuf.size(), false);
				dc_serialize(szr);
				uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - t0).count();
				_raUsTot += us; if (us > _raUsMax) _raUsMax = us; _raN++;
				if (_raN % 600 == 0) {
					printf("[RUNAHEAD-MEASURE] dc_serialize avg=%.2fms max=%.2fms size=%.1fMB n=%llu\n",
						(_raUsTot / (double)_raN) / 1000.0, _raUsMax / 1000.0,
						szr.size() / 1048576.0, (unsigned long long)_raN);
					fflush(stdout);
				}
			}
		}
	}

	// A2 v3.1: frame-step stop at the TRUE frame boundary (post-render, scheduler alive) —
	// the same stop-callback-restart context the rollback ring uses from vblank.
	if (maplecast_mirror::raConsumeStepStop())
		getSh4Executor()->Stop();

	if (maplecast_rollback::active() && !mc_runaheadArmed) {
		static uint64_t _rollbackFrameSeq = 0;
		maplecast_rollback::saveFrame(++_rollbackFrameSeq);

		// Stop-callback-restart trigger: if F.1/F.2 (or any caller)
		// requested a rewind during this frame's saveFrame, signal the
		// SH4 executor to Stop(). Run() returns at the next safe SH4
		// instruction boundary; the emu loop checks pendingRollback()
		// after runInternal() returns and executes the deferred rewind
		// from a paused-SH4 context. The break check at the top of the
		// emu loop is gated on maplecast_rollback::active() so the loop
		// continues instead of breaking out the way it would for a
		// non-rollback Stop() (e.g. user-initiated shutdown).
		if (maplecast_rollback::pendingRollback()) {
			getSh4Executor()->Stop();
			// Wake any in-flight render/pvrQueue waits so the SH4 thread
			// can actually exit runInternal(). Without this, Stop() only
			// flips a flag the dynarec polls between slices — but the SH4
			// may already be parked on renderEnd.Wait() or pvrQueue's
			// dequeueEvent.Wait(), neither of which Stop() touches. Result:
			// SH4 thread never returns, emu loop never sees pendingRollback,
			// deadlock. (Emulator::stop() uses this same call for the same
			// reason; we mirror it here for the rewind fast-path.)
			rend_cancel_emu_wait();
			// Skip the rest of vblank's tail (executePendingCapture +
			// renderTimeout block) so we don't re-enqueue render work that
			// creates fresh waits between Stop() and runInternal()'s return.
			return;
		}
	}

	// GENERIC PROBE v2 — no-restart live reload (SH4-THREAD Stop trigger).
	// vblank() runs synchronously from SH4 dispatch on the emu thread (a dynarec
	// frame is on the C++ stack), so we must NOT ResetCache() here. Instead — EXACTLY
	// like the rollback deferred-rewind above — when the render-thread watcher has
	// flagged a config change we Stop() the SH4 so Run() returns to the emu-loop
	// boundary, where mc_probeApplyReload()+getSh4Executor()->ResetCache() run with
	// the SH4 fully paused. rend_cancel_emu_wait() wakes any parked render/pvrQueue
	// wait so the SH4 thread can actually exit runInternal(). The emu-loop break
	// check is gated (see emulator.cpp start loop) so this Stop() resumes instead of
	// terminating the thread. No-op when the probe is disabled / nothing pending.
	if (maplecast_oracle_hook::mc_probeReloadPending()) {
		getSh4Executor()->Stop();
		rend_cancel_emu_wait();
		return;
	}

	// STEP 2 read-set trace: count frames; at the trigger frame Stop() the SH4 so
	// Run() returns to the emu-loop boundary where applyFlip() flips to interpreter
	// under a paused SH4 (SAME safe context as the probe reload above). No-op unless
	// MAPLECAST_READTRACE is set.
	mc_readtrace::onFrame();
	if (mc_readtrace::flipStopPending()) {
		getSh4Executor()->Stop();
		rend_cancel_emu_wait();
		return;
	}

	// Replay writer: deferred state capture. start() and onFrameInMatchFlag
	// queue a pending capture; this hook fires it from the SH4 thread on
	// vblank — same frame-boundary discipline maplecast_rollback::saveFrame
	// uses just above for byte-perfect rollback. No-op when nothing pending.
	maplecast_replay::executePendingCapture();

	// Time out if a frame hasn't been rendered for 50 ms
	if (sh4_sched_now64() - startTime <= 10000000)
		return;
	renderTimeout = true;
	if (ggpo::active())
		ggpo::endOfFrame();
	else if (!config::ThreadedRendering)
		getSh4Executor()->Stop();
}

bool Emulator::restartCpu()
{
	const std::lock_guard<std::mutex> _(mutex);
	if (state != Running)
		return false;
	getSh4Executor()->Start();
	return true;
}

void Emulator::insertGdrom(const std::string& path)
{
	if (settings.platform.isArcade())
		return;
	gdr::insertDisk(path);
	diskChange();
}

void Emulator::openGdrom()
{
	if (settings.platform.isArcade())
		return;
	gdr::openLid();
	diskChange();
}

void Emulator::diskChange()
{
	config::Settings::instance().reset();
	config::Settings::instance().load(false);
	custom_texture.terminate();
	if (!settings.content.path.empty())
	{
		hostfs::FileInfo info = hostfs::storage().getFileInfo(settings.content.path);
		settings.content.fileName = info.name;
		loadGameSpecificSettings();
	}
	else
	{
		settings.content.fileName.clear();
		settings.content.gameId.clear();
		settings.content.title = BIOS_TITLE;
	}
	cheatManager.reset(settings.content.gameId);
	if (cheatManager.isWidescreen())
		config::ScreenStretching.override(134);	// 4:3 -> 16:9
	EventManager::event(Event::DiskChange);
}

Emulator emu;
