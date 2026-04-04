/*
	MapleCast Rend Diff v2 — RAM AUTOPSY ENGINE

	The 253-byte game state produces 1.2% visual divergence.
	This module finds exactly which RAM addresses contain the hidden state.

	Strategy:
	1. Every frame: hash game state (253 bytes) + hash rend_context (vertex positions)
	2. On FIRST occurrence of a game state hash: snapshot wide RAM regions as BASELINE
	3. On DIVERGED occurrence (same GS hash, different visuals): snapshot RAM again
	4. DIFF the snapshots byte-by-byte → the bytes that changed ARE the hidden state
	5. Track per-address divergence correlation → rank by "how often does this byte
	   differ when visuals differ?" → top ranked = highest priority hidden variables

	RAM regions scanned:
	- 0x8C289600-0x8C289700 (256 bytes) — global state area (timer, stage, meters + neighbors)
	- 0x8C1F9C00-0x8C1FA000 (1024 bytes) — camera region + surrounding data
	- 0x8C268000-0x8C26A400 (9216 bytes) — ALL 6 character structs + padding between
	- 0x8C349600-0x8C349800 (512 bytes) — frame counter region
	- 0x8C0E0000-0x8C0E0400 (1024 bytes) — system/PVR area (potential stage anim timers)
	- 0x8C200000-0x8C200400 (1024 bytes) — lower RAM region (potential RNG/system state)

	Output:
	- rend_diff_log.csv — per-frame log (same as v1)
	- rend_autopsy.csv — per-address divergence correlation ranking
	- rend_snapshots.bin — raw snapshot pairs for deep analysis
*/
#include "types.h"
#include "maplecast_rend_diff.h"
#include "maplecast_gamestate.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/sh4/sh4_mem.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <string>

namespace maplecast_rend_diff
{

// ==================== RAM SCAN REGIONS ====================
// Each region is a contiguous block of DC virtual memory to snapshot
struct ScanRegion {
	uint32_t addr;      // DC virtual address (0x8Cxxxxxx)
	uint32_t size;      // bytes to read
	const char* name;   // human-readable label
};

static const ScanRegion SCAN_REGIONS[] = {
	{ 0x8C289600, 256,   "global_state" },       // timer, stage, meters, combos + neighbors
	{ 0x8C1F9C00, 1024,  "camera_region" },       // camera_x/y + surrounding unknown data
	{ 0x8C268000, 9728,  "char_structs" },        // all 6 chars (0x8C268000 to 0x8C26A600)
	{ 0x8C349600, 512,   "frame_ctr_region" },    // frame counter + neighbors
	{ 0x8C0E0000, 1024,  "pvr_system" },          // PVR/system area — potential stage anim
	{ 0x8C200000, 1024,  "low_ram" },             // lower RAM — RNG, system state
	{ 0x8C280000, 4096,  "pre_global" },          // area before global state — fight engine vars
	{ 0x8C260000, 2048,  "pre_char" },            // area before character structs
	{ 0x8C26A600, 2048,  "post_char" },           // area after character structs — projectiles?
	{ 0x8C100000, 512,   "ings_area" },           //ings/ings-like addresses (common in DC games)
};
static const int NUM_REGIONS = sizeof(SCAN_REGIONS) / sizeof(SCAN_REGIONS[0]);

// Total snapshot size
static uint32_t totalSnapshotSize()
{
	uint32_t total = 0;
	for (int i = 0; i < NUM_REGIONS; i++)
		total += SCAN_REGIONS[i].size;
	return total;
}

// ==================== CORE STATE ====================
static std::atomic<bool> _active{false};
static std::mutex _mutex;
static FILE* _csvFile = nullptr;
static FILE* _autopsyFile = nullptr;
static FILE* _snapshotFile = nullptr;
static uint32_t _frameNum = 0;
static uint32_t _divergenceCount = 0;
static uint32_t _totalFrames = 0;

// FNV-1a hash
static uint64_t fnv1a(const void* data, size_t len)
{
	const uint8_t* p = (const uint8_t*)data;
	uint64_t h = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < len; i++)
	{
		h ^= p[i];
		h *= 0x100000001b3ULL;
	}
	return h;
}

// ==================== RENDER FINGERPRINT ====================
struct RendFingerprint {
	uint64_t vertex_hash;
	uint64_t texref_hash;
	uint32_t num_verts;
	uint32_t num_polys_op;
	uint32_t num_polys_pt;
	uint32_t num_polys_tr;
	uint32_t num_indices;
};

