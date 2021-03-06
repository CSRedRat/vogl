/**************************************************************************
 *
 * Copyright 2014 LunarG, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * This file was originally authored by Ian Elliott (ian@lunarg.com).
 *
 **************************************************************************/

#if defined(_WIN32)
#include <windows.h>
#endif

#include <algorithm> // For std::max()
#include <assert.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include "pxfmt.h"

// The internal data structures and functions are put into the following
// unnamed namespace, so that they aren't externally visible to this file:
namespace
{

// Temporarily work-around not being in the VOGL source-code tree:
typedef unsigned char uint8;
typedef signed char int8;
typedef unsigned short uint16;
typedef signed short int16;
typedef unsigned int uint32;
typedef signed int int32;
typedef unsigned int uint;

/******************************************************************************
 *
 * Note: The following code creates a set of compile-time, templated struct's
 * that contain information needed to perform conversions to/from pixels in a
 * given pxfmt_sized_format.
 *
 * This allows the compiler to create, populate, and access something like a
 * table, using a pxfmt_sized_format as an index.  Yet, it's setup at
 * compile-time, not at run-time.  The use of the macro allows different data
 * types to be included in each struct, which are used in the functions that
 * come later in the file.
 *
 ******************************************************************************/

// The following struct contains the maximum value of (and as a result, a mask
// for) a certain number of bits:
template <bool is_signed, int nbits> struct max_values { };

#define MAX_VALUES(is_signed, nbits)                                    \
    template <> struct max_values<is_signed, nbits>                     \
    {                                                                   \
    public:                                                             \
        static const uint32 m_max =                                     \
            /* Calculate using a 64-bit integer, so that we can */      \
            /* shift left by 32 bits: */                                \
            (uint32) ((1ULL << ((!is_signed) ?                          \
                                /* The maximum value of unsigned */     \
                                /* integers is "(1 << nbits) - 1": */   \
                                (nbits) :                               \
                                /* The maximum value of signed */       \
                                /* integers is "(1 << (nbits-1)) - 1" */ \
                                /* unless nbits is 0 (which is only */  \
                                /* used at compile time): */            \
                                ((nbits) > 0) ? ((nbits) - 1) : 0)      \
                       /* Now, subtract one: */                         \
                       ) - 1);                                          \
    }


// Note: must define structs for "0" for compiling code below:
MAX_VALUES(false, 0);
MAX_VALUES(false, 1);
MAX_VALUES(false, 2);
MAX_VALUES(false, 3);
MAX_VALUES(false, 4);
MAX_VALUES(false, 5);
MAX_VALUES(false, 6);
MAX_VALUES(false, 8);
MAX_VALUES(false, 10);
MAX_VALUES(false, 16);
MAX_VALUES(false, 24);
MAX_VALUES(false, 32);
MAX_VALUES(true,  0);
MAX_VALUES(true,  8);
MAX_VALUES(true,  16);
MAX_VALUES(true,  32);



/******************************************************************************
 *
 * Note: The following code creates a set of compile-time, templated struct's
 * that contain information needed to perform conversions to/from pixels in a
 * given pxfmt_sized_format.
 *
 * This allows the compiler to create, populate, and access something like a
 * table, using a pxfmt_sized_format as an index.  Yet, it's setup at
 * compile-time, not at run-time.  The use of the macro allows different data
 * types to be included in each struct, which are used in the functions that
 * come later in the file.
 *
 ******************************************************************************/

// The following struct contains information used for the conversion of each
// pxfmt_sized_format (i.e. for each OpenGL format/type combination):
template <pxfmt_sized_format F> struct pxfmt_per_fmt_info { };

#define FMT_INFO(F, ftype, itype, ncomps, bypp,                         \
                 needfp, norm, is_signed, pack,                         \
                 in0, in1, in2, in3,                                    \
                 nbits0, nbits1, nbits2, nbits3,                        \
                 shift0, shift1, shift2, shift3)                        \
                                                                        \
    template <> struct pxfmt_per_fmt_info<F>                            \
    {                                                                   \
        static const pxfmt_sized_format m_fmt = F;                      \
        /* The type to access components and/or pixels of formatted data: */ \
        typedef ftype m_formatted_type;                                 \
        /* The type of intermediate values (convert to/from): */        \
        typedef itype m_intermediate_type;                              \
        static const uint32 m_num_components = ncomps;                  \
        static const uint32 m_bytes_per_pixel = bypp;                   \
                                                                        \
        static const bool m_needs_fp_intermediate = needfp;             \
        static const bool m_is_normalized = norm;                       \
        static const bool m_is_signed = is_signed;                      \
        static const bool m_is_packed = pack;                           \
                                                                        \
        /* The m_index[] members identify which "component index" */    \
        /* to use for the n'th component dealt with.  For example, */   \
        /* for GL_RGBA, component 0 (the first component converted) */  \
        /* has an index of 0 (i.e. red), where for GL_BGRA, component */ \
        /* 0 is 2 (i.e. blue). */                                       \
        static const int32 m_index[4];                                 \
                                                                        \
        /* The m_max[] members contain the "maximum value" to use for */ \
        /* the n'th component dealt with, and is used for converting */ \
        /* between fixed-point normalized integer values and. */        \
        /* floating-point values. */                                    \
        static const uint32 m_max[4];                                   \
                                                                        \
        /* The m_mask[] members contain how many bits to shift between */ \
        /* the n'th component's bottom bit, and the bottom bit of a */  \
        /* a uint32.  This is only for packed types. */                 \
        static const uint32 m_shift[4];                                 \
                                                                        \
        /* The m_mask[] members identify which bits to use for the */   \
        /* n'th component dealt with.  This is only for packed types. */ \
        static const uint32 m_mask[4];                                  \
    };                                                                  \
    const int32 pxfmt_per_fmt_info<F>::m_index[] = {in0, in1, in2, in3}; \
    const uint32 pxfmt_per_fmt_info<F>::m_max[] =                       \
        {max_values<is_signed,nbits0>::m_max,                           \
         max_values<is_signed,nbits1>::m_max,                           \
         max_values<is_signed,nbits2>::m_max,                           \
         max_values<is_signed,nbits3>::m_max};                          \
    const uint32 pxfmt_per_fmt_info<F>::m_shift[] =                     \
        {shift0, shift1, shift2, shift3};                               \
    const uint32 pxfmt_per_fmt_info<F>::m_mask[] =                      \
        {(max_values<is_signed,nbits0>::m_max << shift0),               \
         (max_values<is_signed,nbits1>::m_max << shift1),               \
         (max_values<is_signed,nbits2>::m_max << shift2),               \
         (max_values<is_signed,nbits3>::m_max << shift3)};


// FIXME: use awk sometime to pretty this up, and align the columns of
// information:

// GL_RED
FMT_INFO(PXFMT_R8_UNORM,  uint8,  double, 1, 1, true, true, false, false,       0, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R8_SNORM,  int8,   double, 1, 1, true, true, true, false,        0, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R16_UNORM, uint16, double, 1, 2, true, true, false, false,       0, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R16_SNORM, int16,  double, 1, 2, true, true, true, false,        0, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R32_UNORM, uint32, double, 1, 4, true, true, false, false,       0, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R32_SNORM, int32,  double, 1, 4, true, true, true, false,        0, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R32_FLOAT, float,  double, 1, 4, true, false, false, false,      0, -1, -1, -1,    0,  0,  0,  0,   0, 0, 0, 0);

