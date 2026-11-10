#include "hqx_upscaler.h"
#include "worker_pool.h"

#include <intrin.h>

#include <hqx/HQ2x.hh>
#include <hqx/HQ3x.hh>

#pragma intrinsic(__movsd)

// The game renders 8-bit palettized frames. Each 32-bit hqx input pixel
// carries its palette index in the alpha byte, so color similarity becomes a
// lookup in a table that is rebuilt only when the palette changes. The
// output alpha byte is a meaningless blend of indices; X8R8G8B8 ignores it.
//
// The definitions of the HQx base class below replace
// vendor/hqx/source/HQx.cc, which is not compiled: its isDifferent converts
// both colors to YUV with floating point on every call. The table follows
// the same YUV formula and the library's sharper (DOSBox) comparison.
static const uint32_t kThresholdY = 0x30;
static const uint32_t kThresholdU = 0x07;
static const uint32_t kThresholdV = 0x06;
static const uint32_t kThresholdA = 0x50;

// The frame is scaled in horizontal stripes, one per worker pool part. Each
// stripe is scaled with one extra source row above and below where it has a
// neighbor, so its 3x3 windows match those of the whole frame; the output of
// those halo rows is discarded.
static const DWORD kMaxStripes = 8;
static const DWORD kMinStripeRows = 16;

struct Stripe
{
    DWORD firstRow;
    DWORD rowCount;
    DWORD haloTop;
    DWORD inputRows;
    DWORD* input;
    DWORD* output;
};

struct ScaleJob
{
    const BYTE* source;
    LONG sourcePitch;
    BYTE* destination;
    LONG destinationPitch;
};

static BYTE g_different[256 * 256];
static DWORD g_rgb[256];
static BYTE g_paletteCache[256 * 3];
static BOOL g_paletteValid = FALSE;
static DWORD g_width = 0;
static DWORD g_height = 0;
static DWORD g_scale = 0;
static Stripe g_stripes[kMaxStripes];
static DWORD g_stripeCount = 0;

HQx::HQx()
{
}

HQx::~HQx()
{
}

static void ComputeYuv(DWORD rgb, LONG* y, LONG* u, LONG* v)
{
    LONG red = static_cast<LONG>((rgb >> 16) & 0xFF);
    LONG green = static_cast<LONG>((rgb >> 8) & 0xFF);
    LONG blue = static_cast<LONG>(rgb & 0xFF);

    // Integer form of the library's ARGBtoAYUV; division truncates toward
    // zero like its double-to-integer conversion.
    *y = (299 * red + 587 * green + 114 * blue) / 1000;
    *u = (-169 * red - 331 * green + 500 * blue) / 1000 + 128;
    *v = (500 * red - 419 * green - 81 * blue) / 1000 + 128;
}

uint32_t HQx::ARGBtoAYUV(uint32_t value)
{
    LONG y;
    LONG u;
    LONG v;

    ComputeYuv(value, &y, &u, &v);
    return (value & 0xFF000000) |
        (static_cast<uint32_t>(y) << 16) |
        ((static_cast<uint32_t>(u) & 0xFF) << 8) |
        (static_cast<uint32_t>(v) & 0xFF);
}

bool HQx::isDifferent(
    uint32_t color1,
    uint32_t color2,
    uint32_t,
    uint32_t,
    uint32_t,
    uint32_t)
{
    return g_different[((color1 >> 24) << 8) | (color2 >> 24)] != 0;
}

// The vtables of the hqx classes reference these. Nothing allocates hqx
// objects on the heap and no pure virtual function is ever called.
void __cdecl operator delete(void* pointer)
{
    if (pointer != NULL)
    {
        HeapFree(GetProcessHeap(), 0, pointer);
    }
}

extern "C" int __cdecl _purecall(void)
{
    return 0;
}

