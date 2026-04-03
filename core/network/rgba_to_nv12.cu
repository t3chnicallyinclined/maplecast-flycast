// CUDA kernels for MapleCast frame processing
// Compile: nvcc -ptx -arch=sm_86 rgba_to_nv12.cu -o rgba_to_nv12.ptx

// Strip alpha channel: RGBA (4 bpp) → RGB (3 bpp) for nvJPEG
extern "C" __global__ void rgba_to_rgb(
    const unsigned char* __restrict__ rgba, int rgba_pitch,
    unsigned char* __restrict__ rgb, int rgb_pitch,
    int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    int src = y * rgba_pitch + x * 4;
    int dst = y * rgb_pitch + x * 3;
    rgb[dst + 0] = rgba[src + 0];
    rgb[dst + 1] = rgba[src + 1];
    rgb[dst + 2] = rgba[src + 2];
}

// RGBA → NV12 (BT.601 limited range) for NVENC

extern "C" __global__ void rgba_to_nv12(
    const unsigned char* __restrict__ rgba, int rgba_pitch,
    unsigned char* __restrict__ y_plane,
    unsigned char* __restrict__ uv_plane,
    int nv12_pitch, int width, int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    int rgba_idx = y * rgba_pitch + x * 4;
    int r = rgba[rgba_idx + 0];
    int g = rgba[rgba_idx + 1];
    int b = rgba[rgba_idx + 2];

    // BT.601 limited range
    int Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
    y_plane[y * nv12_pitch + x] = (unsigned char)min(max(Y, 0), 255);

    // Subsample UV every 2x2
    if ((x & 1) == 0 && (y & 1) == 0)
    {
        int U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
        int V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        int uv_idx = (y / 2) * nv12_pitch + x;
        uv_plane[uv_idx + 0] = (unsigned char)min(max(U, 0), 255);
        uv_plane[uv_idx + 1] = (unsigned char)min(max(V, 0), 255);
    }
}