// GL_GREEN
FMT_INFO(PXFMT_G8_UNORM,  uint8,  double, 1, 1, true, true, false, false,       1, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G8_SNORM,  int8,  double, 1, 1,  true, true, true, false,        1, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G16_UNORM, uint16, double, 1, 2,  true, true, false, false,      1, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G16_SNORM, int16,  double, 1, 2,  true, true, true, false,       1, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G32_UNORM, uint32, double, 1, 4, true, true, false, false,       1, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G32_SNORM, int32,  double, 1, 4, true, true, true, false,        1, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G32_FLOAT, float,  double, 1, 4, true, false, false, false,      1, -1, -1, -1,    0,  0,  0,  0,   0, 0, 0, 0);

// GL_BLUE
FMT_INFO(PXFMT_B8_UNORM,  uint8,  double, 1, 1, true, true, false, false,       2, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B8_SNORM,  int8,  double, 1, 1,  true, true, true, false,        2, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B16_UNORM, uint16, double, 1, 2,  true, true, false, false,      2, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B16_SNORM, int16,  double, 1, 2,  true, true, true, false,       2, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B32_UNORM, uint32, double, 1, 4, true, true, false, false,       2, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B32_SNORM, int32,  double, 1, 4, true, true, true, false,        2, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B32_FLOAT, float,  double, 1, 4, true, false, false, false,      2, -1, -1, -1,    0,  0,  0,  0,   0, 0, 0, 0);

// GL_ALPHA
FMT_INFO(PXFMT_A8_UNORM,  uint8,  double, 1, 1, true, true, false, false,       3, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A8_SNORM,  int8,   double, 1, 1, true, true, true, false,        3, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A16_UNORM, uint16, double, 1, 2, true, true, false, false,       3, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A16_SNORM, int16,  double, 1, 2, true, true, true, false,        3, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A32_UNORM, uint32, double, 1, 4, true, true, false, false,       3, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A32_SNORM, int32,  double, 1, 4, true, true, true, false,        3, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A32_FLOAT, float,  double, 1, 4, true, false, false, false,      3, -1, -1, -1,    0,  0,  0,  0,   0, 0, 0, 0);

// GL_RG
FMT_INFO(PXFMT_RG8_UNORM,  uint8,  double, 2, 2, true, true, false, false,      0, 1, -1, -1,     8,  8,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG8_SNORM,  int8,   double, 2, 2, true, true, true, false,       0, 1, -1, -1,     8,  8,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG16_UNORM, uint16, double, 2, 4, true, true, false, false,      0, 1, -1, -1,    16, 16,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG16_SNORM, int16,  double, 2, 4, true, true, true, false,       0, 1, -1, -1,    16, 16,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG32_UNORM, uint32, double, 2, 8, true, true, false, false,      0, 1, -1, -1,    32, 32,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG32_SNORM, int32,  double, 2, 8, true, true, true, false,       0, 1, -1, -1,    32, 32,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG32_FLOAT, float,  double, 2, 8, true, false, false, false,     0, 1, -1, -1,     0,  0,  0,  0,   0, 0, 0, 0);

// GL_RGB
FMT_INFO(PXFMT_RGB8_UNORM,  uint8,  double, 3, 3,  true, true, false, false,    0, 1, 2, -1,      8,  8,  8,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB8_SNORM,  int8,   double, 3, 3,  true, true, true, false,     0, 1, 2, -1,      8,  8,  8,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB16_UNORM, uint16, double, 3, 6,  true, true, false, false,    0, 1, 2, -1,     16, 16, 16,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB16_SNORM, int16,  double, 3, 6,  true, true, true, false,     0, 1, 2, -1,     16, 16, 16,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB32_UNORM, uint32, double, 3, 12, true, true, false, false,    0, 1, 2, -1,     32, 32, 32,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB32_SNORM, int32,  double, 3, 12, true, true, true, false,     0, 1, 2, -1,     32, 32, 32,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB32_FLOAT, float,  double, 3, 12, true, false, false, false,   0, 1, 2, -1,      0,  0,  0,  0,   0, 0, 0, 0);

FMT_INFO(PXFMT_RGB332_UNORM,  uint8,  double, 3, 1,  true, true, false, true,   0, 1, 2, -1,      3,  3,  2,  0,    5, 2, 0, 0);
FMT_INFO(PXFMT_RGB233_UNORM,  uint8,  double, 3, 1,  true, true, false, true,   0, 1, 2, -1,      3,  3,  2,  0,    0, 3, 6, 0);
FMT_INFO(PXFMT_RGB565_UNORM,  uint16, double, 3, 2,  true, true, false, true,   0, 1, 2, -1,      5,  6,  5,  0,   11, 5, 0, 0);
FMT_INFO(PXFMT_RGB565REV_UNORM,uint16,double, 3, 2,  true, true, false, true,   0, 1, 2, -1,      5,  6,  5,  0,   0, 5, 11, 0);

// GL_RGBA
FMT_INFO(PXFMT_RGBA8_UNORM,  uint32, double, 4, 4,  true, true, false, true,    0, 1, 2, 3,       8,  8,  8,  8,   0, 8, 16, 24);
FMT_INFO(PXFMT_RGBA8_SNORM,  uint32, double, 4, 4,  true, true, true, true,     0, 1, 2, 3,       8,  8,  8,  8,   0, 8, 16, 24);
FMT_INFO(PXFMT_RGBA16_UNORM, uint16, double, 4, 8,  true, true, false, false,   0, 1, 2, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA16_SNORM, int16,  double, 4, 8,  true, true, true, false,    0, 1, 2, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA32_UNORM, uint32, double, 4, 16, true, true, false, false,   0, 1, 2, 3,      32, 32, 32, 32,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA32_SNORM, int32,  double, 4, 16, true, true, true, false,    0, 1, 2, 3,      32, 32, 32, 32,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA32_FLOAT, float,  double, 4, 16, true, false, false, false,  0, 1, 2, 3,       0,  0,  0,  0,   0, 0, 0, 0);

FMT_INFO(PXFMT_RGBA4_UNORM,   uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       4,  4,  4,  4,  12, 8, 4, 0);
FMT_INFO(PXFMT_RGBA4REV_UNORM,uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       4,  4,  4,  4,  0, 4, 8, 12);
FMT_INFO(PXFMT_RGB5A1_UNORM,  uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       5,  5,  5,  1,  11, 6, 1, 0);
FMT_INFO(PXFMT_A1RGB5_UNORM,  uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       5,  5,  5,  1,  0, 5, 10, 15);
FMT_INFO(PXFMT_RGBA8REV_UNORM, uint32, double,  4, 4,  true, true, false, true, 0, 1, 2, 3,       8,  8,  8,  8,  24, 16, 8, 0);
FMT_INFO(PXFMT_RGB10A2_UNORM, uint32, double, 4, 4, true, true, false, true,    0, 1, 2, 3,      10, 10, 10,  2,  22, 12, 2, 0);
FMT_INFO(PXFMT_A2RGB10_UNORM, uint32, double, 4, 4, true, true, false, true,    0, 1, 2, 3,      10, 10, 10,  2,  0, 10, 20, 30);