static DWORD* AllocatePixels(DWORD count)
{
    return reinterpret_cast<DWORD*>(VirtualAlloc(
        NULL,
        count * sizeof(DWORD),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
}

static void FreePixels(DWORD** pixels)
{
    if (*pixels != NULL)
    {
        VirtualFree(*pixels, 0, MEM_RELEASE);
        *pixels = NULL;
    }
}

static void ProcessStripe(const ScaleJob* job, Stripe* stripe)
{
    DWORD outputWidth = g_width * g_scale;
    const BYTE* source = job->source +
        static_cast<LONG>(stripe->firstRow - stripe->haloTop) * job->sourcePitch;
    DWORD* input = stripe->input;
    const DWORD* output;
    BYTE* destination;
    DWORD row;
    DWORD column;

    for (row = 0; row < stripe->inputRows; ++row)
    {
        for (column = 0; column < g_width; ++column)
        {
            DWORD index = source[column];

            input[column] = (index << 24) | g_rgb[index];
        }

        input += g_width;
        source += job->sourcePitch;
    }

    if (g_scale == 3)
    {
        HQ3x scaler;

        scaler.resize(
            reinterpret_cast<const uint32_t*>(stripe->input),
            g_width, stripe->inputRows,
            reinterpret_cast<uint32_t*>(stripe->output),
            kThresholdY, kThresholdU, kThresholdV, kThresholdA,
            false, false);
    }
    else
    {
        HQ2x scaler;

        scaler.resize(
            reinterpret_cast<const uint32_t*>(stripe->input),
            g_width, stripe->inputRows,
            reinterpret_cast<uint32_t*>(stripe->output),
            kThresholdY, kThresholdU, kThresholdV, kThresholdA,
            false, false);
    }

    output = stripe->output + stripe->haloTop * g_scale * outputWidth;
    destination = job->destination +
        static_cast<LONG>(stripe->firstRow * g_scale) * job->destinationPitch;
    for (row = 0; row < stripe->rowCount * g_scale; ++row)
    {
        __movsd(
            reinterpret_cast<unsigned long*>(destination),
            reinterpret_cast<const unsigned long*>(output),
            outputWidth);
        output += outputWidth;
        destination += job->destinationPitch;
    }
}

static void ScaleTask(DWORD part, DWORD, void* context)
{
    if (part < g_stripeCount)
    {
        ProcessStripe(
            reinterpret_cast<const ScaleJob*>(context),
            &g_stripes[part]);
    }
}

static void FreeStripes()
{
    DWORD index;

    for (index = 0; index < kMaxStripes; ++index)
    {
        FreePixels(&g_stripes[index].input);
        FreePixels(&g_stripes[index].output);
    }

    g_stripeCount = 0;
}

BOOL HqxUpscalerInitialize(DWORD width, DWORD height, DWORD scale)
{
    DWORD count = WorkerPoolSize();
    DWORD index;

    HqxUpscalerShutdown();
    if (width == 0 || height == 0 || (scale != 2 && scale != 3))
    {
        return FALSE;
    }

    if (count > kMaxStripes)
    {
        count = kMaxStripes;
    }

    while (count > 1 && height / count < kMinStripeRows)
    {
        --count;
    }

    g_width = width;
    g_height = height;
    g_scale = scale;
    g_paletteValid = FALSE;
    g_stripeCount = count;
    for (index = 0; index < count; ++index)
    {
        Stripe* stripe = &g_stripes[index];
        DWORD haloBottom = index + 1 < count ? 1 : 0;

        stripe->firstRow = height * index / count;
        stripe->rowCount = height * (index + 1) / count - stripe->firstRow;
        stripe->haloTop = index > 0 ? 1 : 0;
        stripe->inputRows = stripe->haloTop + stripe->rowCount + haloBottom;
        stripe->input = AllocatePixels(width * stripe->inputRows);
        stripe->output = AllocatePixels(
            width * scale * stripe->inputRows * scale);
        if (stripe->input == NULL || stripe->output == NULL)
        {
            HqxUpscalerShutdown();
            return FALSE;
        }
    }

    return TRUE;
}

void HqxUpscalerShutdown()
{
    FreeStripes();
    g_width = 0;
    g_height = 0;
    g_scale = 0;
}

BOOL HqxUpscalerIsReady()
{
    return g_stripeCount != 0;
}

void HqxUpscalerSetPalette(const BYTE* entries)
{
    LONG y[256];
    LONG u[256];
    LONG v[256];
    BOOL changed = !g_paletteValid;
    DWORD index;
    DWORD other;

    for (index = 0; index < 256; ++index)
    {
        const BYTE* entry = entries + index * 4;
        BYTE* cached = g_paletteCache + index * 3;

        if (cached[0] != entry[0] ||
            cached[1] != entry[1] ||
            cached[2] != entry[2])
        {
            cached[0] = entry[0];
            cached[1] = entry[1];
            cached[2] = entry[2];
            changed = TRUE;
        }
    }

    if (!changed)
    {
        return;
    }

    for (index = 0; index < 256; ++index)
    {
        const BYTE* cached = g_paletteCache + index * 3;

        g_rgb[index] =
            (static_cast<DWORD>(cached[0]) << 16) |
            (static_cast<DWORD>(cached[1]) << 8) |
            static_cast<DWORD>(cached[2]);
        ComputeYuv(g_rgb[index], &y[index], &u[index], &v[index]);
    }

    for (index = 0; index < 256; ++index)
    {
        BYTE* row = g_different + (index << 8);

        for (other = 0; other < 256; ++other)
        {
            LONG dy = y[index] - y[other];
            LONG du = u[index] - u[other];
            LONG dv = v[index] - v[other];

            if (dy < 0) dy = -dy;
            if (du < 0) du = -du;
            if (dv < 0) dv = -dv;
            row[other] = static_cast<BYTE>(
                dy > static_cast<LONG>(kThresholdY) ||
                du > static_cast<LONG>(kThresholdU) ||
                dv > static_cast<LONG>(kThresholdV));
        }
    }

    g_paletteValid = TRUE;
}

void HqxUpscalerScale(
    const BYTE* source,
    LONG sourcePitch,
    BYTE* destination,
    LONG destinationPitch)
{
    ScaleJob job;

    if (g_stripeCount == 0)
    {
        return;
    }

    job.source = source;
    job.sourcePitch = sourcePitch;
    job.destination = destination;
    job.destinationPitch = destinationPitch;
    WorkerPoolRun(ScaleTask, &job);
}
