/*
	MapleCast zstd compression for mirror streaming.

	Wraps zstd compress/decompress with pre-allocated contexts and buffers.
	Compressed frames use a "ZCST" magic header so clients can detect and
	decompress transparently, while uncompressed frames pass through unchanged.

	Wire format: [ZCST(4)] [uncompressedSize(4)] [zstd blob(N)]

	Define MAPLECAST_COMPRESS_ONLY_DECOMPRESS to exclude the compressor
	(standalone WASM renderer only links zstd decompress sources).
*/
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <zstd.h>

static constexpr uint32_t MCST_MAGIC_COMPRESSED = 0x5A435354; // "ZCST"

#ifndef MAPLECAST_COMPRESS_ONLY_DECOMPRESS

#include <chrono>

struct MirrorCompressor
{
	ZSTD_CCtx* cctx = nullptr;
	uint8_t*   buf = nullptr;
	size_t     bufSize = 0;

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