// GL_BGRA
FMT_INFO(PXFMT_BGRA8_UNORM, uint32, double,  4, 4,  true, true, false, true,    0, 1, 2, 3,       8,  8,  8,  8,  16, 8, 0, 24);
FMT_INFO(PXFMT_BGRA8_SNORM, uint32, double,  4, 4,  true, true, true, true,     0, 1, 2, 3,       8,  8,  8,  8,  16, 8, 0, 24);
FMT_INFO(PXFMT_BGRA16_UNORM, uint16, double, 4, 8,  true, true, false, false,   2, 1, 0, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA16_SNORM, int16,  double, 4, 8,  true, true, true, false,    2, 1, 0, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA32_UNORM, uint32, double, 4, 16, true, true, false, false,   2, 1, 0, 3,      32, 32, 32, 32,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA32_SNORM, int32,  double, 4, 16, true, true, true, false,    2, 1, 0, 3,      32, 32, 32, 32,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA32_FLOAT, float, double, 4, 16,  true, false, false, false,  2, 1, 0, 3,       0,  0,  0,  0,   0, 0, 0, 0);

FMT_INFO(PXFMT_BGRA4_UNORM,   uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       4,  4,  4,  4,  4, 8, 12, 0);
FMT_INFO(PXFMT_BGRA4REV_UNORM,uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       4,  4,  4,  4,  0, 12, 8, 4);
FMT_INFO(PXFMT_BGR5A1_UNORM,  uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       5,  5,  5,  1,  1, 6, 11, 0);
FMT_INFO(PXFMT_A1BGR5_UNORM,  uint16, double, 4, 2, true, true, false, true,    0, 1, 2, 3,       5,  5,  5,  1,  10, 5, 0, 15);
FMT_INFO(PXFMT_BGRA8REV_UNORM,uint32, double, 4, 4,  true, true, false, true,   0, 1, 2, 3,       8,  8,  8,  8,  24, 0, 8, 16);
FMT_INFO(PXFMT_BGR10A2_UNORM, uint32, double, 4, 4, true, true, false, true,    0, 1, 2, 3,      10, 10, 10,  2,  2, 12, 22, 0);
FMT_INFO(PXFMT_A2BGR10_UNORM, uint32, double, 4, 4, true, true, false, true,    0, 1, 2, 3,      10, 10, 10,  2,  20, 10, 0, 30);

// GL_RED_INTEGER
FMT_INFO(PXFMT_R8_UINT,   uint8,  uint32, 1, 1, false, false, false, false,     0, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R8_SINT,   int8,   uint32, 1, 1, false, false, true, false,      0, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R16_UINT,  uint16, uint32, 1, 2, false, false, false, false,     0, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R16_SINT,  int16,  uint32, 1, 2, false, false, true, false,      0, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R32_UINT,  uint32, uint32, 1, 4, false, false, false, false,     0, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_R32_SINT,  int32,  uint32, 1, 4, false, false, true, false,      0, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);

// GL_GREEN_INTEGER
FMT_INFO(PXFMT_G8_UINT,   uint8,  uint32, 1, 1, false, false, false, false,     1, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G8_SINT,   int8,   uint32, 1, 1, false, false, true, false,      1, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G16_UINT,  uint16, uint32, 1, 2, false, false, false, false,     1, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G16_SINT,  int16,  uint32, 1, 2, false, false, true, false,      1, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G32_UINT,  uint32, uint32, 1, 4, false, false, false, false,     1, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_G32_SINT,  int32,  uint32, 1, 4, false, false, true, false,      1, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);

// GL_BLUE_INTEGER
FMT_INFO(PXFMT_B8_UINT,   uint8,  uint32, 1, 1, false, false, false, false,     2, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B8_SINT,   int8,   uint32, 1, 1, false, false, true, false,      2, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B16_UINT,  uint16, uint32, 1, 2, false, false, false, false,     2, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B16_SINT,  int16,  uint32, 1, 2, false, false, true, false,      2, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B32_UINT,  uint32, uint32, 1, 4, false, false, false, false,     2, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_B32_SINT,  int32,  uint32, 1, 4, false, false, true, false,      2, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);

// GL_ALPHA_INTEGER
FMT_INFO(PXFMT_A8_UINT,   uint8,  uint32, 1, 1, false, false, false, false,     3, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A8_SINT,   int8,   uint32, 1, 1, false, false, true, false,      3, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A16_UINT,  uint16, uint32, 1, 2, false, false, false, false,     3, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A16_SINT,  int16,  uint32, 1, 2, false, false, true, false,      3, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A32_UINT,  uint32, uint32, 1, 4, false, false, false, false,     3, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_A32_SINT,  int32,  uint32, 1, 4, false, false, true, false,      3, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);

// GL_RG_INTEGER
FMT_INFO(PXFMT_RG8_UINT,  uint8,  uint32, 2, 2, false, false, false, false,     0, 1, -1, -1,     8,  8,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG8_SINT,  int8,   uint32, 2, 2, false, false, true, false,      0, 1, -1, -1,     8,  8,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG16_UINT, uint16, uint32, 2, 4, false, false, false, false,     0, 1, -1, -1,    16, 16,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG16_SINT, int16,  uint32, 2, 4, false, false, true, false,      0, 1, -1, -1,    16, 16,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG32_UINT, uint32, uint32, 2, 8, false, false, false, false,     0, 1, -1, -1,    32, 32,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RG32_SINT, int32,  uint32, 2, 8, false, false, true, false,      0, 1, -1, -1,    32, 32,  0,  0,   0, 0, 0, 0);

// GL_RGB_INTEGER
FMT_INFO(PXFMT_RGB8_UINT,  uint8,  uint32, 3, 3,  false, false, false, false,   0, 1, 2, -1,      8,  8,  8,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB8_SINT,  int8,   uint32, 3, 3,  false, false, true, false,    0, 1, 2, -1,      8,  8,  8,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB16_UINT, uint16, uint32, 3, 6,  false, false, false, false,   0, 1, 2, -1,     16, 16, 16,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB16_SINT, int16,  uint32, 3, 6,  false, false, true, false,    0, 1, 2, -1,     16, 16, 16,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB32_UINT, uint32, uint32, 3, 12, false, false, false, false,   0, 1, 2, -1,     32, 32, 32,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGB32_SINT, int32,  uint32, 3, 12, false, false, true, false,    0, 1, 2, -1,     32, 32, 32,  0,   0, 0, 0, 0);

FMT_INFO(PXFMT_RGB332_UINT,  uint8,  uint32, 3, 1,  false, false, false, true,  0, 1, 2, -1,      3,  3,  2,  0,    5, 2, 0, 0);
FMT_INFO(PXFMT_RGB233_UINT,  uint8,  uint32, 3, 1,  false, false, false, true,  0, 1, 2, -1,      3,  3,  2,  0,    0, 3, 6, 0);
FMT_INFO(PXFMT_RGB565_UINT,  uint8,  uint32, 3, 2,  false, false, false, true,  0, 1, 2, -1,      5,  6,  5,  0,   11, 5, 0, 0);
FMT_INFO(PXFMT_RGB565REV_UINT,uint8, uint32, 3, 2,  false, false, false, true,  0, 1, 2, -1,      5,  6,  5,  0,   0, 5, 11, 0);

// GL_RGBA_INTEGER
FMT_INFO(PXFMT_RGBA8_UINT,  uint32, uint32, 4, 4,   false, false, false, true,  0, 1, 2, 3,       8,  8,  8,  8,   0, 8, 16, 24);
FMT_INFO(PXFMT_RGBA8_SINT,  uint32, uint32, 4, 4,   false, false, true, true,   0, 1, 2, 3,       8,  8,  8,  8,   0, 8, 16, 24);
FMT_INFO(PXFMT_RGBA16_UINT, uint16, uint32, 4, 8,   false, false, false, false, 0, 1, 2, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA16_SINT, int16,  uint32, 4, 8,   false, false, true, false,  0, 1, 2, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA32_UINT, uint32, uint32, 4, 16,  false, false, false, false, 0, 1, 2, 3,      32, 32, 32, 32,   0, 0, 0, 0);
FMT_INFO(PXFMT_RGBA32_SINT, int32,  uint32, 4, 16,  false, false, true, false,  0, 1, 2, 3,      32, 32, 32, 32,   0, 0, 0, 0);

