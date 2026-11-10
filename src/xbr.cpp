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
enum Neighbor
{
    kE, kF, kH, kI, kB, kC, kD, kG, kF4, kI4, kH5, kI5, kNeighborCount
};

static const int kNeighborOffsets[kNeighborCount][2] =
{
    { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 },
    { 0, -1 }, { 1, -1 }, { -1, 0 }, { -1, 1 },
    { 2, 0 }, { 2, 1 }, { 0, 2 }, { 1, 2 }
};

// Subpixels a corner can touch, in canonical centered coordinates. For 2x
// the coordinates are -1 and 1 (subpixel centers at -0.5 and 0.5, doubled);
// for 3x they are -1, 0 and 1.
enum Subpixel
{
    kCorner,        // the corner subpixel
    kRightOfCorner, // 3x: right middle; 2x: top right
    kBelowCorner,   // 3x: bottom middle; 2x: bottom left
    kTopRight,      // 3x only
    kBottomLeft,    // 3x only
    kSubpixelCount
};

static const int kSubpixelOffsets2x[kSubpixelCount][2] =
{
    { 1, 1 }, { 1, -1 }, { -1, 1 }, { 1, -1 }, { -1, 1 }
};

static const int kSubpixelOffsets3x[kSubpixelCount][2] =
{
    { 1, 1 }, { 1, 0 }, { 0, 1 }, { 1, -1 }, { -1, 1 }
};

static const DWORD kMaxScale = 3;
static const LONG kSimilarY = 48;
static const LONG kSimilarU = 7;
static const LONG kSimilarV = 6;

struct ScaleJob
{
    const BYTE* source;
    LONG sourcePitch;
    BYTE* destination;
    LONG destinationPitch;
};

