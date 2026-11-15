// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alexander Schmidt

#include "xbr.h"
#include "worker_pool.h"

// The corner rule is written once for the bottom-right corner of a source
// pixel E, in canonical coordinates (x right, y down):
//
//          B  C
//       D  E  F  F4
//       G  H  I  I4
//          H5 I5
//
// Two weighted distance sums decide whether an edge runs along the H-F
// diagonal through the corner. If so, the output subpixels at the corner are
// blended toward F or H (whichever is closer to E); a further test detects
// shallow slopes ("left": the edge continues along the bottom, "up": along
// the right side), which cover more subpixels. The other three corners use
// the same rule with the neighborhood rotated by 90, 180 and 270 degrees.
//
// Speed (this runs on single-core Pentium 4 PCs too): the frame is first
// copied into a buffer with a two-pixel border of repeated edge pixels, so
// neighbors are read through fixed offsets without clamping. In that copy,
// palette indices are replaced by the first index of the same color, so
// "same color" is a byte compare. A rotation is just a pair of steps: one
// canonical step right and one down. Output is written in place.

// Subpixels a corner can touch, named in the canonical (bottom-right)
// orientation: the corner itself, then outward along the right column and
// the bottom row of the output block. Positions are subpixel centers in
// doubled units relative to the block center: 2x uses -1 and 1, 3x -2, 0
// and 2, 4x -3, -1, 1 and 3. Unused names of a scale repeat the corner.
enum Subpixel
{
    kCorner,
    kRightOfCorner, // right column, next to the corner
    kBelowCorner,   // bottom row, next to the corner
    kRightInner,    // right column, second from the corner (3x, 4x)
    kBottomInner,   // bottom row, second from the corner (3x, 4x)
    kRightOuter,    // right column, third from the corner (4x)
    kBottomOuter,   // bottom row, third from the corner (4x)
    kInner,         // diagonally inward from the corner (4x)
    kSubpixelCount
};

static const int kSubpixelOffsets2x[kSubpixelCount][2] =
{
    { 1, 1 }, { 1, -1 }, { -1, 1 },
    { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }, { 1, 1 }
};

static const int kSubpixelOffsets3x[kSubpixelCount][2] =
{
    { 2, 2 }, { 2, 0 }, { 0, 2 }, { 2, -2 }, { -2, 2 },
    { 2, 2 }, { 2, 2 }, { 2, 2 }
};

static const int kSubpixelOffsets4x[kSubpixelCount][2] =
{
    { 3, 3 }, { 3, 1 }, { 1, 3 }, { 3, -1 }, { -1, 3 },
    { 3, -3 }, { -3, 3 }, { 1, 1 }
};

static const DWORD kMaxScale = 4;
static const DWORD kBorder = 2;
static const DWORD kMaxParts = 8;
static const DWORD kNoRow = 0xFFFFFFFF;
static const LONG kSimilarY = 48;
static const LONG kSimilarU = 7;
static const LONG kSimilarV = 6;

// Byte offset of each subpixel from the top-left of its output block, per
// rotation, for one output pitch.
typedef LONG SubpixelOffsets[4][kSubpixelCount];

struct PadJob
{
    const BYTE* source;
    LONG sourcePitch;
};

struct ScaleJob
{
    BYTE* destination;
    LONG destinationPitch;
    SubpixelOffsets subpixelOffset;
};

// The output rows of one source row, kept for a worker part that reads the
// scaled frame row by row (XbrRow).
struct PartBlock
{
    DWORD* pixels;
    DWORD sourceRow;
};

static DWORD g_width = 0;
static DWORD g_height = 0;
static DWORD g_scale = 0;
// The frame with a kBorder-pixel border, in first-index-of-a-color form.
static BYTE* g_padded = NULL;
static LONG g_paddedPitch = 0;
// Per rotation: each subpixel's column and row in the output block.
static BYTE g_subpixelColumn[4][kSubpixelCount];
static BYTE g_subpixelRow[4][kSubpixelCount];
static PartBlock g_parts[kMaxParts];
static LONG g_blockPitch = 0;
static SubpixelOffsets g_blockOffsets;
// Palette-dependent tables, indexed by (first << 8) | second.
static DWORD g_rgb[256];
static BYTE g_firstOfColor[256];
static WORD g_distance[256 * 256];
static BYTE g_similar[256 * 256];
static BOOL g_tablesValid = FALSE;