FMT_INFO(PXFMT_RGBA4_UINT, uint16,  uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       4,  4,  4,  4,  12, 8, 4, 0);
FMT_INFO(PXFMT_RGBA4REV_UINT,uint16,uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       4,  4,  4,  4,  0, 4, 8, 12);
FMT_INFO(PXFMT_RGB5A1_UINT, uint16, uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       5,  5,  5,  1,  11, 6, 1, 0);
FMT_INFO(PXFMT_A1RGB5_UINT, uint16, uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       5,  5,  5,  1,  0, 5, 10, 15);
FMT_INFO(PXFMT_RGBA8REV_UINT, uint32, uint32,  4, 4,  false, false, false, true,0, 1, 2, 3,       8,  8,  8,  8,  24, 16, 8, 0);
FMT_INFO(PXFMT_RGB10A2_UINT, uint32, uint32, 4, 4,  false, false, false, true,  0, 1, 2, 3,      10, 10, 10,  2,  22, 12, 2, 0);
FMT_INFO(PXFMT_A2RGB10_UINT, uint32, uint32, 4, 4,  false, false, false, true,  0, 1, 2, 3,      10, 10, 10,  2,  0, 10, 20, 30);

// GL_BGRA_INTEGER
FMT_INFO(PXFMT_BGRA8_UINT,  uint32, uint32, 4, 4,   false, false, false, true,  0, 1, 2, 3,       8,  8,  8,  8,  16, 8, 0, 24);
FMT_INFO(PXFMT_BGRA8_SINT,  uint32, uint32, 4, 4,   false, false, true, true,   0, 1, 2, 3,       8,  8,  8,  8,  16, 8, 0, 24);
FMT_INFO(PXFMT_BGRA16_UINT, uint16, uint32, 4, 8,   false, false, false, false, 2, 1, 0, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA16_SINT, int16,  uint32, 4, 8,   false, false, true, false,  2, 1, 0, 3,      16, 16, 16, 16,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA32_UINT, uint32, uint32, 4, 16,  false, false, false, false, 2, 1, 0, 3,      32, 32, 32, 32,   0, 0, 0, 0);
FMT_INFO(PXFMT_BGRA32_SINT, int32,  uint32, 4, 16,  false, false, true, false,  2, 1, 0, 3,      32, 32, 32, 32,   0, 0, 0, 0);

FMT_INFO(PXFMT_BGRA4_UINT, uint16,  uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       4,  4,  4,  4,  4, 8, 12, 0);
FMT_INFO(PXFMT_BGRA4REV_UINT,uint16,uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       4,  4,  4,  4,  0, 12, 8, 4);
FMT_INFO(PXFMT_BGR5A1_UINT, uint16, uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       5,  5,  5,  1,  1, 6, 11, 0);
FMT_INFO(PXFMT_A1BGR5_UINT, uint16, uint32, 4, 2,  false, false, false, true,   0, 1, 2, 3,       5,  5,  5,  1,  10, 5, 0, 15);
FMT_INFO(PXFMT_BGRA8REV_UINT,uint32, uint32, 4, 4,  false, false, false, true,  0, 1, 2, 3,       8,  8,  8,  8,  24, 0, 8, 16);
FMT_INFO(PXFMT_BGR10A2_UINT, uint32, uint32, 4, 4,  false, false, false, true,  0, 1, 2, 3,      10, 10, 10,  2,  2, 12, 22, 0);
FMT_INFO(PXFMT_A2BGR10_UINT, uint32, uint32, 4, 4,  false, false, false, true,  0, 1, 2, 3,      10, 10, 10,  2,  20, 10, 0, 30);

// GL_DEPTH_COMPONENT
FMT_INFO(PXFMT_D8_UNORM,  uint8,  double, 1, 1, true, true, false, false,       0, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D8_SNORM,  int8,   double, 1, 1, true, true, true, false,        0, -1, -1, -1,    8,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D16_UNORM, uint16, double, 1, 2, true, true, false, false,       0, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D16_SNORM, int16,  double, 1, 2, true, true, true, false,        0, -1, -1, -1,   16,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D32_UNORM, uint32, double, 1, 4, true, true, false, false,       0, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D32_SNORM, int32,  double, 1, 4, true, true, true, false,        0, -1, -1, -1,   32,  0,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D32_FLOAT, float,  double, 1, 4, true, false, false, false,      0, -1, -1, -1,    0,  0,  0,  0,   0, 0, 0, 0);

// GL_STENCIL_INDEX
FMT_INFO(PXFMT_S8_UINT,   uint8,  uint32, 1, 1, false, false, false, false,     0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);
FMT_INFO(PXFMT_S8_SINT,   int8,   uint32, 1, 1, false, false, true, false,      0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);
FMT_INFO(PXFMT_S16_UINT,  uint16, uint32, 1, 2, false, false, false, false,     0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);
FMT_INFO(PXFMT_S16_SINT,  int16,  uint32, 1, 2, false, false, true, false,      0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);
FMT_INFO(PXFMT_S32_UINT,  uint32, uint32, 1, 4, false, false, false, false,     0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);
FMT_INFO(PXFMT_S32_SINT,  int32,  uint32, 1, 4, false, false, true, false,      0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);
FMT_INFO(PXFMT_S32_FLOAT, float,  uint32, 1, 4, false, false, false, false,     0, -1, -1, -1,   0, 0, 0, 0,   0, 0, 0, 0);

// GL_DEPTH_STENCIL - Note: These require special treatment; as a result not all of the values look correct:
FMT_INFO(PXFMT_D24_UNORM_S8_UINT, uint32, double, 2, 4, true, false, false, false, 0, 1, -1, -1,   24,  8,  0,  0,   0, 0, 0, 0);
FMT_INFO(PXFMT_D32_FLOAT_S8_UINT, float,  double, 2, 8, true, false, false, false, 0, 1, -1, -1,    0,  8,  0,  0,   0, 0, 0, 0);



/******************************************************************************
 *
 * NOTE: The same pattern repeats itself several times (starting here):
 *
 * 1) A templatized function
 *
 * 2) A function that converts from a run-time pxfmt_sized_format to a
 * compile-time templatized function for the specific pxfmt_sized_format.
 *
 ******************************************************************************/

// This templatized function determines the per-stride and per-row stride for
// the given pxfmt_sized_format and width.
template <pxfmt_sized_format F>
inline
void get_pxfmt_info(const uint32 width, uint32 &pixel_stride,
                    uint32 &row_stride, bool &needs_fp_intermediate)
{
    pixel_stride = pxfmt_per_fmt_info<F>::m_bytes_per_pixel;
    row_stride = ((pixel_stride * width) + 3) & 0xFFFFFFFC;
    needs_fp_intermediate = pxfmt_per_fmt_info<F>::m_needs_fp_intermediate;
}