static RendFingerprint hashRendContext(rend_context& rc)
{
	RendFingerprint fp = {};
	fp.num_verts = (uint32_t)rc.verts.size();
	fp.num_polys_op = (uint32_t)rc.global_param_op.size();
	fp.num_polys_pt = (uint32_t)rc.global_param_pt.size();
	fp.num_polys_tr = (uint32_t)rc.global_param_tr.size();
	fp.num_indices = (uint32_t)rc.idx.size();

	if (!rc.verts.empty())
	{
		std::vector<float> pos;
		pos.reserve(rc.verts.size() * 3);
		for (const auto& v : rc.verts)
		{
			pos.push_back(v.x);
			pos.push_back(v.y);
			pos.push_back(v.z);
		}
		fp.vertex_hash = fnv1a(pos.data(), pos.size() * sizeof(float));
	}

	{
		std::vector<uint32_t> tex;
		const std::vector<PolyParam>* lists[3] = {
			&rc.global_param_op, &rc.global_param_pt, &rc.global_param_tr
		};
		for (int li = 0; li < 3; li++)
			for (const auto& pp : *lists[li])
				tex.push_back(pp.texture ? pp.tcw.full : 0);
		if (!tex.empty())
			fp.texref_hash = fnv1a(tex.data(), tex.size() * sizeof(uint32_t));
	}

	return fp;
}

static uint64_t hashFingerprint(const RendFingerprint& fp)
{
	uint64_t h = fp.vertex_hash;
	h ^= fp.texref_hash * 0x9e3779b97f4a7c15ULL;
	h ^= (uint64_t)fp.num_verts * 0x517cc1b727220a95ULL;
	h ^= (uint64_t)fp.num_polys_op * 0x6c62272e07bb0142ULL;
	h ^= (uint64_t)fp.num_polys_tr * 0x62b821756295c58dULL;
	return h;
}

// ==================== RAM SNAPSHOT ====================
struct RamSnapshot {
	std::vector<uint8_t> data;  // concatenated region snapshots

	void capture()
	{
		uint32_t total = totalSnapshotSize();
		data.resize(total);
		uint32_t off = 0;
		for (int i = 0; i < NUM_REGIONS; i++)
		{
			uint32_t addr = SCAN_REGIONS[i].addr;
			uint32_t size = SCAN_REGIONS[i].size;
			// Read 32-bit aligned chunks (4x faster than byte-by-byte)
			uint32_t j = 0;
			for (; j + 3 < size; j += 4)
			{
				uint32_t val = addrspace::read32(addr + j);
				memcpy(&data[off + j], &val, 4);
			}
			// Handle remaining bytes
			for (; j < size; j++)
				data[off + j] = (uint8_t)addrspace::read8(addr + j);
			off += size;
		}
	}
};

// ==================== STATE MAP WITH RAM SNAPSHOTS ====================
struct StateEntry {
	uint64_t rend_hash;
	uint32_t first_frame;
	RendFingerprint first_fp;
	RamSnapshot baseline_ram;   // RAM snapshot from first (clean) occurrence
	uint32_t hit_count;
	uint32_t diverge_count;
};

static std::unordered_map<uint64_t, StateEntry> _stateMap;

// ==================== PER-ADDRESS DIVERGENCE TRACKING ====================
// For every byte offset in the snapshot, track:
// - how many times it differed when visuals diverged
// - how many times it differed when visuals matched (false positive control)
struct AddrStats {
	uint32_t diff_on_diverge;    // byte changed AND visuals diverged
	uint32_t diff_on_clean;      // byte changed BUT visuals were same (noise)
	uint32_t same_on_diverge;    // byte unchanged BUT visuals diverged
};

static std::vector<AddrStats> _addrStats;

// Convert snapshot offset to DC address + region name
static void offsetToAddr(uint32_t offset, uint32_t& dcAddr, const char*& regionName)
{
	uint32_t off = 0;
	for (int i = 0; i < NUM_REGIONS; i++)
	{
		if (offset < off + SCAN_REGIONS[i].size)
		{
			dcAddr = SCAN_REGIONS[i].addr + (offset - off);
			regionName = SCAN_REGIONS[i].name;
			return;
		}
		off += SCAN_REGIONS[i].size;
	}
	dcAddr = 0;
	regionName = "unknown";
}

