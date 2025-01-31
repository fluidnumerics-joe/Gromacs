/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2015,2016,2019,2020,2021, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */

#ifndef VECTYPE_OPS_HPP
#define VECTYPE_OPS_HPP

/**** float3 ****/
static __forceinline__ __host__ __device__ float3 make_float3(float s)
{
    return make_float3(s, s, s);
}
static __forceinline__ __host__ __device__ float3 make_float3(float4 a)
{
    return make_float3(a.x, a.y, a.z);
}
static __forceinline__ __host__ __device__ float norm(float3 a)
{
    return sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
static __forceinline__ __host__ __device__ float norm2(float3 a)
{
    return (a.x * a.x + a.y * a.y + a.z * a.z);
}
static __forceinline__ __host__ __device__ float dist3(float3 a, float3 b)
{
    return norm(b - a);
}
static __forceinline__ __device__ void atomicAdd(float3* addr, float3 val)
{
    atomicAdd(&addr->x, val.x);
    atomicAdd(&addr->y, val.y);
    atomicAdd(&addr->z, val.z);
}
/****************************************************************/

/**** float4 ****/
static __forceinline__ __host__ __device__ float4 make_float4(float s)
{
    return make_float4(s, s, s, s);
}
static __forceinline__ __host__ __device__ float4 make_float4(float3 a)
{
    return make_float4(a.x, a.y, a.z, 0.0F);
}
static __forceinline__ __host__ __device__ float4 operator+(float4 a, float3 b)
{
    return make_float4(a.x + b.x, a.y + b.y, a.z + b.z, a.w);
}

static __forceinline__ __host__ __device__ float norm(float4 a)
{
    return sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
}

static __forceinline__ __host__ __device__ float dist3(float4 a, float4 b)
{
    return norm(b - a);
}

/* \brief Compute the scalar product of two vectors.
 *
 * \param[in] a  First vector.
 * \param[in] b  Second vector.
 * \returns Scalar product.
 */
static __forceinline__ __device__ float iprod(const float3 a, const float3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/* \brief Compute the vector product of two vectors.
 *
 * \param[in] a  First vector.
 * \param[in] b  Second vector.
 * \returns Vector product.
 */
static __forceinline__ __device__ float3 cprod(const float3 a, const float3 b)
{
    float3 c;
    c.x = a.y * b.z - a.z * b.y;
    c.y = a.z * b.x - a.x * b.z;
    c.z = a.x * b.y - a.y * b.x;
    return c;
}

/* \brief Cosine of an angle between two vectors.
 *
 * Computes cosine using the following formula:
 *
 *                  ax*bx + ay*by + az*bz
 * cos-vec (a,b) =  ---------------------
 *                      ||a|| * ||b||
 *
 * This function also makes sure that the cosine does not leave the [-1, 1]
 * interval, which can happen due to numerical errors.
 *
 * \param[in] a  First vector.
 * \param[in] b  Second vector.
 * \returns Cosine between a and b.
 */
static __forceinline__ __device__ float cos_angle(const float3 a, const float3 b)
{
    float cosval;

    float ipa  = norm2(a);
    float ipb  = norm2(b);
    float ip   = iprod(a, b);
    float ipab = ipa * ipb;
    if (ipab > 0.0F)
    {
        cosval = ip * __frsqrt_rn(ipab);
    }
    else
    {
        cosval = 1.0F;
    }
    if (cosval > 1.0F)
    {
        return 1.0F;
    }
    if (cosval < -1.0F)
    {
        return -1.0F;
    }

    return cosval;
}

/* \brief Compute the angle between two vectors.
 *
 * Uses atan( |axb| / a.b ) formula.
 *
 * \param[in] a  First vector.
 * \param[in] b  Second vector.
 * \returns Angle between vectors in radians.
 */
static __forceinline__ __device__ float gmx_angle(const float3 a, const float3 b)
{
    float3 w = cprod(a, b);

    float wlen = norm(w);
    float s    = iprod(a, b);

    return atan2f(wlen, s); // requires float
}

/* \brief Atomically add components of the vector.
 *
 * Executes atomicAdd one-by-one on all components of the float3 vector.
 *
 * \param[in] a  First vector.
 * \param[in] b  Second vector.
 */
// NOLINTNEXTLINE(google-runtime-references)
static __forceinline__ __device__ void atomicAdd(float3& a, const float3 b)
{
    atomicAdd(&a.x, b.x);
    atomicAdd(&a.y, b.y);
    atomicAdd(&a.z, b.z);
}

/* Special implementation of float3 for faster computations using packed math on gfx90a.
 * HIP's float3 is defined as a struct of 3 fields, the compiler is not aware of its vector nature
 * hence it is not able to generate packed math instructions (v_pk_...) without SLP vectorization
 * (-fno-slp-vectorize). This new type is defined as struct of float2 (x, y) and float (z)
 * so packed math can be used for x and y.
 */
struct fast_float3
{
    typedef float __attribute__((ext_vector_type(2))) Native_float2_;

    union
    {
        struct __attribute__((packed)) { Native_float2_ dxy; float dz; };
        struct { float x, y, z; };
    };

    __host__ __device__
    fast_float3() = default;

    __host__ __device__
    fast_float3(float x_, float y_, float z_) : dxy{ x_, y_ }, dz{ z_ } {}

    __host__ __device__
    fast_float3(Native_float2_ xy_, float z_) : dxy{ xy_ }, dz{ z_ } {}

    __host__ __device__
    operator float3() const
    {
        return float3{ x, y, z };
    }

    __host__ __device__
    fast_float3& operator=(const fast_float3& x)
    {
        dxy = x.dxy;
        dz = x.dz;
        return *this;
    }
};
static_assert(sizeof(fast_float3) == 12);

__forceinline__ __host__ __device__
fast_float3 operator*(fast_float3 x, fast_float3 y)
{
    return fast_float3{ x.dxy * y.dxy, x.dz * y.dz };
}

__forceinline__ __host__ __device__
fast_float3 operator*(fast_float3 x, float y)
{
    return fast_float3{ x.dxy * y, x.dz * y };
}

__forceinline__ __host__ __device__
fast_float3 operator*(float x, fast_float3 y)
{
    return fast_float3{ x * y.dxy, x * y.dz };
}

__forceinline__ __host__ __device__
fast_float3 operator+(fast_float3 x, fast_float3 y)
{
    return fast_float3{ x.dxy + y.dxy, x.dz + y.dz };
}

__forceinline__ __host__ __device__
fast_float3 operator-(fast_float3 x, fast_float3 y)
{
    return fast_float3{ x.dxy - y.dxy, x.dz - y.dz };
}

static __forceinline__ __host__ __device__ fast_float3 make_fast_float3(float x)
{
    return fast_float3{ x, x, x };
}

static __forceinline__ __host__ __device__ fast_float3 make_fast_float3(float4 x)
{
    return fast_float3{ x.x, x.y, x.z };
}

static __forceinline__ __host__ __device__ float norm2(fast_float3 a)
{
    fast_float3 b = a * a;
    return (b.x + b.y + b.z);
}

#endif /* VECTYPE_OPS_HPP */
