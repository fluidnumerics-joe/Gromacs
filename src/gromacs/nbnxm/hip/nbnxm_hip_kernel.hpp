#include "hip/hip_runtime.h"
/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2012,2013,2014,2015,2016 by the GROMACS development team.
 * Copyright (c) 2017,2018,2019,2020,2021, by the GROMACS development team, led by
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

/*! \internal \file
 *  \brief
 *  HIP non-bonded kernel used through preprocessor-based code generation
 *  of multiple kernel flavors, see nbnxn_hip_kernels.hpp.
 *
 *  NOTE: No include fence as it is meant to be included multiple times.
 *
 *  \author Szilárd Páll <pall.szilard@gmail.com>
 *  \author Berk Hess <hess@kth.se>
 *  \ingroup module_nbnxm
 */

#include "gromacs/gpu_utils/hip_arch_utils.hpp"
#include "gromacs/gpu_utils/hip_kernel_utils.hpp"
#include "gromacs/gpu_utils/typecasts.hpp"
#include "gromacs/math/units.h"
#include "gromacs/math/utilities.h"
#include "gromacs/pbcutil/ishift.h"
/* Note that floating-point constants in HIP code should be suffixed
 * with f (e.g. 0.5f), to stop the compiler producing intermediate
 * code that is in double precision.
 */

#if defined EL_EWALD_ANA || defined EL_EWALD_TAB
/* Note: convenience macro, needs to be undef-ed at the end of the file. */
#    define EL_EWALD_ANY
#endif

#if defined LJ_EWALD_COMB_GEOM || defined LJ_EWALD_COMB_LB
/* Note: convenience macro, needs to be undef-ed at the end of the file. */
#    define LJ_EWALD
#endif

#if defined EL_EWALD_ANY || defined EL_RF || defined LJ_EWALD \
        || (defined EL_CUTOFF && defined CALC_ENERGIES)
/* Macro to control the calculation of exclusion forces in the kernel
 * We do that with Ewald (elec/vdw) and RF. Cut-off only has exclusion
 * energy terms.
 *
 * Note: convenience macro, needs to be undef-ed at the end of the file.
 */
#    define EXCLUSION_FORCES
#endif

#if defined LJ_COMB_GEOM || defined LJ_COMB_LB
#    define LJ_COMB
#endif

/*
   Kernel launch parameters:
    - #blocks   = #pair lists, blockId = pair list Id
    - #threads  = NTHREAD_Z * c_clSize^2
    - shmem     = see nbnxn_hip.cu:calc_shmem_required_nonbonded()

    Each thread calculates an i force-component taking one pair of i-j atoms.
 */

/**@{*/
/*! \brief Compute capability dependent definition of kernel launch configuration parameters.
 *
 * NTHREAD_Z controls the number of j-clusters processed concurrently on NTHREAD_Z
 * warp-pairs per block.
 *
 * - On CC 3.0-3.5, and >=5.0 NTHREAD_Z == 1, translating to 64 th/block with 16
 * blocks/multiproc, is the fastest even though this setup gives low occupancy
 * (except on 6.0).
 * NTHREAD_Z > 1 results in excessive register spilling unless the minimum blocks
 * per multiprocessor is reduced proportionally to get the original number of max
 * threads in flight (and slightly lower performance).
 * - On CC 3.7 there are enough registers to double the number of threads; using
 * NTHREADS_Z == 2 is fastest with 16 blocks (TODO: test with RF and other kernels
 * with low-register use).
 *
 * Note that the current kernel implementation only supports NTHREAD_Z > 1 with
 * shuffle-based reduction, hence CC >= 3.0.
 *
 *
 * NOTEs on Volta / HIP 9 extensions:
 *
 * - While active thread masks are required for the warp collectives
 *   (we use any and shfl), the kernel is designed such that all conditions
 *   (other than the inner-most distance check) including loop trip counts
 *   are warp-synchronous. Therefore, we don't need ballot to compute the
 *   active masks as these are all full-warp masks.
 *
 */