inline
void get_pxfmt_info(const uint32 width, uint32 &pixel_stride,
                    uint32 &row_stride, bool &needs_fp_intermediate,
                    const pxfmt_sized_format fmt)
{
#ifdef CASE_STATEMENT
#undef CASE_STATEMENT
#endif
#define CASE_STATEMENT(fmt)                                             \
    case fmt:                                                           \
        get_pxfmt_info<fmt>(width, pixel_stride, row_stride,            \
                             needs_fp_intermediate);                    \
        break;

    switch (fmt)
    {
#include "pxfmt_case_statements.inl"
        case PXFMT_INVALID: break;
    }
}



/******************************************************************************
 *
 * The following are conversion functions for going from a pxfmt_sized_format
 * to an intermediate format (either double or uint32):
 *
 ******************************************************************************/

// This function sets the four components to default values of [0, 0, 0, 1]:
template <pxfmt_sized_format F>
inline
void set_intermediate_to_defaults(void *intermediate)
{
    // Point to the intermediate pixel values with the correct type:
    typename pxfmt_per_fmt_info<F>::m_intermediate_type *dst =
        (typename pxfmt_per_fmt_info<F>::m_intermediate_type *) intermediate;
    dst[0] = 0;
    dst[1] = 0;
    dst[2] = 0;
    dst[3] = 1;
}

// This function converts one component from a packed-integer value, (either
// integer or normalized-fixed-point) to an intermediate value (either a uint32
// or a double):
template <pxfmt_sized_format F, typename Tint, typename Tsrc>
inline
void to_int_comp_packed(Tint *dst, const Tsrc *src, const uint32 c)
{
    // Note: local variables are used in these functions in order to improve
    // readability and debug-ability.  The compiler should optimize things
    // appropriately.
    uint32 raw = (uint32) *src;
    uint32 index = pxfmt_per_fmt_info<F>::m_index[c];
    uint32 mask = pxfmt_per_fmt_info<F>::m_mask[c];
    uint32 shift = pxfmt_per_fmt_info<F>::m_shift[c];
    if (pxfmt_per_fmt_info<F>::m_is_normalized)
    {
        uint32 max = pxfmt_per_fmt_info<F>::m_max[c];

        raw = ((raw & mask) >> shift);
        dst[index] = (double) raw / (double) max;
        if (pxfmt_per_fmt_info<F>::m_is_signed)
        {
            dst[index] = std::max<double>(dst[index], -1.0);
        }
    }
    else
    {
        dst[index] = ((raw & mask) >> shift);
    }
}


// This function converts one component from a normalized-fixed-point value to
// an intermediate value, which is a double:
template <pxfmt_sized_format F, typename Tint, typename Tsrc>
inline
void to_int_comp_norm_unpacked(Tint *dst, const Tsrc *src, const uint32 c)
{
    uint32 raw = (uint32) src[c];
    uint32 index = pxfmt_per_fmt_info<F>::m_index[c];
    uint32 max = pxfmt_per_fmt_info<F>::m_max[c];
    dst[index] = (double) raw / (double) max;
    if (pxfmt_per_fmt_info<F>::m_is_signed)
    {
        dst[index] = std::max<double>(dst[index], -1.0);
    }
}


// This function performs a "copy conversion" (e.g. from a floating-point value
// to a double, or from an integer to a uint32):
template <pxfmt_sized_format F, typename Tint, typename Tsrc>
inline
void to_int_comp_copy(Tint *dst, const Tsrc *src, const uint32 c)
{
    uint32 index = pxfmt_per_fmt_info<F>::m_index[c];
    dst[index] = src[c];
}


// This special function converts all components of a source pixel to an
// intermediate format for a format of GL_DEPTH_STENCIL and a type of
// GL_UNSIGNED_INT_24_8:
template <pxfmt_sized_format F, typename Tint, typename Tsrc>
inline
void to_int_d24_unorm_s8_uint(Tint *dst, const Tsrc *src)
{
    // Convert the depth value:
    uint32 depth = *src;
    double *pDepthDst = (double *) &dst[0];
    *pDepthDst = (((double) ((depth & pxfmt_per_fmt_info<F>::m_mask[0]) >>
                             pxfmt_per_fmt_info<F>::m_shift[0])) /
                  ((double) pxfmt_per_fmt_info<F>::m_max[0]));

    // Convert the stencil value:
    uint32 stencil = *src;
    uint32 *pStencilDst = (uint32 *) &dst[1];
    *pStencilDst = ((stencil & pxfmt_per_fmt_info<F>::m_mask[1]) >>
                    pxfmt_per_fmt_info<F>::m_shift[1]);
}


// This special function converts all components of a source pixel to an
// intermediate format for a format of GL_DEPTH_STENCIL and a type of
// GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
template <pxfmt_sized_format F, typename Tint, typename Tsrc>
inline
void to_int_d32_float_s8_uint(Tint *dst, const Tsrc *src)
{
    // Convert the depth value:
    to_int_comp_copy<F>(dst, src, 0);

    // Convert the stencil value:
    uint32 *pStencilSrc = (uint32 *) &src[1];
    uint32 *pStencilDst = (uint32 *) &dst[1];
    *pStencilDst = ((*pStencilSrc & pxfmt_per_fmt_info<F>::m_mask[1]) >>
                    pxfmt_per_fmt_info<F>::m_shift[1]);
}


// This function converts all components of a source pixel to an intermediate
// format:
template <pxfmt_sized_format F>
inline
void to_intermediate(void *intermediate, const void *pSrc)
{
    // Point to the source pixel with the correct type:
    typename pxfmt_per_fmt_info<F>::m_formatted_type *src =
        (typename pxfmt_per_fmt_info<F>::m_formatted_type *) pSrc;

    // Point to the intermediate pixel values with the correct type:
    typename pxfmt_per_fmt_info<F>::m_intermediate_type *dst =
        (typename pxfmt_per_fmt_info<F>::m_intermediate_type *) intermediate;

    // Initialize the intermediate pixel values to the correct default values,
    // of the correct type:
    set_intermediate_to_defaults<F>(intermediate);

    // Note: The compiler should remove most of the code below, during
    // optimization, because (at compile time) it can be determined that it is
    // not needed for the given pxfmt_sized_format:

    if (pxfmt_per_fmt_info<F>::m_fmt == PXFMT_D24_UNORM_S8_UINT)
    {
        to_int_d24_unorm_s8_uint<F>(dst, src);
    }
    else if (pxfmt_per_fmt_info<F>::m_fmt == PXFMT_D32_FLOAT_S8_UINT)
    {
        to_int_d32_float_s8_uint<F>(dst, src);
    }
    else
    {
        // Convert this pixel to the intermediate, one component at a time:
        for (uint c = 0 ; c < pxfmt_per_fmt_info<F>::m_num_components; c++)
        {
            if (pxfmt_per_fmt_info<F>::m_index[c] >= 0)
            {
                if (pxfmt_per_fmt_info<F>::m_is_packed)
                {
                    to_int_comp_packed<F>(dst, src, c);
                }
                else
                {
                    if (pxfmt_per_fmt_info<F>::m_is_normalized)
                    {
                        to_int_comp_norm_unpacked<F>(dst, src, c);
                    }
                    else
                    {
                        to_int_comp_copy<F>(dst, src, c);
                    }
                }
            }
        }
    }
}


// This function converts a run-time call to a compile-time call to a function
// that converts all components of a source pixel to an intermediate format:
inline
void to_intermediate(void *intermediate, const void *pSrc,
                     const pxfmt_sized_format src_fmt)
{
#ifdef CASE_STATEMENT
#undef CASE_STATEMENT
#endif
#define CASE_STATEMENT(fmt)                                             \
    case fmt:                                                           \
        to_intermediate<fmt>(intermediate, pSrc);                       \
        break;

    switch (src_fmt)
    {
#include "pxfmt_case_statements.inl"
        case PXFMT_INVALID: break;
    }
}



