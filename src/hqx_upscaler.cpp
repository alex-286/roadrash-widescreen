#include "hqx_upscaler.h"

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

// The frame is scaled in horizontal stripes, one per processor (the calling
// thread takes the first). Each stripe is scaled with one extra source row
// above and below where it has a neighbor, so its 3x3 windows match those of
// the whole frame; the output of those halo rows is discarded.
static const DWORD kMaxStripes = 8;
static const DWORD kMinStripeRows = 16;
static const DWORD kWorkerStackSize = 64 * 1024;

struct Stripe
{
    DWORD firstRow;
    DWORD rowCount;
    DWORD haloTop;
    DWORD inputRows;
    DWORD* input;
    DWORD* output;
    HANDLE thread;
    HANDLE start;
};

static BYTE g_different[256 * 256];
static DWORD g_rgb[256];
static BYTE g_paletteCache[256 * 3];
static BOOL g_paletteValid = FALSE;
static DWORD g_width = 0;
static DWORD g_height = 0;
static DWORD g_scale = 0;
static Stripe g_stripes[kMaxStripes];
static HANDLE g_doneEvents[kMaxStripes];
static DWORD g_stripeCount = 0;
static volatile LONG g_quit = 0;

// The current job. It is written before the start events are set, which
// orders the writes before the workers read them.
static const BYTE* g_jobSource = NULL;
static LONG g_jobSourcePitch = 0;
static BYTE* g_jobDestination = NULL;
static LONG g_jobDestinationPitch = 0;

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

static void ProcessStripe(Stripe* stripe)
{
    DWORD outputWidth = g_width * g_scale;
    const BYTE* source = g_jobSource +
        static_cast<LONG>(stripe->firstRow - stripe->haloTop) * g_jobSourcePitch;
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
        source += g_jobSourcePitch;
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
    destination = g_jobDestination +
        static_cast<LONG>(stripe->firstRow * g_scale) * g_jobDestinationPitch;
    for (row = 0; row < stripe->rowCount * g_scale; ++row)
    {
        __movsd(
            reinterpret_cast<unsigned long*>(destination),
            reinterpret_cast<const unsigned long*>(output),
            outputWidth);
        output += outputWidth;
        destination += g_jobDestinationPitch;
    }
}

static DWORD WINAPI StripeWorker(LPVOID parameter)
{
    Stripe* stripe = reinterpret_cast<Stripe*>(parameter);
    DWORD index = static_cast<DWORD>(stripe - g_stripes);

    for (;;)
    {
        WaitForSingleObject(stripe->start, INFINITE);
        if (g_quit != 0)
        {
            break;
        }

        ProcessStripe(stripe);
        SetEvent(g_doneEvents[index]);
    }

    return 0;
}

static DWORD ChooseStripeCount(DWORD height)
{
    SYSTEM_INFO system;
    DWORD count;

    GetSystemInfo(&system);
    count = system.dwNumberOfProcessors;
    if (count > kMaxStripes)
    {
        count = kMaxStripes;
    }

    while (count > 1 && height / count < kMinStripeRows)
    {
        --count;
    }

    return count == 0 ? 1 : count;
}

static BOOL SetUpStripes(DWORD count)
{
    DWORD index;

    g_stripeCount = count;
    for (index = 0; index < count; ++index)
    {
        Stripe* stripe = &g_stripes[index];
        DWORD haloBottom;

        stripe->firstRow = g_height * index / count;
        stripe->rowCount = g_height * (index + 1) / count - stripe->firstRow;
        stripe->haloTop = index > 0 ? 1 : 0;
        haloBottom = index + 1 < count ? 1 : 0;
        stripe->inputRows = stripe->haloTop + stripe->rowCount + haloBottom;
        stripe->input = AllocatePixels(g_width * stripe->inputRows);
        stripe->output = AllocatePixels(
            g_width * g_scale * stripe->inputRows * g_scale);
        if (stripe->input == NULL || stripe->output == NULL)
        {
            return FALSE;
        }

        if (index == 0)
        {
            continue;
        }

        stripe->start = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_doneEvents[index] = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (stripe->start == NULL || g_doneEvents[index] == NULL)
        {
            return FALSE;
        }

        stripe->thread = CreateThread(
            NULL,
            kWorkerStackSize,
            StripeWorker,
            stripe,
            0,
            NULL);
        if (stripe->thread == NULL)
        {
            return FALSE;
        }
    }

    return TRUE;
}

static void TearDownStripes()
{
    DWORD index;

    g_quit = 1;
    for (index = 1; index < g_stripeCount; ++index)
    {
        Stripe* stripe = &g_stripes[index];

        if (stripe->thread != NULL)
        {
            SetEvent(stripe->start);
            WaitForSingleObject(stripe->thread, INFINITE);
            CloseHandle(stripe->thread);
        }
    }

    for (index = 0; index < kMaxStripes; ++index)
    {
        Stripe* stripe = &g_stripes[index];

        if (stripe->start != NULL)
        {
            CloseHandle(stripe->start);
        }

        if (g_doneEvents[index] != NULL)
        {
            CloseHandle(g_doneEvents[index]);
            g_doneEvents[index] = NULL;
        }

        FreePixels(&stripe->input);
        FreePixels(&stripe->output);
        stripe->thread = NULL;
        stripe->start = NULL;
    }

    g_stripeCount = 0;
    g_quit = 0;
}

BOOL HqxUpscalerInitialize(DWORD width, DWORD height, DWORD scale)
{
    DWORD count;

    HqxUpscalerShutdown();
    if (width == 0 || height == 0 || (scale != 2 && scale != 3))
    {
        return FALSE;
    }

    g_width = width;
    g_height = height;
    g_scale = scale;
    g_paletteValid = FALSE;

    // Fall back to a single stripe on the calling thread if workers cannot
    // be started.
    count = ChooseStripeCount(height);
    if (!SetUpStripes(count))
    {
        TearDownStripes();
        if (count == 1 || !SetUpStripes(1))
        {
            TearDownStripes();
            g_width = 0;
            g_height = 0;
            g_scale = 0;
            return FALSE;
        }
    }

    return TRUE;
}

DWORD HqxUpscalerThreadCount()
{
    return g_stripeCount;
}

void HqxUpscalerShutdown()
{
    TearDownStripes();
    g_width = 0;
    g_height = 0;
    g_scale = 0;
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
    DWORD index;

    if (g_stripeCount == 0)
    {
        return;
    }

    g_jobSource = source;
    g_jobSourcePitch = sourcePitch;
    g_jobDestination = destination;
    g_jobDestinationPitch = destinationPitch;
    for (index = 1; index < g_stripeCount; ++index)
    {
        SetEvent(g_stripes[index].start);
    }

    ProcessStripe(&g_stripes[0]);
    if (g_stripeCount > 1)
    {
        WaitForMultipleObjects(
            g_stripeCount - 1,
            g_doneEvents + 1,
            TRUE,
            INFINITE);
    }
}

void HqxUpscalerConvert(
    const BYTE* source,
    LONG sourcePitch,
    BYTE* destination,
    LONG destinationPitch)
{
    DWORD row;
    DWORD column;

    for (row = 0; row < g_height; ++row)
    {
        DWORD* output = reinterpret_cast<DWORD*>(destination);

        for (column = 0; column < g_width; ++column)
        {
            output[column] = g_rgb[source[column]];
        }

        source += sourcePitch;
        destination += destinationPitch;
    }
}