static DWORD g_width = 0;
static DWORD g_height = 0;
static DWORD g_scale = 0;
// Per rotation: each neighbor's index in the 5x5 window and each subpixel's
// index in the scale x scale output block.
static BYTE g_neighborIndex[4][kNeighborCount];
static BYTE g_subpixelIndex[4][kSubpixelCount];
// Palette-dependent tables, indexed by (first << 8) | second.
static DWORD g_rgb[256];
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
    const int (*subpixels)[2] = scale == 3 ? kSubpixelOffsets3x : kSubpixelOffsets2x;
    int rotation;
    int item;

    for (rotation = 0; rotation < 4; ++rotation)
    {
        for (item = 0; item < kNeighborCount; ++item)
        {
            int x;
            int y;

            Rotate(rotation, kNeighborOffsets[item][0], kNeighborOffsets[item][1], &x, &y);
            g_neighborIndex[rotation][item] = static_cast<BYTE>((y + 2) * 5 + (x + 2));
        }

        for (item = 0; item < kSubpixelCount; ++item)
        {
            int x;
            int y;

            Rotate(rotation, subpixels[item][0], subpixels[item][1], &x, &y);
            if (scale == 2)
            {
                x = (x + 1) / 2;
                y = (y + 1) / 2;
            }
            else
            {
                x += 1;
                y += 1;
            }

            g_subpixelIndex[rotation][item] = static_cast<BYTE>(y * scale + x);
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

static __forceinline BOOL Same(BYTE first, BYTE second)
{
    return g_rgb[first] == g_rgb[second];
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

static void ApplyCorner(const BYTE* window, int rotation, DWORD* block)
{
    const BYTE* at = g_neighborIndex[rotation];
    const BYTE* sub = g_subpixelIndex[rotation];
    BYTE e = window[at[kE]];
    BYTE f = window[at[kF]];
    BYTE h = window[at[kH]];
    BYTE i;
    BYTE b;
    BYTE c;
    BYTE d;
    BYTE g;
    BYTE f4;
    BYTE i4;
    BYTE h5;
    BYTE i5;
    DWORD edge;
    DWORD across;
    DWORD target;

    if (Same(e, f) || Same(e, h))
    {
        return;
    }

    i = window[at[kI]];
    b = window[at[kB]];
    c = window[at[kC]];
    d = window[at[kD]];
    g = window[at[kG]];
    f4 = window[at[kF4]];
    i4 = window[at[kI4]];
    h5 = window[at[kH5]];
    i5 = window[at[kI5]];

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
        BOOL left = 2 * slopeLeft <= slopeUp && !Same(e, g) && !Same(d, g);
        BOOL up = slopeLeft >= 2 * slopeUp && !Same(e, c) && !Same(b, c);

        if (g_scale == 3)
        {
            if (left && up)
            {
                block[sub[kBelowCorner]] = Blend(block[sub[kBelowCorner]], target, 192);
                block[sub[kBottomLeft]] = Blend(block[sub[kBottomLeft]], target, 64);
                block[sub[kRightOfCorner]] = Blend(block[sub[kRightOfCorner]], target, 192);
                block[sub[kTopRight]] = Blend(block[sub[kTopRight]], target, 64);
                block[sub[kCorner]] = target;
            }
            else if (left)
            {
                block[sub[kBelowCorner]] = Blend(block[sub[kBelowCorner]], target, 192);
                block[sub[kRightOfCorner]] = Blend(block[sub[kRightOfCorner]], target, 64);
                block[sub[kBottomLeft]] = Blend(block[sub[kBottomLeft]], target, 64);
                block[sub[kCorner]] = target;
            }
            else if (up)
            {
                block[sub[kRightOfCorner]] = Blend(block[sub[kRightOfCorner]], target, 192);
                block[sub[kBelowCorner]] = Blend(block[sub[kBelowCorner]], target, 64);
                block[sub[kTopRight]] = Blend(block[sub[kTopRight]], target, 64);
                block[sub[kCorner]] = target;
            }
            else
            {
                block[sub[kCorner]] = Blend(block[sub[kCorner]], target, 224);
                block[sub[kRightOfCorner]] = Blend(block[sub[kRightOfCorner]], target, 32);
                block[sub[kBelowCorner]] = Blend(block[sub[kBelowCorner]], target, 32);
            }
        }
        else
        {
            if (left && up)
            {
                block[sub[kCorner]] = Blend(block[sub[kCorner]], target, 224);
                block[sub[kBelowCorner]] = Blend(block[sub[kBelowCorner]], target, 64);
                block[sub[kRightOfCorner]] = Blend(block[sub[kRightOfCorner]], target, 64);
            }
            else if (left)
            {
                block[sub[kCorner]] = Blend(block[sub[kCorner]], target, 192);
                block[sub[kBelowCorner]] = Blend(block[sub[kBelowCorner]], target, 64);
            }
            else if (up)
            {
                block[sub[kCorner]] = Blend(block[sub[kCorner]], target, 192);
                block[sub[kRightOfCorner]] = Blend(block[sub[kRightOfCorner]], target, 64);
            }
            else
            {
                block[sub[kCorner]] = Blend(block[sub[kCorner]], target, 128);
            }
        }
    }
    else
    {
        block[sub[kCorner]] = Blend(block[sub[kCorner]], target, 128);
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
        const BYTE* lines[5];
        BYTE* output = job->destination +
            static_cast<LONG>(row * g_scale) * job->destinationPitch;
        DWORD column;
        int line;

        for (line = 0; line < 5; ++line)
        {
            LONG y = static_cast<LONG>(row) + line - 2;

            if (y < 0)
            {
                y = 0;
            }
            else if (y >= static_cast<LONG>(g_height))
            {
                y = static_cast<LONG>(g_height) - 1;
            }

            lines[line] = job->source + y * job->sourcePitch;
        }

        for (column = 0; column < g_width; ++column)
        {
            BYTE window[25];
            DWORD block[kMaxScale * kMaxScale];
            DWORD color;
            DWORD sub;
            int rotation;
            int x;
            BYTE center = lines[2][column];
            BYTE left = lines[2][column > 0 ? column - 1 : 0];
            BYTE right = lines[2][column + 1 < g_width ? column + 1 : column];

            // Every corner rule needs the center to differ from both of its
            // side neighbors; a pixel equal to all four keeps its color.
            if (Same(center, left) && Same(center, right) &&
                Same(center, lines[1][column]) && Same(center, lines[3][column]))
            {
                color = g_rgb[center];
                for (line = 0; line < static_cast<int>(g_scale); ++line)
                {
                    DWORD* pixels = reinterpret_cast<DWORD*>(
                        output + line * job->destinationPitch) + column * g_scale;

                    for (x = 0; x < static_cast<int>(g_scale); ++x)
                    {
                        pixels[x] = color;
                    }
                }

                continue;
            }

            for (x = 0; x < 5; ++x)
            {
                LONG sourceColumn = static_cast<LONG>(column) + x - 2;

                if (sourceColumn < 0)
                {
                    sourceColumn = 0;
                }
                else if (sourceColumn >= static_cast<LONG>(g_width))
                {
                    sourceColumn = static_cast<LONG>(g_width) - 1;
                }

                for (line = 0; line < 5; ++line)
                {
                    window[line * 5 + x] = lines[line][sourceColumn];
                }
            }

            color = g_rgb[window[12]];
            for (sub = 0; sub < g_scale * g_scale; ++sub)
            {
                block[sub] = color;
            }

            for (rotation = 0; rotation < 4; ++rotation)
            {
                ApplyCorner(window, rotation, block);
            }

            for (line = 0; line < static_cast<int>(g_scale); ++line)
            {
                DWORD* pixels = reinterpret_cast<DWORD*>(
                    output + line * job->destinationPitch) + column * g_scale;

                for (x = 0; x < static_cast<int>(g_scale); ++x)
                {
                    pixels[x] = block[line * g_scale + x];
                }
            }
        }
    }
}

BOOL XbrInitialize(DWORD width, DWORD height, DWORD scale)
{
    XbrShutdown();
    if (width == 0 || height == 0 || (scale != 2 && scale != 3))
    {
        return FALSE;
    }

    g_width = width;
    g_height = height;
    g_scale = scale;
    g_tablesValid = FALSE;
    BuildRotations(scale);
    return TRUE;
}

void XbrShutdown()
{
    g_width = 0;
    g_height = 0;
    g_scale = 0;
}

BOOL XbrIsReady()
{
    return g_scale != 0;
}

void XbrScale(
    const BYTE* source,
    LONG sourcePitch,
    const DWORD* rgb,
    BYTE* destination,
    LONG destinationPitch)
{
    ScaleJob job;
    DWORD index;

    if (g_scale == 0)
    {
        return;
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

    job.source = source;
    job.sourcePitch = sourcePitch;
    job.destination = destination;
    job.destinationPitch = destinationPitch;
    WorkerPoolRun(ScaleTask, &job);
}
