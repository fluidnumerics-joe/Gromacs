/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2014,2015,2016,2017 by the GROMACS development team.
 * Copyright (c) 2018,2019,2020,2021, by the GROMACS development team, led by
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
#ifndef HIP_ARCH_UTILS_HPP_
#define HIP_ARCH_UTILS_HPP_

#include "gromacs/utility/basedefinitions.h"

/*! \file
 *  \brief HIP arch dependent definitions.
 *
 *  \author Szilard Pall <pall.szilard@gmail.com>
 */

/* GMX_PTX_ARCH is set to the virtual arch (PTX) version targeted by
 * the current compiler pass or zero for the host pass and it is
 * intended to be used instead of __HIP_ARCH__.
 */
//#ifndef __HIP_ARCH__
//#    define GMX_PTX_ARCH 0
//#else
//#    define GMX_PTX_ARCH __HIP_ARCH__
//#endif

/* Until CC 5.2 and likely for the near future all NVIDIA architectures
   have a warp size of 32, but this could change later. If it does, the
   following constants should depend on the value of GMX_PTX_ARCH.
 */
static const int warp_size      = 64;
static const int warp_size_log2 = 6;
/*! \brief Bitmask corresponding to all threads active in a warp.
 *  NOTE that here too we assume 32-wide warps.
 */
//static const unsigned int c_fullWarpMask = 0xffffffff;
static const unsigned long c_fullWarpMask = 0xffffffffffffffff;

/*! \brief Allow disabling HIP textures using the GMX_DISABLE_HIP_TEXTURES macro.
 *
 *  Only texture objects supported.
 *  Disable texture support missing in clang (all versions up to <=5.0-dev as of writing).
 *  Disable texture support on CC 7.0 and 8.0 for performance reasons (Issue #3845).
 *
 *  This option will not influence functionality. All features using textures ought
 *  to have fallback for texture-less reads (direct/LDG loads), all new code needs
 *  to provide fallback code.
 */
/*
#if defined(GMX_DISABLE_HIP_TEXTURES) || (defined(__clang__) && defined(__HIP__)) \
        || (GMX_PTX_ARCH == 700) || (GMX_PTX_ARCH == 800)
#    define DISABLE_HIP_TEXTURES 1
#else
#    define DISABLE_HIP_TEXTURES 0
#endif
*/
#if defined(GMX_DISABLE_HIP_TEXTURES) || (defined(__clang__) && defined(__HIPCC__)) 
#    define DISABLE_HIP_TEXTURES 1
#else
#    define DISABLE_HIP_TEXTURES 0
#endif

/*! \brief True if the use of texture fetch in the HIP kernels is disabled. */
static const bool c_disableHipTextures = DISABLE_HIP_TEXTURES;


/* HIP architecture technical characteristics. Needs macros because it is used
 * in the __launch_bounds__ function qualifiers and might need it in preprocessor
 * conditionals.
 *
 */
//#if GMX_PTX_ARCH > 0
//#    if GMX_PTX_ARCH <= 370 // CC 3.x
//#        define GMX_HIP_MAX_BLOCKS_PER_MP 16
//#        define GMX_HIP_MAX_THREADS_PER_MP 2048
//#    elif GMX_PTX_ARCH == 750 // CC 7.5, lower limits compared to 7.0
//#        define GMX_HIP_MAX_BLOCKS_PER_MP 16
//#        define GMX_HIP_MAX_THREADS_PER_MP 1024
//#    elif GMX_PTX_ARCH == 860 // CC 8.6, lower limits compared to 8.0
//#        define GMX_HIP_MAX_BLOCKS_PER_MP 16
//#        define GMX_HIP_MAX_THREADS_PER_MP 1536
//#    else // CC 5.x, 6.x, 7.0, 8.0
/* Note that this final branch covers all future architectures (current gen
 * is 8.x as of writing), hence assuming that these *currently defined* upper
 * limits will not be lowered.
 */
//#        define GMX_HIP_MAX_BLOCKS_PER_MP 32
//#        define GMX_HIP_MAX_THREADS_PER_MP 2048
//#    endif
//#else
#    define GMX_HIP_MAX_BLOCKS_PER_MP 16
#    define GMX_HIP_MAX_THREADS_PER_MP 1024
//#endif

// Macro defined for clang HIP device compilation in the presence of debug symbols
// used to work around codegen bug that breaks some kernels when assertions are on
// at -O1 and higher (tested with clang 6-8).
//#if defined(__clang__) && defined(__HIP__) && defined(__HIP_ARCH__) && !defined(NDEBUG)
//#    define CLANG_DISABLE_OPTIMIZATION_ATTRIBUTE __attribute__((optnone))
//#else
#    define CLANG_DISABLE_OPTIMIZATION_ATTRIBUTE
//#endif


#endif /* HIP_ARCH_UTILS_HPP_ */