// Rotates canonical coordinates to the corner of the given rotation:
// 0 bottom-right, 1 top-right, 2 top-left, 3 bottom-left.
static void Rotate(int rotation, int x, int y, int* outX, int* outY)
{
    switch (rotation)
    {
    case 1:
        *outX = y;
        *outY = -x;
        break;
    case 2:
        *outX = -x;
        *outY = -y;
        break;
    case 3:
        *outX = -y;
        *outY = x;
        break;
    default:
        *outX = x;
        *outY = y;
        break;
    }
}

static void BuildRotations(DWORD scale)
{
    const int (*subpixels)[2] =
        scale == 4 ? kSubpixelOffsets4x :
        scale == 3 ? kSubpixelOffsets3x : kSubpixelOffsets2x;
    int last = static_cast<int>(scale) - 1;
    int rotation;
    int item;

    for (rotation = 0; rotation < 4; ++rotation)
    {
        for (item = 0; item < kSubpixelCount; ++item)
        {
            int x;
            int y;

            // Doubled centered coordinates back to column and row.
            Rotate(rotation, subpixels[item][0], subpixels[item][1], &x, &y);
            g_subpixelColumn[rotation][item] = static_cast<BYTE>((x + last) / 2);
            g_subpixelRow[rotation][item] = static_cast<BYTE>((y + last) / 2);
        }
    }
}

static void ComputeYuv(DWORD rgb, LONG* y, LONG* u, LONG* v)
{
    LONG red = static_cast<LONG>((rgb >> 16) & 0xFF);
    LONG green = static_cast<LONG>((rgb >> 8) & 0xFF);
    LONG blue = static_cast<LONG>(rgb & 0xFF);

    *y = (299 * red + 587 * green + 114 * blue) / 1000;
    *u = (-169 * red - 331 * green + 500 * blue) / 1000 + 128;
    *v = (500 * red - 419 * green - 81 * blue) / 1000 + 128;
}

// Distance: YUV differences weighted 48:7:6, as in Hyllian's description.
// Similar: within the hqx thresholds.
static void BuildTables(const DWORD* rgb)
{
    LONG y[256];
    LONG u[256];
    LONG v[256];
    DWORD first;
    DWORD second;

    for (first = 0; first < 256; ++first)
    {
        g_rgb[first] = rgb[first] & 0x00FFFFFF;
        ComputeYuv(g_rgb[first], &y[first], &u[first], &v[first]);
        g_firstOfColor[first] = static_cast<BYTE>(first);
        for (second = 0; second < first; ++second)
        {
            if (g_rgb[second] == g_rgb[first])
            {
                g_firstOfColor[first] = static_cast<BYTE>(second);
                break;
            }
        }
    }

    for (first = 0; first < 256; ++first)
    {
        for (second = 0; second < 256; ++second)
        {
            LONG dy = y[first] - y[second];
            LONG du = u[first] - u[second];
            LONG dv = v[first] - v[second];
            DWORD index = (first << 8) | second;

            if (dy < 0) dy = -dy;
            if (du < 0) du = -du;
            if (dv < 0) dv = -dv;
            g_distance[index] = static_cast<WORD>(48 * dy + 7 * du + 6 * dv);
            g_similar[index] = static_cast<BYTE>(
                dy <= kSimilarY && du <= kSimilarU && dv <= kSimilarV);
        }
    }

    g_tablesValid = TRUE;
}

static __forceinline DWORD Distance(BYTE first, BYTE second)
{
    return g_distance[(static_cast<DWORD>(first) << 8) | second];
}

static __forceinline BOOL Similar(BYTE first, BYTE second)
{
    return g_similar[(static_cast<DWORD>(first) << 8) | second];
}

// Moves color toward target by weight/256.
static __forceinline DWORD Blend(DWORD color, DWORD target, DWORD weight)
{
    DWORD inverse = 256 - weight;
    DWORD redBlue =
        ((color & 0x00FF00FF) * inverse + (target & 0x00FF00FF) * weight) >> 8;
    DWORD green =
        ((color & 0x0000FF00) * inverse + (target & 0x0000FF00) * weight) >> 8;

    return (redBlue & 0x00FF00FF) | (green & 0x0000FF00);
}

#define XBR_PIXEL(name) \
    (*reinterpret_cast<DWORD*>(block + sub[name]))
#define XBR_SET(name) \
    (XBR_PIXEL(name) = target)
#define XBR_BLEND(name, weight) \
    (XBR_PIXEL(name) = Blend(XBR_PIXEL(name), target, weight))