// ==================== INIT ====================
void init()
{
	_active = true;
	_frameNum = 0;
	_divergenceCount = 0;
	_totalFrames = 0;
	_stateMap.clear();
	_addrStats.clear();
	_addrStats.resize(totalSnapshotSize(), {0, 0, 0});

	_csvFile = fopen("rend_diff_log.csv", "w");
	if (_csvFile)
	{
		fprintf(_csvFile,
			"frame,gs_hash,rend_hash,diverged,"
			"verts,polys_op,polys_pt,polys_tr,indices,"
			"timer,stage,frame_ctr,"
			"ram_bytes_changed,top_changed_addr\n");
		fflush(_csvFile);
	}

	_snapshotFile = fopen("rend_snapshots.bin", "wb");

	printf("[REND_DIFF v2] === RAM AUTOPSY ENGINE INITIALIZED ===\n");
	printf("[REND_DIFF v2] Scanning %d RAM regions, %u bytes per snapshot\n",
		NUM_REGIONS, totalSnapshotSize());
	for (int i = 0; i < NUM_REGIONS; i++)
		printf("  [%s] 0x%08X - 0x%08X (%u bytes)\n",
			SCAN_REGIONS[i].name, SCAN_REGIONS[i].addr,
			SCAN_REGIONS[i].addr + SCAN_REGIONS[i].size, SCAN_REGIONS[i].size);
	printf("[REND_DIFF v2] Output: rend_diff_log.csv, rend_autopsy.csv, rend_snapshots.bin\n");
}