/* Kernel launch bounds for different compute capabilities. The value of NTHREAD_Z
 * determines the number of threads per block and it is chosen such that
 * 16 blocks/multiprocessor can be kept in flight.
 * - CC 3.0,3.5, and >=5.0: NTHREAD_Z=1, (64, 16) bounds
 * - CC 3.7:                NTHREAD_Z=2, (128, 16) bounds
 *
 * Note: convenience macros, need to be undef-ed at the end of the file.
 */
#define NTHREAD_Z 1

// MI2** GPUs (gfx90a) have one unified pool of VGPRs and AccVGPRs. AccVGPRs are not used so
// we can use twice as many registers as on MI100 and earlier devices without spilling.
// Also it looks like spilling to global memory causes segfaults for some versions of the kernel.
#if defined(__gfx90a__)
#    define MIN_BLOCKS_PER_MP 1
#else
#    ifdef CALC_ENERGIES
#        define MIN_BLOCKS_PER_MP 6
#    else
#        define MIN_BLOCKS_PER_MP 8
#    endif
#endif
#define THREADS_PER_BLOCK (c_clSize * c_clSize * NTHREAD_Z)

__launch_bounds__(THREADS_PER_BLOCK, MIN_BLOCKS_PER_MP)
#ifdef PRUNE_NBL
#    ifdef CALC_ENERGIES
        __global__ void NB_KERNEL_FUNC_NAME(nbnxn_kernel, _VF_prune_hip)
#    else
        __global__ void NB_KERNEL_FUNC_NAME(nbnxn_kernel, _F_prune_hip)
#    endif /* CALC_ENERGIES */
#else
#    ifdef CALC_ENERGIES
        __global__ void NB_KERNEL_FUNC_NAME(nbnxn_kernel, _VF_hip)
#    else
        __global__ void NB_KERNEL_FUNC_NAME(nbnxn_kernel, _F_hip)
#    endif /* CALC_ENERGIES */
#endif     /* PRUNE_NBL */
                (NBAtomDataGpu atdat, NBParamGpu nbparam, Nbnxm::gpu_plist plist, bool bCalcFshift, nbnxn_cj4_t* __restrict__ pl_cj4)
#ifdef FUNCTION_DECLARATION_ONLY
                        ; /* Only do function declaration, omit the function body. */