// The corner rule for one rotation. p points at E in the bordered frame;
// right and down are the offsets of one canonical step. The caller has
// checked that E differs from F and from H. block is the top-left of the
// output block, sub the subpixel offsets of this rotation.
template <DWORD Scale>
static __forceinline void ApplyCorner(
    const BYTE* p,
    LONG right,
    LONG down,
    BYTE* block,
    const LONG* sub)
{
    BYTE e = p[0];
    BYTE f = p[right];
    BYTE h = p[down];
    BYTE i = p[right + down];
    BYTE b = p[-down];
    BYTE c = p[right - down];
    BYTE d = p[-right];
    BYTE g = p[down - right];
    BYTE f4 = p[2 * right];
    BYTE i4 = p[2 * right + down];
    BYTE h5 = p[2 * down];
    BYTE i5 = p[right + 2 * down];
    DWORD edge;
    DWORD across;
    DWORD target;

    // Color changes along the H-F diagonal against changes across it.
    edge = Distance(e, c) + Distance(e, g) + Distance(i, h5) +
        Distance(i, f4) + 4 * Distance(h, f);
    across = Distance(h, d) + Distance(h, i5) + Distance(f, i4) +
        Distance(f, b) + 4 * Distance(e, i);
    if (edge > across)
    {
        return;
    }

    target = Distance(e, f) <= Distance(e, h) ? g_rgb[f] : g_rgb[h];
    if (edge < across &&
        ((!Similar(f, b) && !Similar(h, d)) ||
         (Similar(e, i) && !Similar(f, i4) && !Similar(h, i5)) ||
         Similar(e, g) || Similar(e, c)))
    {
        DWORD slopeLeft = Distance(f, g);
        DWORD slopeUp = Distance(h, c);
        BOOL left = 2 * slopeLeft <= slopeUp && e != g && d != g;
        BOOL up = slopeLeft >= 2 * slopeUp && e != c && b != c;

        if (Scale == 4)
        {
            // "left" and "up" mirror each other across the diagonal.
            if (left && up)
            {
                XBR_BLEND(kBottomInner, 192);
                XBR_BLEND(kBottomOuter, 64);
                XBR_BLEND(kRightInner, 192);
                XBR_BLEND(kRightOuter, 64);
                XBR_BLEND(kInner, 64);
                XBR_SET(kCorner);
                XBR_SET(kBelowCorner);
                XBR_SET(kRightOfCorner);
            }
            else if (left)
            {
                XBR_BLEND(kRightOfCorner, 192);
                XBR_BLEND(kBottomInner, 192);
                XBR_BLEND(kInner, 64);
                XBR_BLEND(kBottomOuter, 64);
                XBR_SET(kBelowCorner);
                XBR_SET(kCorner);
            }
            else if (up)
            {
                XBR_BLEND(kBelowCorner, 192);
                XBR_BLEND(kRightInner, 192);
                XBR_BLEND(kInner, 64);
                XBR_BLEND(kRightOuter, 64);
                XBR_SET(kRightOfCorner);
                XBR_SET(kCorner);
            }
            else
            {
                XBR_BLEND(kRightOfCorner, 128);
                XBR_BLEND(kBelowCorner, 128);
                XBR_SET(kCorner);
            }
        }
        else if (Scale == 3)
        {
            if (left && up)
            {
                XBR_BLEND(kBelowCorner, 192);
                XBR_BLEND(kBottomInner, 64);
                XBR_BLEND(kRightOfCorner, 192);
                XBR_BLEND(kRightInner, 64);
                XBR_SET(kCorner);
            }
            else if (left)
            {
                XBR_BLEND(kBelowCorner, 192);
                XBR_BLEND(kRightOfCorner, 64);
                XBR_BLEND(kBottomInner, 64);
                XBR_SET(kCorner);
            }
            else if (up)
            {
                XBR_BLEND(kRightOfCorner, 192);
                XBR_BLEND(kBelowCorner, 64);
                XBR_BLEND(kRightInner, 64);
                XBR_SET(kCorner);
            }
            else
            {
                XBR_BLEND(kCorner, 224);
                XBR_BLEND(kRightOfCorner, 32);
                XBR_BLEND(kBelowCorner, 32);
            }
        }
        else
        {
            if (left && up)
            {
                XBR_BLEND(kCorner, 224);
                XBR_BLEND(kBelowCorner, 64);
                XBR_BLEND(kRightOfCorner, 64);
            }
            else if (left)
            {
                XBR_BLEND(kCorner, 192);
                XBR_BLEND(kBelowCorner, 64);
            }
            else if (up)
            {
                XBR_BLEND(kCorner, 192);
                XBR_BLEND(kRightOfCorner, 64);
            }
            else
            {
                XBR_BLEND(kCorner, 128);
            }
        }
    }
    else
    {
        XBR_BLEND(kCorner, 128);
    }
}

