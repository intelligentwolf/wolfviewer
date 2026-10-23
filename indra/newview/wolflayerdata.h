/**
 * @file wolflayerdata.h
 * @brief WolfViewer: an exact model of how a terrain patch's heights reach the viewer (OpenSim's LayerData encoder and this
 *        viewer's decoder), so the terrain writer can tell "the region set it" from "the region refused it".
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * WolfViewer — Wolf Territories Grid
 * Copyright (C) 2026 IntelligentWolf Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef WOLF_LAYERDATA_H
#define WOLF_LAYERDATA_H

// [WOLF TERRAFORM 2026-10-23] Paul's first Terrace on Sandbox ("Terraces 2.0 m apart, 25 x 35 m") was reported as 812 m² refused,
// e.g. at 784, 409: wanted 40.48 m, the ground 40.3 m - on his own region, whose estate limits are +-4100 m
// (westocean.regionsettings). The heights the viewer reads back are not the region's heights: each 16 x 16 patch is sent as a
// lossy DCT (OpenSim Region/Framework/Scenes/TerrainCompressor.cs CreatePatchFromTerrainData:155-188): scaled so the patch's
// range (Range = (int)(zmax - zmin + 1), :190-200) spans 1024 steps (prequant 10, CompressPatch:281-295), transformed
// (dct16x16:422-920) and each coefficient TRUNCATED (Net8Cast.ToInt32) after dividing by 8 (1 + 2 (u + v)) (the quantise
// table, BuildQuantizeTables16:299-317): the high frequencies in steps of up to 61. The viewer multiplies back and inverts
// (llmessage/patch_idct.cpp decompress_patch:590-636, idct_column / idct_line). A patch with sharp shelves (terraces, a
// beach's edge, a cut) has strong high frequencies, so its decoded heights can be tenths of a metre off what the region holds,
// while a smooth one is within millimetres - no fixed tolerance fits both, and the worst case from the coefficients' bounds
// alone is about the whole patch range. So the writer models the round trip: what the viewer WOULD decode if the region holds
// the heights it was sent, and compares that with what it did decode.
// The region itself stores and sets heights exactly (TerrainData.cs:271-283 float, FlattenArea.cs:47-58 sets map[x, y] =
// height at strength >= 0.999): the only loss is this patch coding.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace WolfLayerData
{
    constexpr int PATCH = 16;

    // Source: OpenSim Net8Cast.cs:65-69, :110 ToInt32(float): truncation toward zero when it fits, else int.MinValue.
    inline int osTrunc(float f)
    {
        const double d = (double)f;
        return (d > -2147483649.0 && d < 2147483648.0) ? (int)d : std::numeric_limits<int>::min();
    }

    // Source: TerrainCompressor.cs:299-317 BuildQuantizeTables16 (oosob = 2 / 16; quantise = oosob / (1 + 2 (i + j))).
    inline const float* osQuantizeTable16()
    {
        static float t[256];
        static bool made = false;
        if (!made)
        {
            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 16; i++) t[j * 16 + i] = (2.0f / 16) / (1.0f + 2.0f * (i + j));
            made = true;
        }
        return t;
    }

    // Source: TerrainCompressor.cs:320-375 BuildCopyMatrix16, and the viewer's own patch_idct.cpp:73-135 build_decopy_matrix
    // (the same zig-zag walk).
    inline void zigzag(int* m)
    {
        bool diag = false, right = true;
        int i = 0, j = 0, count = 0;
        while (i < 16 && j < 16)
        {
            m[j * 16 + i] = count++;
            if (!diag)
            {
                if (right) { if (i < 15) i++; else j++; right = false; diag = true; }
                else { if (j < 15) j++; else i++; right = true; diag = true; }
            }
            else
            {
                if (right) { i++; j--; if (i == 15 || j == 0) diag = false; }
                else { i--; j++; if (j == 15 || i == 0) diag = false; }
            }
        }
    }
    inline const int* osCopyMatrix16()
    {
        static int m[256];
        static bool made = false;
        if (!made) { zigzag(m); made = true; }
        return m;
    }

    // Source: TerrainCompressor.cs:422-920 dct16x16 (with its constants :400-420), translated line for line (C# float ->
    // float; Net8Cast.ToInt32 -> osTrunc; the fixed() blocks -> the tables above).
    constexpr float C16_1R = 0.35185093438159561476f * 2.82842712474619f;
    constexpr float C16_1I = 0.03465429229977286565f * 2.82842712474619f;
    constexpr float C16_2R = 0.34675996133053686546f * 2.82842712474619f;
    constexpr float C16_2I = 0.06897484482073575308f * 2.82842712474619f;
    constexpr float C16_3R = 0.33832950029358816957f * 2.82842712474619f;
    constexpr float C16_3I = 0.10263113188058934529f * 2.82842712474619f;
    constexpr float C16_4R = 0.32664074121909413196f * 2.82842712474619f;
    constexpr float C16_4I = 0.13529902503654924610f * 2.82842712474619f;
    constexpr float C16_5R = 0.31180625324666780814f * 2.82842712474619f;
    constexpr float C16_5I = 0.16666391461943662432f * 2.82842712474619f;
    constexpr float C16_6R = 0.29396890060483967924f * 2.82842712474619f;
    constexpr float C16_6I = 0.19642373959677554532f * 2.82842712474619f;
    constexpr float C16_7R = 0.27330046675043937206f * 2.82842712474619f;
    constexpr float C16_7I = 0.22429189658565907106f * 2.82842712474619f;
    constexpr float C16_8R = 0.25f * 2.82842712474619f;
    constexpr float W16_4R = 0.92387953251128675613f;
    constexpr float W16_4I = 0.38268343236508977173f;
    constexpr float W16_8R = 0.70710678118654752440f;
        inline void osDct16x16(const float* a, int* iout, int& wbits)
        {
            float tmp[256];

            float x0r, x0i, x1r, x1i, x2r, x2i, x3r, x3i;
            float x4r, x4i, x5r, x5i, x6r, x6i, x7r, x7i;
            float xr, xi;
            float ftmp;

            int itmp;
            int j, k;
            int indx;

            const int maxwbits = 17; // per header encoding
            int wbitsMaxValue = 1 << wbits;
            bool dowbits = wbits < 17;
            const float* fQuantizeTable16 = osQuantizeTable16();
            const int* fCopyMatrix16 = osCopyMatrix16();
            {
                for (j = 0, k = 0; j < 256; j += 16, k++)
                {
                    x4r = a[0 + j] - a[15 + j];
                    xr = a[0 + j] + a[15 + j];
                    x4i = a[8 + j] - a[7 + j];
                    xi = a[8 + j] + a[7 + j];
                    x0r = xr + xi;
                    x0i = xr - xi;
                    x5r = a[2 + j] - a[13 + j];
                    xr = a[2 + j] + a[13 + j];
                    x5i = a[10 + j] - a[5 + j];
                    xi = a[10 + j] + a[5 + j];
                    x1r = xr + xi;
                    x1i = xr - xi;
                    x6r = a[4 + j] - a[11 + j];
                    xr = a[4 + j] + a[11 + j];
                    x6i = a[12 + j] - a[3 + j];
                    xi = a[12 + j] + a[3 + j];
                    x2r = xr + xi;
                    x2i = xr - xi;
                    x7r = a[6 + j] - a[9 + j];
                    xr = a[6 + j] + a[9 + j];
                    x7i = a[14 + j] - a[1 + j];
                    xi = a[14 + j] + a[1 + j];
                    x3r = xr + xi;
                    x3i = xr - xi;
                    xr = x0r + x2r;
                    xi = x1r + x3r;
                    tmp[k] = C16_8R * (xr + xi); //
                    tmp[8 * 16 + k] = C16_8R * (xr - xi); //
                    xr = x0r - x2r;
                    xi = x1r - x3r;
                    tmp[4 * 16 + k] = C16_4R * xr - C16_4I * xi; //
                    tmp[12 * 16 + k] = C16_4R * xi + C16_4I * xr;  //
                    x0r = W16_8R * (x1i - x3i);
                    x2r = W16_8R * (x1i + x3i);
                    xr = x0i + x0r;
                    xi = x2r + x2i;
                    tmp[2 * 16 + k] = C16_2R * xr - C16_2I * xi;  //
                    tmp[14 * 16 + k] = C16_2R * xi + C16_2I * xr;  //
                    xr = x0i - x0r;
                    xi = x2r - x2i;
                    tmp[6 * 16 + k] = C16_6R * xr - C16_6I * xi;  //
                    tmp[10 * 16 + k] = C16_6R * xi + C16_6I * xr; //
                    xr = W16_8R * (x6r - x6i);
                    xi = W16_8R * (x6i + x6r);
                    x6r = x4r - xr;
                    x6i = x4i - xi;
                    x4r += xr;
                    x4i += xi;
                    xr = W16_4I * x7r - W16_4R * x7i;
                    xi = W16_4I * x7i + W16_4R * x7r;
                    x7r = W16_4R * x5r - W16_4I * x5i;
                    x7i = W16_4R * x5i + W16_4I * x5r;
                    x5r = x7r + xr;
                    x5i = x7i + xi;
                    x7r -= xr;
                    x7i -= xi;
                    xr = x4r + x5r;
                    xi = x5i + x4i;
                    tmp[16 + k] = C16_1R * xr - C16_1I * xi;  //
                    tmp[15 * 16 + k] = C16_1R * xi + C16_1I * xr;  //
                    xr = x4r - x5r;
                    xi = x5i - x4i;
                    tmp[7 * 16 + k] = C16_7R * xr - C16_7I * xi;  //
                    tmp[9 * 16 + k] = C16_7R * xi + C16_7I * xr;  //
                    xr = x6r - x7i;
                    xi = x7r + x6i;
                    tmp[5 * 16 + k] = C16_5R * xr - C16_5I * xi;  //
                    tmp[11 * 16 + k] = C16_5R * xi + C16_5I * xr;  //
                    xr = x6r + x7i;
                    xi = x7r - x6i;
                    tmp[3 * 16 + k] = C16_3R * xr - C16_3I * xi;  //
                    tmp[13 * 16 + k] = C16_3R * xi + C16_3I * xr;  //
                }

                for (j = 0, k = 0; j < 256; j += 16, k++)
                {
                    x4r = tmp[0 + j] - tmp[15 + j];
                    xr = tmp[0 + j] + tmp[15 + j];
                    x4i = tmp[8 + j] - tmp[7 + j];
                    xi = tmp[8 + j] + tmp[7 + j];
                    x0r = xr + xi;
                    x0i = xr - xi;
                    x5r = tmp[2 + j] - tmp[13 + j];
                    xr = tmp[2 + j] + tmp[13 + j];
                    x5i = tmp[10 + j] - tmp[5 + j];
                    xi = tmp[10 + j] + tmp[5 + j];
                    x1r = xr + xi;
                    x1i = xr - xi;
                    x6r = tmp[4 + j] - tmp[11 + j];
                    xr = tmp[4 + j] + tmp[11 + j];
                    x6i = tmp[12 + j] - tmp[3 + j];
                    xi = tmp[12 + j] + tmp[3 + j];
                    x2r = xr + xi;
                    x2i = xr - xi;
                    x7r = tmp[6 + j] - tmp[9 + j];
                    xr = tmp[6 + j] + tmp[9 + j];
                    x7i = tmp[14 + j] - tmp[1 + j];
                    xi = tmp[14 + j] + tmp[1 + j];
                    x3r = xr + xi;
                    x3i = xr - xi;
                    xr = x0r + x2r;
                    xi = x1r + x3r;

                    //tmp[0 + k] = C16_8R * (xr + xi); //
                    ftmp = C16_8R * (xr + xi);
                    itmp = osTrunc((ftmp * fQuantizeTable16[k]));
                    iout[fCopyMatrix16[k]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[8 * Constants.TerrainPatchSize + k] = C16_8R * (xr - xi); //
                    ftmp = C16_8R * (xr - xi);
                    indx = 8 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    xr = x0r - x2r;
                    xi = x1r - x3r;

                    //tmp[4 * Constants.TerrainPatchSize + k] = C16_4R * xr - C16_4I * xi; //
                    ftmp = C16_4R * xr - C16_4I * xi;
                    indx = 4 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[12 * Constants.TerrainPatchSize + k] = C16_4R * xi + C16_4I * xr;  //
                    ftmp = C16_4R * xi + C16_4I * xr;
                    indx = 12 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    x0r = W16_8R * (x1i - x3i);
                    x2r = W16_8R * (x1i + x3i);
                    xr = x0i + x0r;
                    xi = x2r + x2i;

                    //tmp[2 * Constants.TerrainPatchSize + k] = C16_2R * xr - C16_2I * xi;  //
                    ftmp = C16_2R * xr - C16_2I * xi;
                    indx = 2 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[14 * Constants.TerrainPatchSize + k] = C16_2R * xi + C16_2I * xr;  //
                    ftmp = C16_2R * xi + C16_2I * xr;
                    indx = 14 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    xr = x0i - x0r;
                    xi = x2r - x2i;

                    //tmp[6 * Constants.TerrainPatchSize + k] = C16_6R * xr - C16_6I * xi;  //
                    ftmp = C16_6R * xr - C16_6I * xi;
                    indx = 6 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[10 * Constants.TerrainPatchSize + k] = C16_6R * xi + C16_6I * xr; //
                    ftmp = C16_6R * xi + C16_6I * xr;
                    indx = 10 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    xr = W16_8R * (x6r - x6i);
                    xi = W16_8R * (x6i + x6r);
                    x6r = x4r - xr;
                    x6i = x4i - xi;
                    x4r += xr;
                    x4i += xi;
                    xr = W16_4I * x7r - W16_4R * x7i;
                    xi = W16_4I * x7i + W16_4R * x7r;
                    x7r = W16_4R * x5r - W16_4I * x5i;
                    x7i = W16_4R * x5i + W16_4I * x5r;
                    x5r = x7r + xr;
                    x5i = x7i + xi;
                    x7r -= xr;
                    x7i -= xi;
                    xr = x4r + x5r;
                    xi = x5i + x4i;

                    //tmp[1 * Constants.TerrainPatchSize + k] = C16_1R * xr - C16_1I * xi;  //
                    ftmp = C16_1R * xr - C16_1I * xi;
                    indx = 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[15 * Constants.TerrainPatchSize + k] = C16_1R * xi + C16_1I * xr;  //
                    ftmp = C16_1R * xi + C16_1I * xr;
                    indx = 15 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    xr = x4r - x5r;
                    xi = x5i - x4i;

                    //tmp[7 * Constants.TerrainPatchSize + k] = C16_7R * xr - C16_7I * xi;  //
                    ftmp = C16_7R * xr - C16_7I * xi;
                    indx = 7 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[9 * Constants.TerrainPatchSize + k] = C16_7R * xi + C16_7I * xr;  //
                    ftmp = C16_7R * xi + C16_7I * xr;
                    indx = 9 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    xr = x6r - x7i;
                    xi = x7r + x6i;

                    //tmp[5 * Constants.TerrainPatchSize + k] = C16_5R * xr - C16_5I * xi;  //
                    ftmp = C16_5R * xr - C16_5I * xi;
                    indx = 5 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[11 * Constants.TerrainPatchSize + k] = C16_5R * xi + C16_5I * xr;  //
                    ftmp = C16_5R * xi + C16_5I * xr;
                    indx = 11 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    xr = x6r + x7i;
                    xi = x7r - x6i;

                    //tmp[3 * Constants.TerrainPatchSize + k] = C16_3R * xr - C16_3I * xi;  //
                    ftmp = C16_3R * xr - C16_3I * xi;
                    indx = 3 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }

                    //tmp[13 * Constants.TerrainPatchSize + k] = C16_3R * xi + C16_3I * xr;  //
                    ftmp = C16_3R * xi + C16_3I * xr;
                    indx = 13 * 16 + k;
                    itmp = osTrunc((ftmp * fQuantizeTable16[indx]));
                    iout[fCopyMatrix16[indx]] = itmp;

                    if (dowbits)
                    {
                        if (itmp < 0) itmp *= -1;
                        while (itmp > wbitsMaxValue)
                        {
                            wbits++;
                            wbitsMaxValue = 1 << wbits;
                            if (wbits == maxwbits)
                            {
                                dowbits = false;
                                break;
                            }
                        }
                    }
                }
            }
        }

    /** The heights this viewer decodes for a patch whose region heights are `h` (row by row, y outer, x inner, as
     *  TerrainData.cs:602-611 GetPatchBlock). Source: TerrainCompressor.cs:155-295 (encoder) and llmessage/patch_idct.cpp
     *  :40-70 (tables), :307-345 idct_column, :180-230 idct_line, :445-505 idct_patch, :590-636 decompress_patch. */
    inline void roundTrip(const float* h, float* out)
    {
        // PrescanPatch:190-200 (GetPatchMinMax, TerrainData.cs:585-599)
        float zmin = std::numeric_limits<float>::max(), zmax = std::numeric_limits<float>::lowest();
        for (int k = 0; k < 256; ++k) { zmin = std::fmin(zmin, h[k]); zmax = std::fmax(zmax, h[k]); }
        const float frange = (zmax - zmin) + 1.0f;
        // :162-177 a flat patch: QuantWBits 0, DCOffset - 0.5, range 1, no coefficients; the viewer then gives
        // mult * (1 << (prequant - 1)) + hmin = (1 / 4) * 2 + zmin - 0.5 = zmin everywhere
        if (std::round((double)frange * 100.0) / 100.0 == 1.0)
        {
            for (int k = 0; k < 256; ++k) out[k] = zmin;
            return;
        }
        const int range = osTrunc(frange);
        // CompressPatch:281-295, prequant 10
        float sub = (float)range;
        const float premult = (float)(1 << 10) / sub;
        sub = 0.5f * sub + zmin;
        float block[256];
        for (int k = 0; k < 256; ++k) block[k] = (h[k] - sub) * premult;
        int iout[256];
        int wbits = 10 >> 1;
        osDct16x16(block, iout, wbits);
        // EncodePatchHeader:203-217 clamps wbits to 2..17; EncodePatch:219-279 sends min(|v|, (1 << wbits) - 1) with its sign
        if (wbits > 17) wbits = 17; else if (wbits < 2) wbits = 2;
        const int maxv = (1 << wbits) - 1;
        for (int k = 0; k < 256; ++k)
        {
            if (iout[k] > maxv) iout[k] = maxv;
            else if (iout[k] < -maxv) iout[k] = -maxv;
        }
        // the viewer: decompress_patch:590-636
        static float dq[256], icos[256];
        static int decopy[256];
        static bool made = false;
        if (!made)
        {
            for (int j = 0; j < 16; j++)
                for (int i = 0; i < 16; i++) dq[j * 16 + i] = (1.f + 2.f * (i + j));
            const float oosob = 3.14159265358979323846f * 0.5f / 16;   // patch_idct.cpp:60 F_PI * 0.5f / size
            for (int u = 0; u < 16; u++)
                for (int n = 0; n < 16; n++) icos[u * 16 + n] = std::cos((2.f * n + 1.f) * u * oosob);
            zigzag(decopy);
            made = true;
        }
        const int prequant = (((10 - 2) & 0x0f) | (((10 - 2) & 0x0f) << 4)) >> 4;   // QuantWBits high nibble = 8
        const int quantize = 1 << (prequant + 2);
        const float mult = (1.f / (float)quantize) * (float)range;
        const float addval = mult * (float)(1 << (prequant + 2 - 1)) + zmin;
        float b[256], t[256];
        for (int k = 0; k < 256; ++k) b[k] = (float)iout[decopy[k]] * dq[k];
        const float OO_SQRT2 = 0.7071067811865475244008443621049f;   // llmath.h OO_SQRT2
        for (int c = 0; c < 16; ++c)       // idct_column: no scale
            for (int n = 0; n < 16; ++n)
            {
                float total = OO_SQRT2 * b[c];
                for (int u = 1; u < 16; ++u) total += b[u * 16 + c] * icos[u * 16 + n];
                t[(n << 4) + c] = total;
            }
        for (int line = 0; line < 16; ++line)   // idct_line: * 2 / 16
            for (int n = 0; n < 16; ++n)
            {
                float total = OO_SQRT2 * t[line * 16];
                for (int u = 1; u < 16; ++u) total += t[line * 16 + u] * icos[u * 16 + n];
                out[line * 16 + n] = total * (2.f / 16.f) * mult + addval;
            }
    }

    /** Heights that decode to `decoded` where `fixed` is false, holding the cells where it is true at `value`: the decoded
     *  heights are the start, then each round trip's difference is fed back FIT_ROUNDS times (the coding is close to the
     *  identity, so this settles; tests/wolfterraformmath_test.cpp measures the fit). */
    constexpr int FIT_ROUNDS = 10;
    inline void fit(const float* decoded, const float* value, const bool* fixed, float* heights)
    {
        float sim[256];
        for (int k = 0; k < 256; ++k) heights[k] = fixed[k] ? value[k] : decoded[k];
        for (int r = 0; r < FIT_ROUNDS; ++r)
        {
            roundTrip(heights, sim);
            for (int k = 0; k < 256; ++k) if (!fixed[k]) heights[k] += decoded[k] - sim[k];
        }
    }

    /**
     * Which of a patch's written cells the region took, judged from what this viewer decoded after the write (`decoded`).
     * `sent` = the height each written cell was sent (NaN: not written); `before` = what this viewer decoded for the patch
     * before the write. Two explanations are modelled - the region holds the sent heights ("took"), or it kept its old ones
     * (estimated from `before`) - and each written cell goes to whichever its decoded height is nearer. A cell within `tol_near`
     * of its sent height counts as taken without the model (the writer's old test). A fixed tolerance cannot do this: a smooth
     * patch decodes within a few centimetres, a terraced one up to half a metre off (tests/wolfterraformmath_test.cpp).
     */
    inline void judge(const float* decoded, const float* sent, const float* before, float tol_near, bool* took)
    {
        bool fixed[256], none[256];
        float old_heights[256], h_took[256], h_kept[256], d_took[256], d_kept[256];
        for (int k = 0; k < 256; ++k) { fixed[k] = !std::isnan(sent[k]); none[k] = false; }
        fit(before, before, none, old_heights);
        fit(decoded, sent, fixed, h_took);
        fit(decoded, old_heights, fixed, h_kept);
        roundTrip(h_took, d_took);
        roundTrip(h_kept, d_kept);
        for (int k = 0; k < 256; ++k)
        {
            if (!fixed[k]) { took[k] = true; continue; }
            took[k] = std::fabs(decoded[k] - sent[k]) <= tol_near || std::fabs(decoded[k] - d_took[k]) <= std::fabs(decoded[k] - d_kept[k]);
        }
    }
}

#endif // WOLF_LAYERDATA_H