/******************************************************************************
 *
 * The following are conversion functions for going from an intermediate format
 * (either double or uint32) to a pxfmt_sized_format:
 *
 ******************************************************************************/

// This function converts all components from an intermediate format (either
// uint32 or double) to a packed-integer (either with integer or
// normalized-fixed-point):
template <pxfmt_sized_format F, typename Tdst, typename Tint>
inline
void from_int_comp_packed(Tdst *dst, const Tint *src)
{
    Tint red =   ((pxfmt_per_fmt_info<F>::m_is_normalized) ?
                  (src[0] * pxfmt_per_fmt_info<F>::m_max[0]) : src[0]);
    Tint green = ((pxfmt_per_fmt_info<F>::m_is_normalized) ?
                  (src[1] * pxfmt_per_fmt_info<F>::m_max[1]) : src[1]);
    Tint blue =  ((pxfmt_per_fmt_info<F>::m_is_normalized) ?
                  (src[2] * pxfmt_per_fmt_info<F>::m_max[2]) : src[2]);
    Tint alpha = ((pxfmt_per_fmt_info<F>::m_is_normalized) ?
                  (src[3] * pxfmt_per_fmt_info<F>::m_max[3]) : src[3]);

    *dst = ((((uint32) red << pxfmt_per_fmt_info<F>::m_shift[0]) &
             pxfmt_per_fmt_info<F>::m_mask[0]) |
            (((uint32) green << pxfmt_per_fmt_info<F>::m_shift[1]) &
             pxfmt_per_fmt_info<F>::m_mask[1]) |
            (((uint32) blue << pxfmt_per_fmt_info<F>::m_shift[2]) &
             pxfmt_per_fmt_info<F>::m_mask[2]) |
            (((uint32) alpha << pxfmt_per_fmt_info<F>::m_shift[3]) &
             pxfmt_per_fmt_info<F>::m_mask[3]));
}


// This function converts one component from an intermediate format (double)
// to a normalized-fixed-point value:
template <pxfmt_sized_format F, typename Tdst, typename Tint>
inline
void from_int_comp_norm_unpacked(Tdst *dst, const Tint *src, const uint32 c)
{
    uint32 index = pxfmt_per_fmt_info<F>::m_index[c];
    uint32 raw = (uint32) src[index]; (void)raw;
    uint32 max = pxfmt_per_fmt_info<F>::m_max[c];

    dst[c] = (Tint) ((double) src[index] * (double) max);
}


// This function performs a "copy conversion" (e.g. from a floating-point value
// to a double, or from an integer to a uint32):
template <pxfmt_sized_format F, typename Tdst, typename Tint>
inline
void from_int_comp_copy(Tdst *dst, const Tint *src, const uint32 c)
{
    uint32 index = pxfmt_per_fmt_info<F>::m_index[c];
    dst[c] = (Tint) src[index];
}


// This special function converts all components of a source pixel from an
// intermediate format for a format of GL_DEPTH_STENCIL and a type of
// GL_UNSIGNED_INT_24_8:
template <pxfmt_sized_format F, typename Tdst, typename Tint>
inline
void from_int_d24_unorm_s8_uint(Tdst *dst, const Tint *src)
{
    // Convert the depth value:
    double depth_double = src[0] * (double) pxfmt_per_fmt_info<F>::m_max[0];
    uint32 depth_uint32 = (uint32) depth_double;
    depth_uint32 = ((depth_uint32 << pxfmt_per_fmt_info<F>::m_shift[0]) &
                    pxfmt_per_fmt_info<F>::m_mask[0]);

    // Convert the stencil value:
    uint32 *pStencilSrc = (uint32 *) &src[1];
    uint32 stencil = ((*pStencilSrc << pxfmt_per_fmt_info<F>::m_shift[1]) &
                      pxfmt_per_fmt_info<F>::m_mask[1]);

    // Combine the depth and stencil values into one, packed-integer value:
    *dst = depth_uint32 | stencil;
}


// This special function converts all components of a source pixel from an
// intermediate format for a format of GL_DEPTH_STENCIL and a type of
// GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
template <pxfmt_sized_format F, typename Tdst, typename Tint>
inline
void from_int_d32_float_s8_uint(Tdst *dst, const Tint *src)
{
    // Convert the depth value:
    from_int_comp_copy<F>(dst, src, 0);

    // Convert the stencil value:
    uint32 *pStencilSrc = (uint32 *) &src[1];
    uint32 *pStencilDst = (uint32 *) &dst[1];
    *pStencilDst = ((*pStencilSrc << pxfmt_per_fmt_info<F>::m_shift[1]) &
                    pxfmt_per_fmt_info<F>::m_mask[1]);
}


// This function converts all components of a destination pixel from an
// intermediate format:
template <pxfmt_sized_format F>
inline
void from_intermediate(void *pDst, const void *intermediate)
{
    // Point to the destination pixel, with the correct type:
    typename pxfmt_per_fmt_info<F>::m_formatted_type *dst =
        (typename pxfmt_per_fmt_info<F>::m_formatted_type *) pDst;

    // Point to the intermediate pixel values with the correct type:
    typename pxfmt_per_fmt_info<F>::m_intermediate_type *src =
        (typename pxfmt_per_fmt_info<F>::m_intermediate_type *) intermediate;

    // Note: The compiler should remove most of the code below, during
    // optimization, because (at compile time) it can be determined that it is
    // not needed for the given pxfmt_sized_format:

    // Convert this pixel from the intermediate, to the destination:
    if (pxfmt_per_fmt_info<F>::m_fmt == PXFMT_D24_UNORM_S8_UINT)
    {
        from_int_d24_unorm_s8_uint<F>(dst, src);
    }
    else if (pxfmt_per_fmt_info<F>::m_fmt == PXFMT_D32_FLOAT_S8_UINT)
    {
        from_int_d32_float_s8_uint<F>(dst, src);
    }
    else if (pxfmt_per_fmt_info<F>::m_is_packed)
    {
        // Convert all components of this pixel at the same time:
        from_int_comp_packed<F>(dst, src);
    }
    else
    {
        // Convert one component of this pixel at a time:
        for (uint c = 0 ; c < pxfmt_per_fmt_info<F>::m_num_components; c++)
        {
            if (pxfmt_per_fmt_info<F>::m_index[c] >= 0)
            {
                if (pxfmt_per_fmt_info<F>::m_is_normalized)
                {
                    from_int_comp_norm_unpacked<F>(dst, src, c);
                }
                else
                {
                    from_int_comp_copy<F>(dst, src, c);
                }
            }
        }
    }
}


// This function converts a run-time call to a compile-time call to a function
// that converts all components of a destination pixel from an intermediate
// format:
inline
void from_intermediate(void *pDst, const void *intermediate,
                       const pxfmt_sized_format dst_fmt)
{
#ifdef CASE_STATEMENT
#undef CASE_STATEMENT
#endif
#define CASE_STATEMENT(fmt)                                             \
    case fmt:                                                           \
        from_intermediate<fmt>(pDst, intermediate);                     \
        break;

    switch (dst_fmt)
    {
#include "pxfmt_case_statements.inl"
        case PXFMT_INVALID: break;
    }
}


} // unamed namespace



/******************************************************************************
 *
 * The following is an externally-visible function of this library:
 *
 ******************************************************************************/