// ==================== TICK (every frame) ====================
void tick(rend_context& rc)
{
	if (!_active) return;

	std::lock_guard<std::mutex> lock(_mutex);

	// Read game state
	maplecast_gamestate::GameState gs;
	maplecast_gamestate::readGameState(gs);
	if (!gs.in_match) return;

	_totalFrames++;
	_frameNum = gs.frame_counter;

	// Game state fingerprint
	uint8_t gsBuf[256];
	int gsLen = maplecast_gamestate::serialize(gs, gsBuf, sizeof(gsBuf));
	uint64_t gsHash = fnv1a(gsBuf, gsLen);

	// Render fingerprint
	RendFingerprint fp = hashRendContext(rc);
	uint64_t rendHash = hashFingerprint(fp);

	bool diverged = false;
	uint32_t ramBytesChanged = 0;
	uint32_t topChangedAddr = 0;

	auto it = _stateMap.find(gsHash);
	if (it == _stateMap.end())
	{
		// First time seeing this state — snapshot RAM as baseline
		StateEntry entry;
		entry.rend_hash = rendHash;
		entry.first_frame = _frameNum;
		entry.first_fp = fp;
		entry.baseline_ram.capture();  // snapshot on first occurrence only
		entry.hit_count = 1;
		entry.diverge_count = 0;
		_stateMap[gsHash] = entry;
	}
	else
	{
		it->second.hit_count++;

		// Only snapshot RAM on divergences (always) or clean matches (sampled 1-in-8)
		bool isVisualDivergence = (it->second.rend_hash != rendHash);
		bool shouldSnapshot = isVisualDivergence || (it->second.hit_count % 8 == 0);

		if (shouldSnapshot)
		{
			RamSnapshot currentRam;
			currentRam.capture();

			const auto& baseline = it->second.baseline_ram.data;
			const auto& current = currentRam.data;
			uint32_t snapSize = (uint32_t)baseline.size();

			if (isVisualDivergence)
			{
				// *** DIVERGENCE: same game state, different visuals ***
				diverged = true;
				it->second.diverge_count++;
				_divergenceCount++;

				// Diff every byte — changed ones correlate with hidden state
				uint32_t maxDiffScore = 0;
				for (uint32_t i = 0; i < snapSize; i++)
				{
					if (baseline[i] != current[i])
					{
						_addrStats[i].diff_on_diverge++;
						ramBytesChanged++;
						if (_addrStats[i].diff_on_diverge > maxDiffScore)
						{
							maxDiffScore = _addrStats[i].diff_on_diverge;
							uint32_t dcAddr;
							const char* rn;
							offsetToAddr(i, dcAddr, rn);
							topChangedAddr = dcAddr;
						}
					}
					else
					{
						_addrStats[i].same_on_diverge++;
					}
				}

				printf("[REND_DIFF v2] *** DIVERGENCE #%u @ frame %u | %u RAM bytes changed | top=0x%08X ***\n",
					_divergenceCount, _frameNum, ramBytesChanged, topChangedAddr);

				// Write snapshot pair for deep analysis
				if (_snapshotFile)
				{
					fwrite(&_frameNum, 4, 1, _snapshotFile);
					fwrite(&gsHash, 8, 1, _snapshotFile);
					fwrite(&it->second.rend_hash, 8, 1, _snapshotFile);
					fwrite(&rendHash, 8, 1, _snapshotFile);
					fwrite(&snapSize, 4, 1, _snapshotFile);
					fwrite(baseline.data(), 1, snapSize, _snapshotFile);
					fwrite(current.data(), 1, snapSize, _snapshotFile);
					fflush(_snapshotFile);
				}
			}
			else
			{
				// Clean match (sampled) — track noise
				for (uint32_t i = 0; i < snapSize; i++)
				{
					if (baseline[i] != current[i])
						_addrStats[i].diff_on_clean++;
				}
			}
		}
		else if (isVisualDivergence)
		{
			// Divergence detected but shouldn't happen since we always snapshot on divergence
			diverged = true;
			it->second.diverge_count++;
			_divergenceCount++;
		}
	}

	// CSV log
	if (_csvFile)
	{
		fprintf(_csvFile,
			"%u,%016lx,%016lx,%d,"
			"%u,%u,%u,%u,%u,"
			"%u,%u,%u,"
			"%u,0x%08X\n",
			_frameNum, gsHash, rendHash, diverged ? 1 : 0,
			fp.num_verts, fp.num_polys_op, fp.num_polys_pt, fp.num_polys_tr, fp.num_indices,
			(uint32_t)gs.game_timer, (uint32_t)gs.stage_id, gs.frame_counter,
			ramBytesChanged, topChangedAddr);

		if (_totalFrames % 60 == 0)
			fflush(_csvFile);
	}

	// Periodic summary with top addresses
	if (_totalFrames % 300 == 0)
	{
		printf("[REND_DIFF v2] %u frames | %zu states | %u divergences (%.1f%%)\n",
			_totalFrames, _stateMap.size(), _divergenceCount,
			_totalFrames > 0 ? 100.0f * _divergenceCount / _totalFrames : 0.f);

		// Quick top-5 addresses
		struct Ranked { uint32_t offset; uint32_t score; float precision; };
		std::vector<Ranked> ranked;
		for (uint32_t i = 0; i < (uint32_t)_addrStats.size(); i++)
		{
			if (_addrStats[i].diff_on_diverge > 0)
			{
				float total = (float)(_addrStats[i].diff_on_diverge + _addrStats[i].diff_on_clean);
				float precision = total > 0 ? _addrStats[i].diff_on_diverge / total : 0;
				ranked.push_back({i, _addrStats[i].diff_on_diverge, precision});
			}
		}
		std::sort(ranked.begin(), ranked.end(),
			[](const Ranked& a, const Ranked& b) {
				// Sort by precision first (high signal), then by count
				if (std::abs(a.precision - b.precision) > 0.1f) return a.precision > b.precision;
				return a.score > b.score;
			});

		printf("  TOP SUSPECT ADDRESSES (precision = diverge_only / all_changes):\n");
		int shown = 0;
		for (const auto& r : ranked)
		{
			if (shown >= 10) break;
			uint32_t dcAddr;
			const char* regionName;
			offsetToAddr(r.offset, dcAddr, regionName);
			printf("    0x%08X [%s] diverged=%u clean=%u precision=%.0f%%\n",
				dcAddr, regionName, _addrStats[r.offset].diff_on_diverge,
				_addrStats[r.offset].diff_on_clean, r.precision * 100.f);
			shown++;
		}
	}
}

bool active()
{
	return _active;
}