#else
{
    /* convenience variables */
    const nbnxn_sci_t* pl_sci = plist.sci_sorted == nullptr ? plist.sci : plist.sci_sorted;
    FastBuffer<nbnxn_excl_t> excl    = FastBuffer<nbnxn_excl_t>(plist.excl);
#    ifndef LJ_COMB
    FastBuffer<int>      atom_types  = FastBuffer<int>(atdat.atomTypes);
    int                  ntypes      = atdat.numTypes;
#    else
    FastBuffer<float2> lj_comb       = FastBuffer<float2>(atdat.ljComb);
    float2                   ljcp_i, ljcp_j;
#    endif
    FastBuffer<float4>   xq          = FastBuffer<float4>(atdat.xq);
    float3*              f           = asFloat3(atdat.f);
    const float3*        shift_vec   = asFloat3(atdat.shiftVec);
    float                rcoulomb_sq = nbparam.rcoulomb_sq;
#    ifdef VDW_CUTOFF_CHECK
    float                rvdw_sq     = nbparam.rvdw_sq;
    float                vdw_in_range;
#    endif
#    ifdef LJ_EWALD
    float                lje_coeff2, lje_coeff6_6;
#    endif
#    ifdef EL_RF
    float                two_k_rf    = nbparam.two_k_rf;
#    endif
#    ifdef EL_EWALD_ANA
    float                beta2       = nbparam.ewald_beta * nbparam.ewald_beta;
    float                beta3       = nbparam.ewald_beta * nbparam.ewald_beta * nbparam.ewald_beta;
#    endif
#    ifdef PRUNE_NBL
    float                rlist_sq    = nbparam.rlistOuter_sq;
#    endif

    unsigned int bidx = blockIdx.x;

#    ifdef CALC_ENERGIES
#        ifdef EL_EWALD_ANY
    float                beta        = nbparam.ewald_beta;
    float                ewald_shift = nbparam.sh_ewald;
#        else
    float reactionFieldShift = nbparam.c_rf;
#        endif /* EL_EWALD_ANY */

#        ifdef GMX_ENABLE_MEMORY_MULTIPLIER
    const unsigned int energy_index_base = 1 + (bidx & (c_clEnergyMemoryMultiplier - 1));
#        else
    const unsigned int energy_index_base = 0;
#        endif     /* GMX_ENABLE_MEMORY_MULTIPLIER */
    float*               e_lj        = atdat.eLJ + energy_index_base;
    float*               e_el        = atdat.eElec + energy_index_base;
#    endif     /* CALC_ENERGIES */

    /* thread/block/warp id-s */
    unsigned int tidxi = threadIdx.x;
    unsigned int tidxj = threadIdx.y;
    unsigned int tidx  = threadIdx.y * c_clSize + threadIdx.x;
#    if NTHREAD_Z == 1
    unsigned int tidxz = 0;
#    else
    unsigned int  tidxz = threadIdx.z;
#    endif

    unsigned int widx  = (c_clSize * c_clSize) == warpSize ? 0 : tidx / c_subWarp; /* warp index */

    int          sci, ci, cj, ai, aj, cij4_start, cij4_end;
#    ifndef LJ_COMB
    int          typei, typej;
#    endif
    int          i, jm, j4, wexcl_idx;
    float        qi, qj_f, r2, inv_r, inv_r2;
#    if !defined LJ_COMB_LB || defined CALC_ENERGIES
    float        inv_r6;
    float2       c6c12;
#    endif
#    ifdef LJ_COMB_LB
    float        sigma, epsilon;
#    endif
    float        int_bit, F_invr;
#    ifdef CALC_ENERGIES
    float        E_lj, E_el;
#    endif
#    if defined CALC_ENERGIES || defined LJ_POT_SWITCH
    float        E_lj_p;
#    endif
    unsigned int wexcl, imask, mask_ji;
    float4       xqbuf;
    fast_float3  xi, xj, rv, n2, f_ij, fcj_buf;
    fast_float3  fci_buf[c_nbnxnGpuNumClusterPerSupercluster]; /* i force buffer */
    nbnxn_sci_t  nb_sci;

    /*! i-cluster interaction mask for a super-cluster with all c_nbnxnGpuNumClusterPerSupercluster=8 bits set */
    const unsigned superClInteractionMask = ((1U << c_nbnxnGpuNumClusterPerSupercluster) - 1U);

    /*********************************************************************
     * Set up shared memory pointers.
     * sm_nextSlotPtr should always be updated to point to the "next slot",
     * that is past the last point where data has been stored.
     */
    HIP_DYNAMIC_SHARED( char, sm_dynamicShmem)
    char*                  sm_nextSlotPtr = sm_dynamicShmem;
    static_assert(sizeof(char) == 1,
                  "The shared memory offset calculation assumes that char is 1 byte");

    /* shmem buffer for i x+q pre-loading */
    float4* xqib = reinterpret_cast<float4*>(sm_nextSlotPtr);
    sm_nextSlotPtr += (c_nbnxnGpuNumClusterPerSupercluster * c_clSize * sizeof(*xqib));

#    ifndef LJ_COMB
    /* shmem buffer for i atom-type pre-loading */
    int* atib = reinterpret_cast<int*>(sm_nextSlotPtr);
    sm_nextSlotPtr += (c_nbnxnGpuNumClusterPerSupercluster * c_clSize * sizeof(*atib));
#    else
    /* shmem buffer for i-atom LJ combination rule parameters */
    float2* ljcpib = reinterpret_cast<float2*>(sm_nextSlotPtr);
    sm_nextSlotPtr += (c_nbnxnGpuNumClusterPerSupercluster * c_clSize * sizeof(*ljcpib));
#    endif
    /*********************************************************************/

    nb_sci     = pl_sci[bidx];         /* my i super-cluster's index = current bidx */
    sci        = nb_sci.sci;           /* super-cluster */
    cij4_start = nb_sci.cj4_ind_start; /* first ...*/
    cij4_end   = nb_sci.cj4_ind_start + nb_sci.cj4_length;   /* and last index of j clusters */

    // We may need only a subset of threads active for preloading i-atoms
    // depending on the super-cluster and cluster / thread-block size.
    constexpr bool c_loadUsingAllXYThreads = (c_clSize == c_nbnxnGpuNumClusterPerSupercluster);
    if (tidxz == 0 && (c_loadUsingAllXYThreads || tidxj < c_nbnxnGpuNumClusterPerSupercluster))
    {
        /* Pre-load i-atom x and q into shared memory */
        ci = sci * c_nbnxnGpuNumClusterPerSupercluster + tidxj;
        ai = ci * c_clSize + tidxi;
        const float3 shift = shift_vec[nb_sci.shift];
        xqbuf = xq[ai];
        // TODO: Remove `-` and reverse operators in `xi + xj` and `+- f_ij` when it's fixed.
        // For some reason the compiler does not generate v_pk_add_f32 and v_sub_f32 for `xi - xj`
        // but generates 3 v_sub_f32. Hence all this mess with signs.
        xqbuf.x = -(xqbuf.x + shift.x);
        xqbuf.y = -(xqbuf.y + shift.y);
        xqbuf.z = -(xqbuf.z + shift.z);
        xqbuf.w *= nbparam.epsfac;
        xqib[tidxj * c_clSize + tidxi] = xqbuf;

#    ifndef LJ_COMB
        /* Pre-load the i-atom types into shared memory */
        atib[tidxj * c_clSize + tidxi] = atom_types[ai];
#    else
        /* Pre-load the LJ combination parameters into shared memory */
        ljcpib[tidxj * c_clSize + tidxi] = lj_comb[ai];
#    endif
    }
    __syncthreads();

    for (i = 0; i < c_nbnxnGpuNumClusterPerSupercluster; i++)
    {
        fci_buf[i] = make_fast_float3(0.0F);
    }

#    ifdef LJ_EWALD
    /* TODO: we are trading registers with flops by keeping lje_coeff-s, try re-calculating it later */
    lje_coeff2   = nbparam.ewaldcoeff_lj * nbparam.ewaldcoeff_lj;
    lje_coeff6_6 = lje_coeff2 * lje_coeff2 * lje_coeff2 * c_oneSixth;
#    endif


#    ifdef CALC_ENERGIES
    E_lj         = 0.0F;
    E_el         = 0.0F;

#        ifdef EXCLUSION_FORCES /* Ewald or RF */
    if (nb_sci.shift == gmx::c_centralShiftIndex
        && pl_cj4[cij4_start].cj[0] == sci * c_nbnxnGpuNumClusterPerSupercluster)
    {
        /* we have the diagonal: add the charge and LJ self interaction energy term */
        for (i = 0; i < c_nbnxnGpuNumClusterPerSupercluster; i++)
        {
#           if defined EL_EWALD_ANY || defined EL_RF || defined EL_CUTOFF
            qi = xqib[i * c_clSize + tidxi].w;
            E_el += qi * qi;
#            endif

#           ifdef LJ_EWALD
            // load only the first 4 bytes of the parameter pair (equivalent with nbfp[idx].x)
            #if DISABLE_HIP_TEXTURES
            E_lj += LDG(reinterpret_cast<float*>(
                    &nbparam.nbfp[atom_types[(sci * c_nbnxnGpuNumClusterPerSupercluster + i) * c_clSize + tidxi]
                                  * (ntypes + 1)]));
            #else
            E_lj += tex1Dfetch<float>(
                    nbparam.nbfp_texobj, atom_types[(sci * c_nbnxnGpuNumClusterPerSupercluster + i) * c_clSize + tidxi]
                                  * (ntypes + 1));
            #endif
#            endif
        }

        /* divide the self term(s) equally over the j-threads, then multiply with the coefficients. */
#            ifdef LJ_EWALD
        E_lj /= c_clSize * NTHREAD_Z;
        E_lj *= 0.5F * c_oneSixth * lje_coeff6_6;
#            endif

#            if defined EL_EWALD_ANY || defined EL_RF || defined EL_CUTOFF
        /* Correct for epsfac^2 due to adding qi^2 */
        E_el /= nbparam.epsfac * c_clSize * NTHREAD_Z;
#                if defined EL_RF || defined EL_CUTOFF
        E_el *= -0.5F * reactionFieldShift;
#                else
        E_el *= -beta * M_FLOAT_1_SQRTPI; /* last factor 1/sqrt(pi) */
#                endif
#            endif /* EL_EWALD_ANY || defined EL_RF || defined EL_CUTOFF */
    }
#        endif     /* EXCLUSION_FORCES */

#    endif /* CALC_ENERGIES */

#    ifdef EXCLUSION_FORCES
    const int nonSelfInteraction = !(nb_sci.shift == gmx::c_centralShiftIndex & tidxj <= tidxi);
#    endif

    /* loop over the j clusters = seen by any of the atoms in the current super-cluster;
     * The loop stride NTHREAD_Z ensures that consecutive warps-pairs are assigned
     * consecutive j4's entries.
     */
    for (j4 = cij4_start; j4 < cij4_end; ++j4)
    {
        imask     = pl_cj4[j4].imei[widx].imask;
        /* When c_nbnxnGpuClusterpairSplit = 1, i.e. on CDNA, ROCm 5.2's compiler correctly
         * generates scalar loads for __restrict__ pl_cj4 (but not for plist.cj4),
         * ROCm 5.0.2's compiler generates vector loads, imask is a vector register.
         * If this happens, "scalarize" imask so it goes to a scalar register and
         * all imask-related checks become simpler scalar instructions.
         * (__builtin_amdgcn_readfirstlane is no-op if it's already a scalar register).
         */
        imask     = (c_clSize * c_clSize) == warpSize ? __builtin_amdgcn_readfirstlane(imask) : imask;
#    ifndef PRUNE_NBL
        if (!imask)
        {
            continue;
        }
#    endif
        wexcl_idx = pl_cj4[j4].imei[widx].excl_ind;
        wexcl     = excl[wexcl_idx].pair[tidx & (c_subWarp - 1)];

#       pragma unroll
        for (jm = 0; jm < c_nbnxnGpuJgroupSize; jm++)
        {
            const bool maskSet = imask & (superClInteractionMask << (jm * c_nbnxnGpuNumClusterPerSupercluster));
            if (!maskSet)
            {
               continue;
            }

            mask_ji = (1U << (jm * c_nbnxnGpuNumClusterPerSupercluster));

            cj = pl_cj4[j4].cj[jm];
            aj = cj * c_clSize + tidxj;

            /* load j atom data */
            xqbuf = xq[aj];
            xj    = make_fast_float3(xqbuf);
            qj_f  = xqbuf.w;
#    ifndef LJ_COMB
            typej = atom_types[aj];
#    else
            ljcp_j = lj_comb[aj];
#    endif

            fcj_buf = make_fast_float3(0.0F);
#           pragma unroll c_nbnxnGpuNumClusterPerSupercluster
            for (i = 0; i < c_nbnxnGpuNumClusterPerSupercluster; i++)
            {
                if (imask & mask_ji)
                {
                    ci = sci * c_nbnxnGpuNumClusterPerSupercluster + i; /* i cluster index */

                    /* all threads load an atom from i cluster ci into shmem! */
                    xqbuf = xqib[i * c_clSize + tidxi];
                    xi    = make_fast_float3(xqbuf);

                    /* distance between i and j atoms */
                    rv = xi + xj;
                    r2 = norm2(rv);

#    ifdef PRUNE_NBL
                    /* If _none_ of the atoms pairs are in cutoff range,
                       the bit corresponding to the current
                       cluster-pair in imask gets set to 0. */
                    if (!__nb_any(r2 < rlist_sq, widx))
                    {
                        imask &= ~mask_ji;
                    }
#    endif
                    int_bit = (wexcl >> (jm * c_nbnxnGpuNumClusterPerSupercluster + i)) & 1;
                    /* cutoff & exclusion check */
#    ifdef EXCLUSION_FORCES
                    if ((r2 < rcoulomb_sq) && (ci != (nonSelfInteraction ? -1 : cj)))
#    else
                    if ((r2 < rcoulomb_sq) * int_bit)
#    endif
                    {
                        /* load the rest of the i-atom parameters */
                        qi = xqbuf.w;
#    ifndef LJ_COMB
                        /* LJ 6*C6 and 12*C12 */
                        typei = atib[i * c_clSize + tidxi];
#        ifdef __gfx1030__
                        c6c12 = fetch_nbfp_c6_c12(nbparam, ntypes * typei + typej);
#        else
                        c6c12 = fetch_nbfp_c6_c12(nbparam, __mul24(ntypes, typei) + typej);
#        endif
#    else
                        ljcp_i       = ljcpib[i * c_clSize + tidxi];
#        ifdef LJ_COMB_GEOM
                        c6c12        = ljcp_i * ljcp_j;
#        else
                        /* LJ 2^(1/6)*sigma and 12*epsilon */
                        sigma   = ljcp_i.x + ljcp_j.x;
                        epsilon = ljcp_i.y * ljcp_j.y;
#            if defined CALC_ENERGIES || defined LJ_FORCE_SWITCH || defined LJ_POT_SWITCH
                        c6c12 = convert_sigma_epsilon_to_c6_c12(sigma, epsilon);
#            endif
#        endif /* LJ_COMB_GEOM */
#    endif     /* LJ_COMB */

                        // Ensure distance do not become so small that r^-12 overflows
                        r2 = fmax(r2, c_nbnxnMinDistanceSquared);

                        inv_r  = __frsqrt_rn(r2);
                        inv_r2 = inv_r * inv_r;
#    if !defined LJ_COMB_LB || defined CALC_ENERGIES
                        inv_r6 = inv_r2 * inv_r2 * inv_r2;
#        ifdef EXCLUSION_FORCES
                        /* We could mask inv_r2, but with Ewald
                         * masking both inv_r6 and F_invr is faster */
                        inv_r6 *= int_bit;
#        endif /* EXCLUSION_FORCES */

                        F_invr = inv_r6 * (c6c12.y * inv_r6 - c6c12.x) * inv_r2;
#        if defined CALC_ENERGIES || defined LJ_POT_SWITCH
                        E_lj_p = int_bit
                                 * (c6c12.y * (inv_r6 * inv_r6 + nbparam.repulsion_shift.cpot) * c_oneTwelveth
                                    - c6c12.x * (inv_r6 + nbparam.dispersion_shift.cpot) * c_oneSixth);
#        endif
#    else /* !LJ_COMB_LB || CALC_ENERGIES */
                        float sig_r  = sigma * inv_r;
                        float sig_r2 = sig_r * sig_r;
                        float sig_r6 = sig_r2 * sig_r2 * sig_r2;
#        ifdef EXCLUSION_FORCES
                        sig_r6 *= int_bit;
#        endif /* EXCLUSION_FORCES */

                        F_invr = epsilon * sig_r6 * (sig_r6 - 1.0F) * inv_r2;
#    endif     /* !LJ_COMB_LB || CALC_ENERGIES */

#    ifdef LJ_FORCE_SWITCH
#        ifdef CALC_ENERGIES
                        calculate_force_switch_F_E(nbparam, c6c12, inv_r, r2, &F_invr, &E_lj_p);
#        else
                        calculate_force_switch_F(nbparam, c6c12, inv_r, r2, &F_invr);
#        endif /* CALC_ENERGIES */
#    endif     /* LJ_FORCE_SWITCH */


#    ifdef LJ_EWALD
#        ifdef LJ_EWALD_COMB_GEOM
#            ifdef CALC_ENERGIES
                        calculate_lj_ewald_comb_geom_F_E(
                                nbparam, typei, typej, r2, inv_r2, lje_coeff2, lje_coeff6_6, int_bit, &F_invr, &E_lj_p);
#            else
                        calculate_lj_ewald_comb_geom_F(
                                nbparam, typei, typej, r2, inv_r2, lje_coeff2, lje_coeff6_6, &F_invr);
#            endif /* CALC_ENERGIES */
#        elif defined LJ_EWALD_COMB_LB
                        calculate_lj_ewald_comb_LB_F_E(nbparam,
                                                       typei,
                                                       typej,
                                                       r2,
                                                       inv_r2,
                                                       lje_coeff2,
                                                       lje_coeff6_6,
#            ifdef CALC_ENERGIES
                                                       int_bit,
                                                       &F_invr,
                                                       &E_lj_p
#            else
                                                       0,
                                                       &F_invr,
                                                       nullptr
#            endif /* CALC_ENERGIES */
                        );
#        endif     /* LJ_EWALD_COMB_GEOM */
#    endif         /* LJ_EWALD */

#    ifdef LJ_POT_SWITCH
#        ifdef CALC_ENERGIES
                        calculate_potential_switch_F_E(nbparam, inv_r, r2, &F_invr, &E_lj_p);
#        else
                        calculate_potential_switch_F(nbparam, inv_r, r2, &F_invr, &E_lj_p);
#        endif /* CALC_ENERGIES */
#    endif     /* LJ_POT_SWITCH */

#    ifdef VDW_CUTOFF_CHECK
                        /* Separate VDW cut-off check to enable twin-range cut-offs
                         * (rvdw < rcoulomb <= rlist)
                         */
                        vdw_in_range = (r2 < rvdw_sq) ? 1.0F : 0.0F;
                        F_invr *= vdw_in_range;
#        ifdef CALC_ENERGIES
                        E_lj_p *= vdw_in_range;
#        endif
#    endif /* VDW_CUTOFF_CHECK */

#    ifdef CALC_ENERGIES
                        E_lj += E_lj_p;
#    endif


#    ifdef EL_CUTOFF
#        ifdef EXCLUSION_FORCES
                        F_invr += qi * qj_f * int_bit * inv_r2 * inv_r;
#        else
                        F_invr += qi * qj_f * inv_r2 * inv_r;
#        endif
#    endif
#    ifdef EL_RF
                        F_invr += qi * qj_f * (int_bit * inv_r2 * inv_r - two_k_rf);
#    endif
#    if defined   EL_EWALD_ANA
                        F_invr += qi * qj_f
                                  * (int_bit * inv_r2 * inv_r + pmecorrF(beta2 * r2) * beta3);
#    elif defined EL_EWALD_TAB
                        F_invr += qi * qj_f
                                  * (int_bit * inv_r2
                                     - interpolate_coulomb_force_r(nbparam, r2 * inv_r))
                                  * inv_r;
#    endif /* EL_EWALD_ANA/TAB */

#    ifdef CALC_ENERGIES
#        ifdef EL_CUTOFF
                        E_el += qi * qj_f * (int_bit * inv_r - reactionFieldShift);
#        endif
#        ifdef EL_RF
                        E_el += qi * qj_f
                                * (int_bit * inv_r + 0.5F * two_k_rf * r2 - reactionFieldShift);
#        endif
#        ifdef EL_EWALD_ANY
                        /* 1.0F - erff is faster than erfcf */
                        E_el += qi * qj_f
                                * (inv_r * (int_bit - erff(r2 * inv_r * beta)) - int_bit * ewald_shift);
#        endif /* EL_EWALD_ANY */
#    endif
                        f_ij = rv * F_invr;

                        /* accumulate j forces in registers */
                        fcj_buf = fcj_buf + f_ij;

                        /* accumulate i forces in registers */
                        fci_buf[i] = fci_buf[i] - f_ij;
                    }
                }

                /* shift the mask bit by 1 */
                mask_ji += mask_ji;
            }

            /* reduce j forces */
            float r = reduce_force_j_warp_shfl(fcj_buf, tidxi);
            if (tidxi < 3)
            {
                atomic_add_force(f, aj, tidxi, r);
            }
        }
#    ifdef PRUNE_NBL
        /* Update the imask with the new one which does not contain the
           out of range clusters anymore. */
        pl_cj4[j4].imei[widx].imask = imask;
#    endif
    }

    /* skip central shifts when summing shift forces */
    if (nb_sci.shift == gmx::c_centralShiftIndex)
    {
        bCalcFshift = false;
    }

#ifndef __gfx1030__
    float fshift_buf = 0.0F;
    float fci[c_nbnxnGpuNumClusterPerSupercluster];

    /* reduce i forces */
    for (i = 0; i < c_nbnxnGpuNumClusterPerSupercluster; i++)
    {
        fci[i] = reduce_force_i_warp_shfl(fci_buf[i], tidxi, tidxj);
        fshift_buf += fci[i];
    }
    if (tidxi < 3)
    {
        for (i = 0; i < c_nbnxnGpuNumClusterPerSupercluster; i++)
        {
            ai = (sci * c_nbnxnGpuNumClusterPerSupercluster + i) * c_clSize + tidxj;
            atomic_add_force(f, ai, tidxi, fci[i]);
        }
    }

    /* add up local shift forces into global mem, tidxi indexes x,y,z */
    if (bCalcFshift)
    {
#ifdef GMX_ENABLE_MEMORY_MULTIPLIER
        const unsigned int shift_index_base = gmx::c_numShiftVectors * (1 + (bidx & (c_clShiftMemoryMultiplier - 1)));
#else
        const unsigned int shift_index_base = 0;
#endif
        if (tidxi < 3)
        {
            float3* fShift = asFloat3(atdat.fShift);
            atomic_add_force(fShift, nb_sci.shift + shift_index_base, tidxi, fshift_buf);
        }
    }
#else
    float3 fshift_buf = make_float3(0.0f);

    /* reduce i forces */
    for (i = 0; i < c_nbnxnGpuNumClusterPerSupercluster; i++)
    {
        ai = (sci * c_nbnxnGpuNumClusterPerSupercluster + i) * c_clSize + tidxi;
        reduce_force_i_warp_shfl(fci_buf[i], f, fshift_buf, bCalcFshift, tidxj, ai);
    }

    /* add up local shift forces into global mem, tidxj indexes x,y,z */
    if (bCalcFshift)
    {
        fshift_buf.x += warp_move_dpp<float, 0xb1>(fshift_buf.x);
        fshift_buf.y += warp_move_dpp<float, 0xb1>(fshift_buf.y);
        fshift_buf.z += warp_move_dpp<float, 0xb1>(fshift_buf.z);

        fshift_buf.x += warp_move_dpp<float, 0x4e>(fshift_buf.x);
        fshift_buf.y += warp_move_dpp<float, 0x4e>(fshift_buf.y);
        fshift_buf.z += warp_move_dpp<float, 0x4e>(fshift_buf.z);

        fshift_buf.x += warp_move_dpp<float, 0x114>(fshift_buf.x);
        fshift_buf.y += warp_move_dpp<float, 0x114>(fshift_buf.y);
        fshift_buf.z += warp_move_dpp<float, 0x114>(fshift_buf.z);

        if ( tidx == (c_clSize - 1) || tidx == (c_subWarp + c_clSize - 1) )
        {
   #ifdef GMX_ENABLE_MEMORY_MULTIPLIER
            const unsigned int shift_index_base = gmx::c_numShiftVectors * (1 + (bidx & (c_clShiftMemoryMultiplier - 1)));
   #else
            const unsigned int shift_index_base = 0;
   #endif
            float3* fShift = asFloat3(atdat.fShift);
            atomicAdd(&(fShift[nb_sci.shift + shift_index_base].x), fshift_buf.x);
            atomicAdd(&(fShift[nb_sci.shift + shift_index_base].y), fshift_buf.y);
            atomicAdd(&(fShift[nb_sci.shift + shift_index_base].z), fshift_buf.z);
        }
    }

#endif

#    ifdef CALC_ENERGIES
    /* reduce the energies over warps and store into global memory */
    reduce_energy_warp_shfl(E_lj, E_el, e_lj, e_el, tidx);
#    endif
}
#endif /* FUNCTION_DECLARATION_ONLY */

#undef NTHREAD_Z
#undef MIN_BLOCKS_PER_MP
#undef THREADS_PER_BLOCK

#undef EL_EWALD_ANY
#undef EXCLUSION_FORCES
#undef LJ_EWALD

#undef LJ_COMB