// This function is used to get back a pxfmt_sized_format enum value for a
// given OpenGL "format" and "type".  If the format-type combination isn't
// supported, PXFMT_INVALID is returned.
pxfmt_sized_format validate_format_type_combo(const GLenum format,
                                              const GLenum type)
{
    switch (format)
    {
    case GL_RED:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_R8_UNORM;
        case GL_BYTE:
            return PXFMT_R8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_R16_UNORM;
        case GL_SHORT:
            return PXFMT_R16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_R32_UNORM;
        case GL_INT:
            return PXFMT_R32_SNORM;
        case GL_FLOAT:
            return PXFMT_R32_FLOAT;
        default:
            break;
        }
        break;
    case GL_GREEN:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_G8_UNORM;
        case GL_BYTE:
            return PXFMT_G8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_G16_UNORM;
        case GL_SHORT:
            return PXFMT_G16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_G32_UNORM;
        case GL_INT:
            return PXFMT_G32_SNORM;
        case GL_FLOAT:
            return PXFMT_G32_FLOAT;
        default:
            break;
        }
        break;
    case GL_BLUE:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_B8_UNORM;
        case GL_BYTE:
            return PXFMT_B8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_B16_UNORM;
        case GL_SHORT:
            return PXFMT_B16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_B32_UNORM;
        case GL_INT:
            return PXFMT_B32_SNORM;
        case GL_FLOAT:
            return PXFMT_B32_FLOAT;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_A8_UNORM;
        case GL_BYTE:
            return PXFMT_A8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_A16_UNORM;
        case GL_SHORT:
            return PXFMT_A16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_A32_UNORM;
        case GL_INT:
            return PXFMT_A32_SNORM;
        case GL_FLOAT:
            return PXFMT_A32_FLOAT;
        default:
            break;
        }
        break;
    case GL_RG:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_RG8_UNORM;
        case GL_BYTE:
            return PXFMT_RG8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_RG16_UNORM;
        case GL_SHORT:
            return PXFMT_RG16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_RG32_UNORM;
        case GL_INT:
            return PXFMT_RG32_SNORM;
        case GL_FLOAT:
            return PXFMT_RG32_FLOAT;
        default:
            break;
        }
        break;
    case GL_RGB:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_RGB8_UNORM;
        case GL_BYTE:
            return PXFMT_RGB8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_RGB16_UNORM;
        case GL_SHORT:
            return PXFMT_RGB16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_RGB32_UNORM;
        case GL_INT:
            return PXFMT_RGB32_SNORM;
        case GL_FLOAT:
            return PXFMT_RGB32_FLOAT;

        case GL_UNSIGNED_BYTE_3_3_2:
            return PXFMT_RGB332_UNORM;
        case GL_UNSIGNED_BYTE_2_3_3_REV:
            return PXFMT_RGB233_UNORM;
        case GL_UNSIGNED_SHORT_5_6_5:
            return PXFMT_RGB565_UNORM;
        case GL_UNSIGNED_SHORT_5_6_5_REV:
            return PXFMT_RGB565REV_UNORM;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_RGBA8_UNORM;
        case GL_BYTE:
            return PXFMT_RGBA8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_RGBA16_UNORM;
        case GL_SHORT:
            return PXFMT_RGBA16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_RGBA32_UNORM;
        case GL_INT:
            return PXFMT_RGBA32_SNORM;
        case GL_FLOAT:
            return PXFMT_RGBA32_FLOAT;

        case GL_UNSIGNED_SHORT_4_4_4_4:
            return PXFMT_RGBA4_UNORM;
        case GL_UNSIGNED_SHORT_4_4_4_4_REV:
            return PXFMT_RGBA4REV_UNORM;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return PXFMT_RGB5A1_UNORM;
        case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            return PXFMT_A1RGB5_UNORM;
        case GL_UNSIGNED_INT_8_8_8_8:
            return PXFMT_RGBA8_UNORM;
        case GL_UNSIGNED_INT_8_8_8_8_REV:
            return PXFMT_RGBA8REV_UNORM;
        case GL_UNSIGNED_INT_10_10_10_2:
            return PXFMT_RGB10A2_UNORM;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            return PXFMT_A2RGB10_UNORM;
        default:
            break;
        }
        break;
    case GL_BGRA:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_BGRA8_UNORM;
        case GL_BYTE:
            return PXFMT_BGRA8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_BGRA16_UNORM;
        case GL_SHORT:
            return PXFMT_BGRA16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_BGRA32_UNORM;
        case GL_INT:
            return PXFMT_BGRA32_SNORM;
        case GL_FLOAT:
            return PXFMT_BGRA32_FLOAT;

        case GL_UNSIGNED_SHORT_4_4_4_4:
            return PXFMT_BGRA4_UNORM;
        case GL_UNSIGNED_SHORT_4_4_4_4_REV:
            return PXFMT_BGRA4REV_UNORM;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return PXFMT_BGR5A1_UNORM;
        case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            return PXFMT_A1BGR5_UNORM;
        case GL_UNSIGNED_INT_8_8_8_8:
            return PXFMT_BGRA8_UNORM;
        case GL_UNSIGNED_INT_8_8_8_8_REV:
            return PXFMT_BGRA8REV_UNORM;
        case GL_UNSIGNED_INT_10_10_10_2:
            return PXFMT_BGR10A2_UNORM;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            return PXFMT_A2BGR10_UNORM;
        default:
            break;
        }
        break;
    case GL_RED_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_R8_UINT;
        case GL_BYTE:
            return PXFMT_R8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_R16_UINT;
        case GL_SHORT:
            return PXFMT_R16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_R32_UINT;
        case GL_INT:
            return PXFMT_R32_SINT;
        default:
            break;
        }
        break;
    case GL_GREEN_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_G8_UINT;
        case GL_BYTE:
            return PXFMT_G8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_G16_UINT;
        case GL_SHORT:
            return PXFMT_G16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_G32_UINT;
        case GL_INT:
            return PXFMT_G32_SINT;
        default:
            break;
        }
        break;
    case GL_BLUE_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_B8_UINT;
        case GL_BYTE:
            return PXFMT_B8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_B16_UINT;
        case GL_SHORT:
            return PXFMT_B16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_B32_UINT;
        case GL_INT:
            return PXFMT_B32_SINT;
        default:
            break;
        }
        break;
    case GL_ALPHA_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_A8_UINT;
        case GL_BYTE:
            return PXFMT_A8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_A16_UINT;
        case GL_SHORT:
            return PXFMT_A16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_A32_UINT;
        case GL_INT:
            return PXFMT_A32_SINT;
        default:
            break;
        }
        break;
    case GL_RG_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_RG8_UINT;
        case GL_BYTE:
            return PXFMT_RG8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_RG16_UINT;
        case GL_SHORT:
            return PXFMT_RG16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_RG32_UINT;
        case GL_INT:
            return PXFMT_RG32_SINT;
        default:
            break;
        }
        break;
    case GL_RGB_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_RGB8_UINT;
        case GL_BYTE:
            return PXFMT_RGB8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_RGB16_UINT;
        case GL_SHORT:
            return PXFMT_RGB16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_RGB32_UINT;
        case GL_INT:
            return PXFMT_RGB32_SINT;

        case GL_UNSIGNED_BYTE_3_3_2:
            return PXFMT_RGB332_UINT;
        case GL_UNSIGNED_BYTE_2_3_3_REV:
            return PXFMT_RGB233_UINT;
        case GL_UNSIGNED_SHORT_5_6_5:
            return PXFMT_RGB565_UINT;
        case GL_UNSIGNED_SHORT_5_6_5_REV:
            return PXFMT_RGB565REV_UINT;
        default:
            break;
        }
        break;
    case GL_RGBA_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_RGBA8_UINT;
        case GL_BYTE:
            return PXFMT_RGBA8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_RGBA16_UINT;
        case GL_SHORT:
            return PXFMT_RGBA16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_RGBA32_UINT;
        case GL_INT:
            return PXFMT_RGBA32_SINT;

        case GL_UNSIGNED_SHORT_4_4_4_4:
            return PXFMT_RGBA4_UINT;
        case GL_UNSIGNED_SHORT_4_4_4_4_REV:
            return PXFMT_RGBA4REV_UINT;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return PXFMT_RGB5A1_UINT;
        case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            return PXFMT_A1RGB5_UINT;
        case GL_UNSIGNED_INT_8_8_8_8:
            return PXFMT_RGBA8_UINT;
        case GL_UNSIGNED_INT_8_8_8_8_REV:
            return PXFMT_RGBA8REV_UINT;
        case GL_UNSIGNED_INT_10_10_10_2:
            return PXFMT_RGB10A2_UINT;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            return PXFMT_A2RGB10_UINT;
        default:
            break;
        }
    case GL_BGRA_INTEGER:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_BGRA8_UINT;
        case GL_BYTE:
            return PXFMT_BGRA8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_BGRA16_UINT;
        case GL_SHORT:
            return PXFMT_BGRA16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_BGRA32_UINT;
        case GL_INT:
            return PXFMT_BGRA32_SINT;

        case GL_UNSIGNED_SHORT_4_4_4_4:
            return PXFMT_BGRA4_UINT;
        case GL_UNSIGNED_SHORT_4_4_4_4_REV:
            return PXFMT_BGRA4REV_UINT;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return PXFMT_BGR5A1_UINT;
        case GL_UNSIGNED_SHORT_1_5_5_5_REV:
            return PXFMT_A1BGR5_UINT;
        case GL_UNSIGNED_INT_8_8_8_8:
            return PXFMT_BGRA8_UINT;
        case GL_UNSIGNED_INT_8_8_8_8_REV:
            return PXFMT_BGRA8REV_UINT;
        case GL_UNSIGNED_INT_10_10_10_2:
            return PXFMT_BGR10A2_UINT;
        case GL_UNSIGNED_INT_2_10_10_10_REV:
            return PXFMT_A2BGR10_UINT;
        default:
            break;
        }
        break;
    case GL_DEPTH_COMPONENT:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_D8_UNORM;
        case GL_BYTE:
            return PXFMT_D8_SNORM;
        case GL_UNSIGNED_SHORT:
            return PXFMT_D16_UNORM;
        case GL_SHORT:
            return PXFMT_D16_SNORM;
        case GL_UNSIGNED_INT:
            return PXFMT_D32_UNORM;
        case GL_INT:
            return PXFMT_D32_SNORM;
        case GL_FLOAT:
            return PXFMT_D32_FLOAT;
        default:
            break;
        }
        break;
    case GL_STENCIL_INDEX:
        switch (type)
        {
        case GL_UNSIGNED_BYTE:
            return PXFMT_S8_UINT;
        case GL_BYTE:
            return PXFMT_S8_SINT;
        case GL_UNSIGNED_SHORT:
            return PXFMT_S16_UINT;
        case GL_SHORT:
            return PXFMT_S16_SINT;
        case GL_UNSIGNED_INT:
            return PXFMT_S32_UINT;
        case GL_INT:
            return PXFMT_S32_SINT;
        case GL_FLOAT:
            return PXFMT_S32_FLOAT;
        default:
            break;
        }
        break;
    case GL_DEPTH_STENCIL:
        switch (type)
        {
        case GL_UNSIGNED_INT_24_8:
            return PXFMT_D24_UNORM_S8_UINT;
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            return PXFMT_D32_FLOAT_S8_UINT;
        default:
            break;
        }
        break;
    }
    // If we get to here, this is an unsupported format-type combination:
    return PXFMT_INVALID;
}