// ==================== FINAL REPORT + AUTOPSY CSV ====================
void report()
{
	if (!_active) return;

	printf("\n[REND_DIFF v2] ============= RAM AUTOPSY REPORT =============\n");
	printf("[REND_DIFF v2] Frames: %u | Unique states: %zu | Divergences: %u (%.2f%%)\n",
		_totalFrames, _stateMap.size(), _divergenceCount,
		_totalFrames > 0 ? 100.0f * _divergenceCount / _totalFrames : 0.f);

	// Build ranked address list
	struct Ranked {
		uint32_t offset;
		uint32_t dcAddr;
		const char* region;
		uint32_t divCount;
		uint32_t cleanCount;
		uint32_t sameOnDiv;
		float precision;   // divCount / (divCount + cleanCount) — how specific to divergence
		float recall;       // divCount / total_divergences — how often it catches divergence
	};

	std::vector<Ranked> ranked;
	for (uint32_t i = 0; i < (uint32_t)_addrStats.size(); i++)
	{
		const auto& s = _addrStats[i];
		if (s.diff_on_diverge == 0) continue;

		Ranked r;
		r.offset = i;
		offsetToAddr(i, r.dcAddr, r.region);
		r.divCount = s.diff_on_diverge;
		r.cleanCount = s.diff_on_clean;
		r.sameOnDiv = s.same_on_diverge;
		float total = (float)(r.divCount + r.cleanCount);
		r.precision = total > 0 ? r.divCount / total : 0;
		r.recall = _divergenceCount > 0 ? (float)r.divCount / _divergenceCount : 0;
		ranked.push_back(r);
	}

	// Sort: precision * recall (F1-like score)
	std::sort(ranked.begin(), ranked.end(),
		[](const Ranked& a, const Ranked& b) {
			float scoreA = 2.f * a.precision * a.recall / (a.precision + a.recall + 0.001f);
			float scoreB = 2.f * b.precision * b.recall / (b.precision + b.recall + 0.001f);
			return scoreA > scoreB;
		});

	printf("\n  HIDDEN STATE CANDIDATES (ranked by F1 = precision * recall):\n");
	printf("  %-12s %-18s %8s %8s %8s %6s %6s\n",
		"DC_ADDR", "REGION", "DIV_HIT", "CLEAN", "PREC%", "REC%", "F1%");
	printf("  %-12s %-18s %8s %8s %8s %6s %6s\n",
		"--------", "------", "-------", "-----", "-----", "----", "---");

	int shown = 0;
	for (const auto& r : ranked)
	{
		if (shown >= 50) break;
		float f1 = 2.f * r.precision * r.recall / (r.precision + r.recall + 0.001f);
		printf("  0x%08X   %-18s %8u %8u %7.0f%% %5.0f%% %4.0f%%\n",
			r.dcAddr, r.region, r.divCount, r.cleanCount,
			r.precision * 100.f, r.recall * 100.f, f1 * 100.f);
		shown++;
	}

	// Write autopsy CSV
	_autopsyFile = fopen("rend_autopsy.csv", "w");
	if (_autopsyFile)
	{
		fprintf(_autopsyFile, "dc_addr,region,offset,div_hit,clean_hit,same_on_div,precision,recall,f1\n");
		for (const auto& r : ranked)
		{
			float f1 = 2.f * r.precision * r.recall / (r.precision + r.recall + 0.001f);
			fprintf(_autopsyFile, "0x%08X,%s,%u,%u,%u,%u,%.4f,%.4f,%.4f\n",
				r.dcAddr, r.region, r.offset, r.divCount, r.cleanCount,
				r.sameOnDiv, r.precision, r.recall, f1);
		}
		fclose(_autopsyFile);
		printf("\n[REND_DIFF v2] Autopsy CSV saved to rend_autopsy.csv\n");
	}

	// Group by region for summary
	printf("\n  DIVERGENCE BY REGION:\n");
	std::unordered_map<std::string, uint32_t> regionCounts;
	for (const auto& r : ranked)
		regionCounts[r.region] += r.divCount;
	std::vector<std::pair<std::string, uint32_t>> regionVec(regionCounts.begin(), regionCounts.end());
	std::sort(regionVec.begin(), regionVec.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });
	for (const auto& rv : regionVec)
		printf("    %-20s %u divergence-byte-hits\n", rv.first.c_str(), rv.second);

	if (_divergenceCount == 0)
		printf("\n[REND_DIFF v2] *** ZERO DIVERGENCES — 253 bytes is COMPLETE! ***\n");
	else
	{
		printf("\n[REND_DIFF v2] *** %u divergences — %zu candidate addresses found ***\n",
			_divergenceCount, ranked.size());
		printf("[REND_DIFF v2] Next step: add top precision+recall addresses to GameState struct\n");
	}
	printf("[REND_DIFF v2] =============================================\n\n");

	if (_csvFile) { fflush(_csvFile); fclose(_csvFile); _csvFile = nullptr; }
	if (_snapshotFile) { fflush(_snapshotFile); fclose(_snapshotFile); _snapshotFile = nullptr; }
}

}  // namespace maplecast_rend_diff
