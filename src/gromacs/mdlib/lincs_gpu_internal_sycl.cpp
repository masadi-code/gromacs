/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2019,2020,2021, by the GROMACS development team, led by
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
 *
 * \brief Implements LINCS kernels using SYCL
 *
 * This file contains SYCL kernels of LINCS constraints algorithm.
 *
 * \author Artem Zhmurov <zhmurov@gmail.com>
 *
 * \ingroup module_mdlib
 */
#include "lincs_gpu_internal.h"

#include "gromacs/gpu_utils/devicebuffer.h"
#include "gromacs/gpu_utils/gmxsycl.h"
#include "gromacs/gpu_utils/sycl_kernel_utils.h"
#include "gromacs/mdlib/lincs_gpu.h"
#include "gromacs/pbcutil/pbc_aiuc_sycl.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/template_mp.h"

namespace gmx
{

using cl::sycl::access::fence_space;
using cl::sycl::access::mode;
using cl::sycl::access::target;

/*! \brief Main kernel for LINCS constraints.
 *
 * See Hess et al., J. Comput. Chem. 18: 1463-1472 (1997) for the description of the algorithm.
 *
 * In GPU version, one thread is responsible for all computations for one constraint. The blocks are
 * filled in a way that no constraint is coupled to the constraint from the next block. This is achieved
 * by moving active threads to the next block, if the correspondent group of coupled constraints is to big
 * to fit the current thread block. This may leave some 'dummy' threads in the end of the thread block, i.e.
 * threads that are not required to do actual work. Since constraints from different blocks are not coupled,
 * there is no need to synchronize across the device. However, extensive communication in a thread block
 * are still needed.
 *
 * \todo Reduce synchronization overhead. Some ideas are:
 *        1. Consider going to warp-level synchronization for the coupled constraints.
 *        2. Move more data to local/shared memory and try to get rid of atomic operations (at least on
 *           the device level).
 *        3. Use analytical solution for matrix A inversion.
 *        4. Introduce mapping of thread id to both single constraint and single atom, thus designating
 *           Nth threads to deal with Nat <= Nth coupled atoms and Nc <= Nth coupled constraints.
 *       See Issue #2885 for details (https://gitlab.com/gromacs/gromacs/-/issues/2885)
 * \todo The use of __restrict__  for gm_xp and gm_v causes failure, probably because of the atomic
 *       operations. Investigate this issue further.
 *
 * \tparam updateVelocities        Whether velocities should be updated this step.
 * \tparam computeVirial           Whether virial tensor should be computed this step.
 * \tparam haveCoupledConstraints  If there are coupled constraints (i.e. LINCS iterations are needed).
 *
 * \param[in]     cgh                           SYCL handler.
 * \param[in]     numConstraintsThreads         Total number of threads.
 * \param[in]     a_constraints                 List of constrained atoms.
 * \param[in]     a_constraintsTargetLengths    Equilibrium distances for the constraints.
 * \param[in]     a_coupledConstraintsCounts    Number of constraints, coupled with the current one.
 * \param[in]     a_coupledConstraintsIndices   List of coupled with the current one.
 * \param[in]     a_massFactors                 Mass factors.
 * \param[in]     a_matrixA                     Elements of the coupling matrix.
 * \param[in]     a_inverseMasses               1/mass for all atoms.
 * \param[in]     numIterations                 Number of iterations used to correct the projection.
 * \param[in]     expansionOrder                Order of expansion when inverting the matrix.
 * \param[in]     a_x                           Unconstrained positions.
 * \param[in,out] a_xp                          Positions at the previous step, will be updated.
 * \param[in]     invdt                         Inverse timestep (needed to update velocities).
 * \param[in,out] a_v                           Velocities of atoms, will be updated if \c updateVelocities.
 * \param[in,out] a_virialScaled                Scaled virial tensor (6 floats: [XX, XY, XZ, YY, YZ, ZZ].
 *                                              Will be updated if \c updateVirial.
 * \param[in]     pbcAiuc                       Periodic boundary data.
 */
template<bool updateVelocities, bool computeVirial, bool haveCoupledConstraints>
auto lincsKernel(cl::sycl::handler&                   cgh,
                 const int                            numConstraintsThreads,
                 DeviceAccessor<AtomPair, mode::read> a_constraints,
                 DeviceAccessor<float, mode::read>    a_constraintsTargetLengths,
                 OptionalAccessor<int, mode::read, haveCoupledConstraints> a_coupledConstraintsCounts,
                 OptionalAccessor<int, mode::read, haveCoupledConstraints> a_coupledConstraintsIndices,
                 OptionalAccessor<float, mode::read, haveCoupledConstraints>       a_massFactors,
                 OptionalAccessor<float, mode::read_write, haveCoupledConstraints> a_matrixA,
                 DeviceAccessor<float, mode::read>                                 a_inverseMasses,
                 const int                                                         numIterations,
                 const int                                                         expansionOrder,
                 DeviceAccessor<Float3, mode::read>                                a_x,
                 DeviceAccessor<float, mode::read_write>                           a_xp,
                 const float                                                       invdt,
                 OptionalAccessor<float, mode::read_write, updateVelocities>       a_v,
                 OptionalAccessor<float, mode::read_write, computeVirial>          a_virialScaled,
                 PbcAiuc                                                           pbcAiuc)
{
    cgh.require(a_constraints);
    cgh.require(a_constraintsTargetLengths);
    if constexpr (haveCoupledConstraints)
    {
        cgh.require(a_coupledConstraintsCounts);
        cgh.require(a_coupledConstraintsIndices);
        cgh.require(a_massFactors);
        cgh.require(a_matrixA);
    }
    cgh.require(a_inverseMasses);
    cgh.require(a_x);
    cgh.require(a_xp);
    if constexpr (updateVelocities)
    {
        cgh.require(a_v);
    }
    if constexpr (computeVirial)
    {
        cgh.require(a_virialScaled);
    }

    // shmem buffer for local distances
    auto sm_r = [&]() {
        return cl::sycl::accessor<Float3, 1, mode::read_write, target::local>(
                cl::sycl::range<1>(c_threadsPerBlock), cgh);
    }();

    // shmem buffer for right-hand-side values
    auto sm_rhs = [&]() {
        return cl::sycl::accessor<float, 1, mode::read_write, target::local>(
                cl::sycl::range<1>(c_threadsPerBlock), cgh);
    }();

    // shmem buffer for virial components
    auto sm_threadVirial = [&]() {
        if constexpr (computeVirial)
        {
            return cl::sycl::accessor<float, 1, mode::read_write, target::local>(
                    cl::sycl::range<1>(c_threadsPerBlock * 6), cgh);
        }
        else
        {
            return nullptr;
        }
    }();

    return [=](cl::sycl::nd_item<1> itemIdx) {
        const int threadIndex   = itemIdx.get_global_linear_id();
        const int threadInBlock = itemIdx.get_local_linear_id(); // Work-item index in work-group

        AtomPair pair = a_constraints[threadIndex];
        int      i    = pair.i;
        int      j    = pair.j;

        // Mass-scaled Lagrange multiplier
        float lagrangeScaled = 0.0F;

        float targetLength;
        float inverseMassi;
        float inverseMassj;
        float sqrtReducedMass;

        Float3 xi;
        Float3 xj;
        Float3 rc;

        // i == -1 indicates dummy constraint at the end of the thread block.
        bool isDummyThread = (i == -1);

        // Everything computed for these dummies will be equal to zero
        if (isDummyThread)
        {
            targetLength    = 0.0F;
            inverseMassi    = 0.0F;
            inverseMassj    = 0.0F;
            sqrtReducedMass = 0.0F;

            xi = Float3(0.0F, 0.0F, 0.0F);
            xj = Float3(0.0F, 0.0F, 0.0F);
            rc = Float3(0.0F, 0.0F, 0.0F);
        }
        else
        {
            // Collecting data
            targetLength    = a_constraintsTargetLengths[threadIndex];
            inverseMassi    = a_inverseMasses[i];
            inverseMassj    = a_inverseMasses[j];
            sqrtReducedMass = cl::sycl::rsqrt(inverseMassi + inverseMassj);

            xi = a_x[i];
            xj = a_x[j];

            Float3 dx;
            pbcDxAiucSycl(pbcAiuc, xi, xj, dx);

            float rlen = cl::sycl::rsqrt(dx[XX] * dx[XX] + dx[YY] * dx[YY] + dx[ZZ] * dx[ZZ]);
            rc         = rlen * dx;
        }

        sm_r[threadIndex] = rc;
        // Make sure that all r's are saved into shared memory
        // before they are accessed in the loop below
        itemIdx.barrier(fence_space::global_and_local);

        /*
         * Constructing LINCS matrix (A)
         */
        int coupledConstraintsCount = 0;
        if constexpr (haveCoupledConstraints)
        {
            // Only non-zero values are saved (for coupled constraints)
            coupledConstraintsCount = a_coupledConstraintsCounts[threadIndex];
            for (int n = 0; n < coupledConstraintsCount; n++)
            {
                int index = n * numConstraintsThreads + threadIndex;
                int c1    = a_coupledConstraintsIndices[index];

                Float3 rc1       = sm_r[c1];
                a_matrixA[index] = a_massFactors[index]
                                   * (rc[XX] * rc1[XX] + rc[YY] * rc1[YY] + rc[ZZ] * rc1[ZZ]);
            }
        }

        // Skipping in dummy threads
        if (!isDummyThread)
        {
            xi[XX] = atomicLoad(a_xp[i * DIM + XX]);
            xi[YY] = atomicLoad(a_xp[i * DIM + YY]);
            xi[ZZ] = atomicLoad(a_xp[i * DIM + ZZ]);
            xj[XX] = atomicLoad(a_xp[j * DIM + XX]);
            xj[YY] = atomicLoad(a_xp[j * DIM + YY]);
            xj[ZZ] = atomicLoad(a_xp[j * DIM + ZZ]);
        }

        Float3 dx;
        pbcDxAiucSycl(pbcAiuc, xi, xj, dx);

        float sol = sqrtReducedMass * ((rc[XX] * dx[XX] + rc[YY] * dx[YY] + rc[ZZ] * dx[ZZ]) - targetLength);

        /*
         *  Inverse matrix using a set of expansionOrder matrix multiplications
         */

        // This will use the same memory space as sm_r, which is no longer needed.
        sm_rhs[threadInBlock] = sol;

        // No need to iterate if there are no coupled constraints.
        if constexpr (haveCoupledConstraints)
        {
            for (int rec = 0; rec < expansionOrder; rec++)
            {
                // Making sure that all sm_rhs are saved before they are accessed in a loop below
                itemIdx.barrier(fence_space::global_and_local);
                float mvb = 0.0F;
                for (int n = 0; n < coupledConstraintsCount; n++)
                {
                    int index = n * numConstraintsThreads + threadIndex;
                    int c1    = a_coupledConstraintsIndices[index];
                    // Convolute current right-hand-side with A
                    // Different, non overlapping parts of sm_rhs[..] are read during odd and even iterations
                    mvb = mvb + a_matrixA[index] * sm_rhs[c1 + c_threadsPerBlock * (rec % 2)];
                }
                // 'Switch' rhs vectors, save current result
                // These values will be accessed in the loop above during the next iteration.
                sm_rhs[threadInBlock + c_threadsPerBlock * ((rec + 1) % 2)] = mvb;
                sol                                                         = sol + mvb;
            }
        }

        // Current mass-scaled Lagrange multipliers
        lagrangeScaled = sqrtReducedMass * sol;

        // Save updated coordinates before correction for the rotational lengthening
        Float3 tmp = rc * lagrangeScaled;

        // Writing for all but dummy constraints
        if (!isDummyThread)
        {
            /*
             * Note: Using memory_scope::work_group for atomic_ref can be better here,
             * but for now we re-use the existing function for memory_scope::device atomics.
             */
            atomicFetchAdd(a_xp[i * DIM + XX], -tmp[XX] * inverseMassi);
            atomicFetchAdd(a_xp[i * DIM + YY], -tmp[YY] * inverseMassi);
            atomicFetchAdd(a_xp[i * DIM + ZZ], -tmp[ZZ] * inverseMassi);
            atomicFetchAdd(a_xp[j * DIM + XX], tmp[XX] * inverseMassj);
            atomicFetchAdd(a_xp[j * DIM + YY], tmp[YY] * inverseMassj);
            atomicFetchAdd(a_xp[j * DIM + ZZ], tmp[ZZ] * inverseMassj);
        }

        /*
         *  Correction for centripetal effects
         */
        for (int iter = 0; iter < numIterations; iter++)
        {
            // Make sure that all xp's are saved: atomic operation calls before are
            // communicating current xp[..] values across thread block.
            itemIdx.barrier(fence_space::global_and_local);

            if (!isDummyThread)
            {
                xi[XX] = atomicLoad(a_xp[i * DIM + XX]);
                xi[YY] = atomicLoad(a_xp[i * DIM + YY]);
                xi[ZZ] = atomicLoad(a_xp[i * DIM + ZZ]);
                xj[XX] = atomicLoad(a_xp[j * DIM + XX]);
                xj[YY] = atomicLoad(a_xp[j * DIM + YY]);
                xj[ZZ] = atomicLoad(a_xp[j * DIM + ZZ]);
            }

            Float3 dx;
            pbcDxAiucSycl(pbcAiuc, xi, xj, dx);

            float len2  = targetLength * targetLength;
            float dlen2 = 2.0F * len2 - (dx[XX] * dx[XX] + dx[YY] * dx[YY] + dx[ZZ] * dx[ZZ]);

            // TODO A little bit more effective but slightly less readable version of the below would be:
            //      float proj = sqrtReducedMass*(targetLength - (dlen2 > 0.0f ? 1.0f : 0.0f)*dlen2*rsqrt(dlen2));
            float proj;
            if (dlen2 > 0.0F)
            {
                proj = sqrtReducedMass * (targetLength - dlen2 * cl::sycl::rsqrt(dlen2));
            }
            else
            {
                proj = sqrtReducedMass * targetLength;
            }

            sm_rhs[threadInBlock] = proj;
            float sol             = proj;

            /*
             * Same matrix inversion as above is used for updated data
             */
            if constexpr (haveCoupledConstraints)
            {
                for (int rec = 0; rec < expansionOrder; rec++)
                {
                    // Make sure that all elements of rhs are saved into shared memory
                    itemIdx.barrier(fence_space::global_and_local);
                    float mvb = 0;
                    for (int n = 0; n < coupledConstraintsCount; n++)
                    {
                        int index = n * numConstraintsThreads + threadIndex;
                        int c1    = a_coupledConstraintsIndices[index];

                        mvb = mvb + a_matrixA[index] * sm_rhs[c1 + c_threadsPerBlock * (rec % 2)];
                    }

                    sm_rhs[threadInBlock + c_threadsPerBlock * ((rec + 1) % 2)] = mvb;
                    sol                                                         = sol + mvb;
                }
            }

            // Add corrections to Lagrange multipliers
            float sqrtmu_sol = sqrtReducedMass * sol;
            lagrangeScaled += sqrtmu_sol;

            // Save updated coordinates for the next iteration
            // Dummy constraints are skipped
            if (!isDummyThread)
            {
                Float3 tmp = rc * sqrtmu_sol;
                atomicFetchAdd(a_xp[i * DIM + XX], -tmp[XX] * inverseMassi);
                atomicFetchAdd(a_xp[i * DIM + YY], -tmp[YY] * inverseMassi);
                atomicFetchAdd(a_xp[i * DIM + ZZ], -tmp[ZZ] * inverseMassi);
                atomicFetchAdd(a_xp[j * DIM + XX], tmp[XX] * inverseMassj);
                atomicFetchAdd(a_xp[j * DIM + YY], tmp[YY] * inverseMassj);
                atomicFetchAdd(a_xp[j * DIM + ZZ], tmp[ZZ] * inverseMassj);
            }
        }

        // Updating particle velocities for all but dummy threads
        if constexpr (updateVelocities)
        {
            if (!isDummyThread)
            {
                Float3 tmp = rc * invdt * lagrangeScaled;
                atomicFetchAdd(a_v[i * DIM + XX], -tmp[XX] * inverseMassi);
                atomicFetchAdd(a_v[i * DIM + YY], -tmp[YY] * inverseMassi);
                atomicFetchAdd(a_v[i * DIM + ZZ], -tmp[ZZ] * inverseMassi);
                atomicFetchAdd(a_v[j * DIM + XX], tmp[XX] * inverseMassj);
                atomicFetchAdd(a_v[j * DIM + YY], tmp[YY] * inverseMassj);
                atomicFetchAdd(a_v[j * DIM + ZZ], tmp[ZZ] * inverseMassj);
            }
        }

        if constexpr (computeVirial)
        {
            // Virial is computed from Lagrange multiplier (lagrangeScaled), target constrain length
            // (targetLength) and the normalized vector connecting constrained atoms before
            // the algorithm was applied (rc). The evaluation of virial in each thread is
            // followed by basic reduction for the values inside single thread block.
            // Then, the values are reduced across grid by atomicAdd(...).
            //
            // TODO Shuffle reduction.
            // TODO Should be unified and/or done once when virial is actually needed.
            // TODO Recursive version that removes atomicAdd(...)'s entirely is needed. Ideally,
            //      one that works for any datatype.

            // Save virial for each thread into the shared memory. Tensor is symmetrical, hence only
            // 6 values are saved. Dummy threads will have zeroes in their virial: targetLength,
            // lagrangeScaled and rc are all set to zero for them in the beginning of the kernel.
            float mult                                             = targetLength * lagrangeScaled;
            sm_threadVirial[0 * c_threadsPerBlock + threadInBlock] = mult * rc[XX] * rc[XX];
            sm_threadVirial[1 * c_threadsPerBlock + threadInBlock] = mult * rc[XX] * rc[YY];
            sm_threadVirial[2 * c_threadsPerBlock + threadInBlock] = mult * rc[XX] * rc[ZZ];
            sm_threadVirial[3 * c_threadsPerBlock + threadInBlock] = mult * rc[YY] * rc[YY];
            sm_threadVirial[4 * c_threadsPerBlock + threadInBlock] = mult * rc[YY] * rc[ZZ];
            sm_threadVirial[5 * c_threadsPerBlock + threadInBlock] = mult * rc[ZZ] * rc[ZZ];

            itemIdx.barrier(fence_space::local_space);
            // This casts unsigned into signed integers to avoid clang warnings
            const int tib          = static_cast<int>(threadInBlock);
            const int blockSize    = static_cast<int>(c_threadsPerBlock);
            const int subGroupSize = itemIdx.get_sub_group().get_max_local_range()[0];

            // Reduce up to one virial per thread block
            // All blocks are divided by half, the first half of threads sums
            // two virials. Then the first half is divided by two and the first half
            // of it sums two values... The procedure continues until only one thread left.
            // Only works if the threads per blocks is a power of two.
            for (int divideBy = 2; divideBy <= blockSize; divideBy *= 2)
            {
                int dividedAt = blockSize / divideBy;
                if (tib < dividedAt)
                {
                    for (int d = 0; d < 6; d++)
                    {
                        sm_threadVirial[d * blockSize + tib] +=
                                sm_threadVirial[d * blockSize + (tib + dividedAt)];
                    }
                }
                if (dividedAt > subGroupSize / 2)
                {
                    itemIdx.barrier(fence_space::local_space);
                }
                else
                {
                    subGroupBarrier(itemIdx);
                }
            }
            // First 6 threads in the block add the 6 components of virial to the global memory address
            if (tib < 6)
            {
                atomicFetchAdd(a_virialScaled[tib], sm_threadVirial[tib * blockSize]);
            }
        }
    };
}

// SYCL 1.2.1 requires providing a unique type for a kernel. Should not be needed for SYCL2020.
template<bool updateVelocities, bool computeVirial, bool haveCoupledConstraints>
class LincsKernelName;

template<bool updateVelocities, bool computeVirial, bool haveCoupledConstraints, class... Args>
static cl::sycl::event launchLincsKernel(const DeviceStream& deviceStream,
                                         const int           numConstraintsThreads,
                                         Args&&... args)
{
    // Should not be needed for SYCL2020.
    using kernelNameType = LincsKernelName<updateVelocities, computeVirial, haveCoupledConstraints>;

    const cl::sycl::nd_range<1> rangeAllLincs(numConstraintsThreads, c_threadsPerBlock);
    cl::sycl::queue             q = deviceStream.stream();

    cl::sycl::event e = q.submit([&](cl::sycl::handler& cgh) {
        auto kernel = lincsKernel<updateVelocities, computeVirial, haveCoupledConstraints>(
                cgh, numConstraintsThreads, std::forward<Args>(args)...);
        cgh.parallel_for<kernelNameType>(rangeAllLincs, kernel);
    });

    return e;
}

/*! \brief Select templated kernel and launch it. */
template<class... Args>
static inline cl::sycl::event
launchLincsKernel(bool updateVelocities, bool computeVirial, bool haveCoupledConstraints, Args&&... args)
{
    return dispatchTemplatedFunction(
            [&](auto updateVelocities_, auto computeVirial_, auto haveCoupledConstraints_) {
                return launchLincsKernel<updateVelocities_, computeVirial_, haveCoupledConstraints_>(
                        std::forward<Args>(args)...);
            },
            updateVelocities,
            computeVirial,
            haveCoupledConstraints);
}


void launchLincsGpuKernel(LincsGpuKernelParameters*   kernelParams,
                          const DeviceBuffer<Float3>& d_x,
                          DeviceBuffer<Float3>        d_xp,
                          const bool                  updateVelocities,
                          DeviceBuffer<Float3>        d_v,
                          const real                  invdt,
                          const bool                  computeVirial,
                          const DeviceStream&         deviceStream)
{
    cl::sycl::buffer<Float3, 1> xp(*d_xp.buffer_);
    auto                        d_xpAsFloat = xp.reinterpret<float, 1>(xp.get_count() * DIM);

    cl::sycl::buffer<Float3, 1> v(*d_v.buffer_);
    auto                        d_vAsFloat = v.reinterpret<float, 1>(v.get_count() * DIM);

    launchLincsKernel(updateVelocities,
                      computeVirial,
                      kernelParams->haveCoupledConstraints,
                      deviceStream,
                      kernelParams->numConstraintsThreads,
                      kernelParams->d_constraints,
                      kernelParams->d_constraintsTargetLengths,
                      kernelParams->d_coupledConstraintsCounts,
                      kernelParams->d_coupledConstraintsIndices,
                      kernelParams->d_massFactors,
                      kernelParams->d_matrixA,
                      kernelParams->d_inverseMasses,
                      kernelParams->numIterations,
                      kernelParams->expansionOrder,
                      d_x,
                      d_xpAsFloat,
                      invdt,
                      d_vAsFloat,
                      kernelParams->d_virialScaled,
                      kernelParams->pbcAiuc);
    return;
}

} // namespace gmx