/******************************************************************************
 *
 * The following is an externally-visible function of this library:
 *
 ******************************************************************************/

// This function is used to convert a rectangular set of pixel data from one
// pxfmt_sized_format to another.  Size-wise, there are two types of
// "intermediate data" values (double and uint32, or 64-bit floating-poitn and
// 32-bit integers).  Data is converted from the source to a set of appropriate
// intermediate values, and then converted from that to the destination.  As
// long as the intermediate values contain enough precision, etc, values can be
// converted in a loss-less fashion.
//
// TODO: This function will eventually be able to handle mipmap levels,
// etc.  For now, all of the conversions are where the interesting action is
// at.
//
// TODO: Create another, similar function that will convert just one pixel
// (e.g. for the GUI).
//
// TODO: Add the "scale and bias" capability needed for the VOGL GUI.
//
// TBD: Does the VOGL GUI need the "scale and bias" capability only for
// floating-point and fixed-point data, or does it also need it for pure
// integer data?
void pxfmt_convert_pixels(void *pDst, const void *pSrc,
                          const int width, const int height,
                          const pxfmt_sized_format src_fmt,
                          const pxfmt_sized_format dst_fmt)
{
    // Use local pointers to the src and dst (both to increment within and
    // between rows) in order to properly deal with all strides:
    uint8 *src, *src_row;
    uint8 *dst, *dst_row;
    src = src_row = (uint8 *) pSrc;
    dst = dst_row = (uint8 *) pDst;

    // Get the per-pixel and per-row strides for both the src and dst:
    bool src_needs_fp_intermediate;
    uint32 src_pixel_stride;
    uint32 src_row_stride;
    bool dst_needs_fp_intermediate;
    uint32 dst_pixel_stride;
    uint32 dst_row_stride;
    get_pxfmt_info(width, src_pixel_stride, src_row_stride,
                    src_needs_fp_intermediate, src_fmt);
    get_pxfmt_info(width, dst_pixel_stride, dst_row_stride,
                    dst_needs_fp_intermediate, dst_fmt);
    assert(src_pixel_stride > 0);
    assert(src_row_stride > 0);
    assert(dst_pixel_stride > 0);
    assert(dst_row_stride > 0);
    assert(src_needs_fp_intermediate == dst_needs_fp_intermediate);


    if (src_needs_fp_intermediate)
    {
        // In order to handle 32-bit normalized values, we need to use
        // double-precision floating-point intermediate values:
        double intermediate[4];

        for (int y = 0 ; y < height ; y++)
        {
            for (int x = 0 ; x < height ; x++)
            {
                to_intermediate(intermediate, src, src_fmt);
                from_intermediate(dst, intermediate, dst_fmt);
                src += src_pixel_stride;
                dst += dst_pixel_stride;
            }
            src = src_row += src_row_stride;
            dst = dst_row += dst_row_stride;
        }
    }
    else
    {
        // The actual intermediate value can be uint32's, or int32's.  They are
        // the same size, and so at this level of the functionality, any will
        // do.  We'll use uint32's:
        uint32 intermediate[4];

        for (int y = 0 ; y < height ; y++)
        {
            for (int x = 0 ; x < height ; x++)
            {
                to_intermediate(intermediate, src, src_fmt);
                from_intermediate(dst, intermediate, dst_fmt);
                src += src_pixel_stride;
                dst += dst_pixel_stride;
            }
            src = src_row += src_row_stride;
            dst = dst_row += dst_row_stride;
        }
    }
}