#undef XBR_SET
#undef XBR_BLEND
#undef XBR_PIXEL

// Copies source rows into the bordered frame (rows and columns beyond the
// edges repeat the edge pixels), mapping each index to the first index of
// its color.
static void PadTask(DWORD part, DWORD parts, void* context)
{
    const PadJob* job = reinterpret_cast<const PadJob*>(context);
    DWORD rows = g_height + 2 * kBorder;
    DWORD firstRow = rows * part / parts;
    DWORD lastRow = rows * (part + 1) / parts;
    DWORD row;

    for (row = firstRow; row < lastRow; ++row)
    {
        LONG sourceRow = static_cast<LONG>(row) - static_cast<LONG>(kBorder);
        const BYTE* line;
        BYTE* output = g_padded + static_cast<LONG>(row) * g_paddedPitch;
        DWORD column;
        BYTE edge;

        if (sourceRow < 0)
        {
            sourceRow = 0;
        }
        else if (sourceRow >= static_cast<LONG>(g_height))
        {
            sourceRow = static_cast<LONG>(g_height) - 1;
        }

        line = job->source + sourceRow * job->sourcePitch;
        edge = g_firstOfColor[line[0]];
        for (column = 0; column < kBorder; ++column)
        {
            output[column] = edge;
        }

        for (column = 0; column < g_width; ++column)
        {
            output[kBorder + column] = g_firstOfColor[line[column]];
        }

        edge = g_firstOfColor[line[g_width - 1]];
        for (column = 0; column < kBorder; ++column)
        {
            output[kBorder + g_width + column] = edge;
        }
    }
}

// Scales source row `row` into its Scale output rows, which start at block.
template <DWORD Scale>
static void ScaleRow(
    DWORD row,
    BYTE* block,
    LONG outputPitch,
    const SubpixelOffsets& offsets)
{
    LONG pitch = g_paddedPitch;
    const BYTE* p = g_padded +
        static_cast<LONG>(row + kBorder) * pitch + kBorder;
    DWORD column;

    for (column = 0; column < g_width; ++column, ++p, block += Scale * 4)
    {
        BYTE e = p[0];
        BOOL differsRight = e != p[1];
        BOOL differsDown = e != p[pitch];
        BOOL differsLeft = e != p[-1];
        BOOL differsUp = e != p[-pitch];
        DWORD color = g_rgb[e];
        BYTE* line = block;
        DWORD y;
        DWORD x;

        for (y = 0; y < Scale; ++y, line += outputPitch)
        {
            for (x = 0; x < Scale; ++x)
            {
                reinterpret_cast<DWORD*>(line)[x] = color;
            }
        }

        // Each corner rule needs E to differ from both of its side
        // neighbors (F and H in canonical terms).
        if (differsRight && differsDown)
        {
            ApplyCorner<Scale>(p, 1, pitch, block, offsets[0]);
        }

        if (differsUp && differsRight)
        {
            ApplyCorner<Scale>(p, -pitch, 1, block, offsets[1]);
        }

        if (differsLeft && differsUp)
        {
            ApplyCorner<Scale>(p, -1, -pitch, block, offsets[2]);
        }

        if (differsDown && differsLeft)
        {
            ApplyCorner<Scale>(p, pitch, -1, block, offsets[3]);
        }
    }
}

static void ScaleRowAnyScale(
    DWORD row,
    BYTE* block,
    LONG outputPitch,
    const SubpixelOffsets& offsets)
{
    switch (g_scale)
    {
    case 4:
        ScaleRow<4>(row, block, outputPitch, offsets);
        break;
    case 3:
        ScaleRow<3>(row, block, outputPitch, offsets);
        break;
    default:
        ScaleRow<2>(row, block, outputPitch, offsets);
        break;
    }
}

static void ScaleTask(DWORD part, DWORD parts, void* context)
{
    const ScaleJob* job = reinterpret_cast<const ScaleJob*>(context);
    DWORD firstRow = g_height * part / parts;
    DWORD lastRow = g_height * (part + 1) / parts;
    DWORD row;

    for (row = firstRow; row < lastRow; ++row)
    {
        ScaleRowAnyScale(
            row,
            job->destination +
                static_cast<LONG>(row * g_scale) * job->destinationPitch,
            job->destinationPitch,
            job->subpixelOffset);
    }
}

