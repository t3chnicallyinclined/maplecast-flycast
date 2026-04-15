/*
	MapleCast zstd compression for mirror streaming.

	Wraps zstd compress/decompress with pre-allocated contexts and buffers.
	Compressed frames use a "ZCST" magic header so clients can detect and
	decompress transparently, while uncompressed frames pass through unchanged.

	Wire format: [ZCST(4)] [uncompressedSize(4)] [zstd blob(N)]

	Define MAPLECAST_COMPRESS_ONLY_DECOMPRESS to exclude the compressor
	(standalone WASM renderer only links zstd decompress sources).

	!!! FRAGILE — THE MAGIC CONSTANT IS A BYTE-ORDER LANDMINE !!!

	The wire bytes for ZCST are [0x5A, 0x43, 0x53, 0x54] ("ZCST" ASCII).
	When loaded as a little-endian uint32 (which is what memcpy on x86_64
	produces), the value MUST be 0x5453435A, NOT 0x5A435354.

	The latter serializes back to bytes "TSCZ" — wire-incompatible with:
	  - JS: relay.js / renderer-bridge.mjs check `getUint32(0, true) === 0x5453435A`
	  - Rust: relay/src/protocol.rs checks `&data[0..4] == b"ZCST"`
	  - C++: this file's MCST_MAGIC_COMPRESSED constant below

	All four sides MUST agree. If you "fix" the constant on one side because
	"the bytes look wrong" you will break the other three. The current value
	below is correct. Do not change it.

	This header is the wire format definition for the entire mirror stream.
	If you change the envelope, you must update:
	  - core/network/maplecast_mirror.cpp (server publish + desktop client receive)
	  - core/network/maplecast_wasm_bridge.cpp (emulator.html WASM)
	  - packages/renderer/src/wasm_bridge.cpp (king.html WASM)
	  - relay/src/fanout.rs and relay/src/protocol.rs (Rust VPS relay)
	  - web/relay.js and web/js/renderer-bridge.mjs (browser routing)

	See docs/ARCHITECTURE.md "Mirror Wire Format — Rules of the Road" for the
	canonical list of rules all four parsers must obey.
*/
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <zstd.h>

// Wire magic: 4 bytes "ZCST" = 0x5A 0x43 0x53 0x54
// As little-endian uint32 stored via memcpy: 0x5453435A
static constexpr uint32_t MCST_MAGIC_COMPRESSED = 0x5453435A; // "ZCST" on the wire

#ifndef MAPLECAST_COMPRESS_ONLY_DECOMPRESS

#include <chrono>
#include <mutex>

struct MirrorCompressor
{
	ZSTD_CCtx* cctx = nullptr;
	uint8_t*   buf = nullptr;
	size_t     bufSize = 0;
	// ZSTD contexts are NOT thread-safe. Real production discovery:
	// serverPublish() can be invoked from the render thread AND from
	// other TA-publish paths (SYNC broadcast, forced-sync) — a concurrent
	// ZSTD_compressCCtx reset-init on the shared cctx corrupts the
	// workspace and crashes with a bogus-pointer free (e.g. customFree
	// with ptr=0x3f800000 that's actually a float from the source buffer
	// we were reading). Serialize all compress() calls here.
	std::mutex mtx;

	void init(size_t maxInputSize)
	{
		cctx = ZSTD_createCCtx();
		// 8-byte header + zstd worst-case expansion
		bufSize = 8 + ZSTD_compressBound(maxInputSize);
		buf = (uint8_t*)malloc(bufSize);
	}

	void destroy()
	{
		if (cctx) { ZSTD_freeCCtx(cctx); cctx = nullptr; }
		if (buf)  { free(buf); buf = nullptr; }
	}

	// Compress src into pre-allocated buf with ZCST header.
	// Returns pointer to compressed output; sets outSize.
	// level: 1 for per-frame (fast), 3 for SYNC (better ratio).
	const uint8_t* compress(const uint8_t* src, uint32_t srcSize,
	                        size_t& outSize, uint64_t& compressUs, int level = 1)
	{
		std::lock_guard<std::mutex> lk(mtx);
		auto t0 = std::chrono::high_resolution_clock::now();

		// Header: magic + uncompressed size
		memcpy(buf, &MCST_MAGIC_COMPRESSED, 4);
		memcpy(buf + 4, &srcSize, 4);

		size_t zstdSize = ZSTD_compressCCtx(cctx, buf + 8, bufSize - 8,
		                                     src, srcSize, level);
		if (ZSTD_isError(zstdSize))
		{
			// Compression failed — send uncompressed (no header)
			outSize = srcSize;
			compressUs = 0;
			return src;
		}

		outSize = 8 + zstdSize;

		auto t1 = std::chrono::high_resolution_clock::now();
		compressUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
		return buf;
	}
};

#endif // MAPLECAST_COMPRESS_ONLY_DECOMPRESS

struct MirrorDecompressor
{
	ZSTD_DCtx* dctx = nullptr;
	uint8_t*   buf = nullptr;
	size_t     bufSize = 0;

	void init(size_t maxOutputSize)
	{
		dctx = ZSTD_createDCtx();
		bufSize = maxOutputSize;
		buf = (uint8_t*)malloc(bufSize);
	}

	void destroy()
	{
		if (dctx) { ZSTD_freeDCtx(dctx); dctx = nullptr; }
		if (buf)  { free(buf); buf = nullptr; }
	}

	// If data starts with ZCST magic: decompress into buf, return buf.
	// Otherwise: return src as-is (zero-copy passthrough).
	const uint8_t* decompress(const uint8_t* src, size_t srcSize, size_t& outSize)
	{
		if (srcSize < 8)
		{
			outSize = srcSize;
			return src;
		}

		uint32_t magic;
		memcpy(&magic, src, 4);
		if (magic != MCST_MAGIC_COMPRESSED)
		{
			// Not compressed — passthrough
			outSize = srcSize;
			return src;
		}

		uint32_t uncompSize;
		memcpy(&uncompSize, src + 4, 4);

		if (uncompSize > bufSize)
		{
			// Grow buffer
			bufSize = uncompSize + (uncompSize >> 2);
			buf = (uint8_t*)realloc(buf, bufSize);
		}

		size_t result = ZSTD_decompressDCtx(dctx, buf, bufSize,
		                                     src + 8, srcSize - 8);
		if (ZSTD_isError(result))
		{
			// Decompression failed — skip frame
			outSize = 0;
			return buf;
		}

		outSize = result;
		return buf;
	}
};