static void ComputeSubpixelOffsets(LONG outputPitch, SubpixelOffsets& offsets)
{
    int rotation;
    int item;

    for (rotation = 0; rotation < 4; ++rotation)
    {
        for (item = 0; item < kSubpixelCount; ++item)
        {
            offsets[rotation][item] =
                static_cast<LONG>(g_subpixelRow[rotation][item]) * outputPitch +
                static_cast<LONG>(g_subpixelColumn[rotation][item]) * 4;
        }
    }
}

BOOL XbrInitialize(DWORD width, DWORD height, DWORD scale)
{
    DWORD part;

    XbrShutdown();
    if (width == 0 || height == 0 || (scale < 2 || scale > kMaxScale))
    {
        return FALSE;
    }

    g_paddedPitch = static_cast<LONG>(width + 2 * kBorder);
    g_padded = reinterpret_cast<BYTE*>(VirtualAlloc(
        NULL,
        g_paddedPitch * (height + 2 * kBorder),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (g_padded == NULL)
    {
        return FALSE;
    }

    g_width = width;
    g_height = height;
    g_scale = scale;
    g_tablesValid = FALSE;
    BuildRotations(scale);
    g_blockPitch = static_cast<LONG>(width * scale * sizeof(DWORD));
    ComputeSubpixelOffsets(g_blockPitch, g_blockOffsets);
    for (part = 0; part < kMaxParts; ++part)
    {
        g_parts[part].sourceRow = kNoRow;
        g_parts[part].pixels = reinterpret_cast<DWORD*>(VirtualAlloc(
            NULL,
            g_blockPitch * scale,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_READWRITE));
        if (g_parts[part].pixels == NULL)
        {
            XbrShutdown();
            return FALSE;
        }
    }

    return TRUE;
}

void XbrShutdown()
{
    DWORD part;

    for (part = 0; part < kMaxParts; ++part)
    {
        if (g_parts[part].pixels != NULL)
        {
            VirtualFree(g_parts[part].pixels, 0, MEM_RELEASE);
            g_parts[part].pixels = NULL;
        }
    }

    if (g_padded != NULL)
    {
        VirtualFree(g_padded, 0, MEM_RELEASE);
        g_padded = NULL;
    }

    g_width = 0;
    g_height = 0;
    g_scale = 0;
}

BOOL XbrIsReady()
{
    return g_scale != 0;
}

BOOL XbrPrepareFrame(const BYTE* source, LONG sourcePitch, const DWORD* rgb)
{
    PadJob job;
    DWORD index;

    if (g_scale == 0)
    {
        return FALSE;
    }

    for (index = 0; index < 256 && g_tablesValid; ++index)
    {
        if (g_rgb[index] != (rgb[index] & 0x00FFFFFF))
        {
            g_tablesValid = FALSE;
        }
    }

    if (!g_tablesValid)
    {
        BuildTables(rgb);
    }

    for (index = 0; index < kMaxParts; ++index)
    {
        g_parts[index].sourceRow = kNoRow;
    }

    // Scaling a row reads up to two rows above and below it, so the
    // bordered frame is complete before any row is scaled.
    job.source = source;
    job.sourcePitch = sourcePitch;
    WorkerPoolRun(PadTask, &job);
    return TRUE;
}

const DWORD* XbrRow(void*, DWORD part, DWORD row)
{
    PartBlock* block = &g_parts[part < kMaxParts ? part : 0];
    DWORD sourceRow = row / g_scale;

    if (block->sourceRow != sourceRow)
    {
        ScaleRowAnyScale(
            sourceRow,
            reinterpret_cast<BYTE*>(block->pixels),
            g_blockPitch,
            g_blockOffsets);
        block->sourceRow = sourceRow;
    }

    return block->pixels + (row - sourceRow * g_scale) * g_width * g_scale;
}

void XbrScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch)
{
    ScaleJob job;

    if (!XbrPrepareFrame(source, sourcePitch, rgb))
    {
        return;
    }

    job.destination = destination;
    job.destinationPitch = destinationPitch;
    ComputeSubpixelOffsets(destinationPitch, job.subpixelOffset);
    WorkerPoolRun(ScaleTask, &job);
}
