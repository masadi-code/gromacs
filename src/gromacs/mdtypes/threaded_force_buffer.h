/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2021, by the GROMACS development team, led by
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
 * \brief This file contains the declaration of ThreadForceBuffer and ThreadedForceBuffer.
 *
 * These classes provides thread-local force, shift force and energy buffers
 * for kernels. These kernels can then run completely independently on
 * multiple threads. Their output can be reduced thread-parallel afterwards.
 *
 * Usage:
 *
 * At domain decomposition time:
 * Each thread calls: ThreadForceBuffer.resizeBufferAndClearMask()
 * Each thread calls: ThreadForceBuffer.addAtomToMask() for all atoms used in the buffer
 * Each thread calls: ThreadForceBuffer.processMask()
 * After that ThreadedForceBuffer.setupReduction() is called
 *
 * At force computation time:
 * Each thread calls: ThreadForceBuffer.clearForcesAndEnergies().
 * Each thread can then accumulate forces and energies into the buffers in ThreadForceBuffer.
 * After that ThreadedForceBuffer.reduce() is called for thread-parallel reduction.
 *
 * \author Berk Hess <hess@kth.se>
 * \ingroup mdtypes
 */
#ifndef GMX_MDTYPES_THREADED_FORCE_BUFFER_H
#define GMX_MDTYPES_THREADED_FORCE_BUFFER_H

#include <memory>

#include "gromacs/math/vectypes.h"
#include "gromacs/mdtypes/enerdata.h"
#include "gromacs/mdtypes/simulation_workload.h"
#include "gromacs/topology/ifunc.h"
#include "gromacs/utility/alignedallocator.h"
#include "gromacs/utility/arrayref.h"
#include "gromacs/utility/bitmask.h"
#include "gromacs/utility/classhelpers.h"

namespace gmx
{

class ForceWithShiftForces;
class StepWorkload;

/*! \internal
 * \brief Object that holds force and energies buffers plus a mask for a thread
 *
 * \tparam ForceBufferElementType  The type for components of the normal force buffer: rvec or rvec4
 */
template<typename ForceBufferElementType>
class ThreadForceBuffer
{
public:
    /* We reduce the force array in blocks of 2^5 atoms. This is large enough
     * to not cause overhead and 32*sizeof(rvec) is a multiple of the cache-line
     * size on all systems.
     */
    //! The log2 of the reduction block size
    static constexpr int s_numReductionBlockBits = 5;
    //! Force buffer block size in atoms
    static constexpr int s_reductionBlockSize = (1 << s_numReductionBlockBits);

    /*! \brief Constructor
     * \param[in] threadIndex  The index of the thread that will fill the buffers in this object
     * \param[in] useEnergyTerms   Whether the list of energy terms will be used
     * \param[in] numEnergyGroups  The number of non-bonded energy groups
     */
    ThreadForceBuffer(int threadIndex, bool useEnergyTerms, int numEnergyGroups);

    //! Resizes the buffer to \p numAtoms and clears the mask
    void resizeBufferAndClearMask(int numAtoms);

    //! Adds atom with index \p atomIndex for reduction
    void addAtomToMask(const int atomIndex)
    {
        bitmask_set_bit(&reductionMask_[atomIndex >> s_numReductionBlockBits], threadIndex_);
    }

    void processMask();

    //! Returns the size of the force buffer in number of atoms
    index size() const { return numAtoms_; }

    //! Clears all force and energy buffers
    void clearForcesAndEnergies();

    //! Returns a plain pointer to the force buffer
    ForceBufferElementType* forceBuffer()
    {
        return reinterpret_cast<ForceBufferElementType*>(forceBuffer_.data());
    }

    //! Returns a view of the shift force buffer
    ArrayRef<RVec> shiftForces() { return shiftForces_; }

    //! Returns a view of the energy terms, size F_NRE
    ArrayRef<real> energyTerms() { return energyTerms_; }

    //! Returns a reference to the energy group pair energies
    gmx_grppairener_t& groupPairEnergies() { return groupPairEnergies_; }

    //! Returns a reference to the dvdl terms
    EnumerationArray<FreeEnergyPerturbationCouplingType, real>& dvdl() { return dvdl_; }

    //! Returns a const view to the reduction masks
    ArrayRef<const gmx_bitmask_t> reductionMask() const { return reductionMask_; }

private:
    //! Force array buffer
    std::vector<real, AlignedAllocator<real>> forceBuffer_;
    //! Mask for marking which parts of f are filled, working array for constructing mask in bonded_threading_t
    std::vector<gmx_bitmask_t> reductionMask_;
    //! Index to touched blocks
    std::vector<int> usedBlockIndices_;
    //! The index of our thread
    int threadIndex_;
    //! The number of atoms in the buffer
    int numAtoms_ = 0;

    //! Shift force array, size c_numShiftVectors
    std::vector<RVec> shiftForces_;
    //! Energy array, can be empty
    std::vector<real> energyTerms_;
    //! Group pair energy data for pairs
    gmx_grppairener_t groupPairEnergies_;
    //! Free-energy dV/dl output
    gmx::EnumerationArray<FreeEnergyPerturbationCouplingType, real> dvdl_;

    // Disallow copy and assign, remove this we we get rid of f_
    GMX_DISALLOW_COPY_MOVE_AND_ASSIGN(ThreadForceBuffer);
};

/*! \internal
 * \brief Class for accumulating and reducing forces and energies on threads in parallel
 *
 * \tparam ForceBufferElementType  The type for components of the normal force buffer: rvec or rvec4
 */
template<typename ForceBufferElementType>
class ThreadedForceBuffer
{
public:
    /*! \brief Constructor
     * \param[in] numThreads       The number of threads that will use the buffers and reduce
     * \param[in] useEnergyTerms   Whether the list of energy terms will be used
     * \param[in] numEnergyGroups  The number of non-bonded energy groups
     */
    ThreadedForceBuffer(int numThreads, bool useEnergyTerms, int numEnergyGroups);

    //! Returns the number of thread buffers
    int numThreadBuffers() const { return threadForceBuffers_.size(); }

    //! Returns a reference to the buffer object for the thread with index \p bufferIndex
    ThreadForceBuffer<ForceBufferElementType>& threadForceBuffer(int bufferIndex)
    {
        return *threadForceBuffers_[bufferIndex];
    }

    //! Sets up the reduction, should be called after generating the masks on each thread
    void setupReduction();

    /*! \brief Reduces forces and energies, as requested by \p stepWork
     *
     * The reduction of all output starts at the output from thread \p reductionBeginIndex,
     * except for the normal force buffer, which always starts at 0.
     *
     * Buffers that will not be used as indicated by the flags in \p stepWork
     * are allowed to be nullptr or empty.
     */
    void reduce(gmx::ForceWithShiftForces* forceWithShiftForces,
                real*                      ener,
                gmx_grppairener_t*         grpp,
                gmx::ArrayRef<real>        dvdl,
                const gmx::StepWorkload&   stepWork,
                int                        reductionBeginIndex);

private:
    //! Whether the energy buffer is used
    bool useEnergyTerms_;
    //! Force/energy data per thread, size nthreads, stored in unique_ptr to allow thread local allocation
    std::vector<std::unique_ptr<ThreadForceBuffer<ForceBufferElementType>>> threadForceBuffers_;
    //! Indices of blocks that are used, i.e. have force contributions.
    std::vector<int> usedBlockIndices_;
    //! Mask array, one element corresponds to a block of reduction_block_size atoms of the force array, bit corresponding to thread indices set if a thread writes to that block
    std::vector<gmx_bitmask_t> reductionMask_;
    //! The number of atoms forces are computed for
    int numAtomsForce_ = 0;

    // Disallow copies to avoid sub-optimal ownership of allocated memory
    GMX_DISALLOW_COPY_MOVE_AND_ASSIGN(ThreadedForceBuffer);
};

// Instantiate for RVec
extern template class ThreadForceBuffer<RVec>;
extern template class ThreadedForceBuffer<RVec>;

// Instantiate for rvec4
extern template class ThreadForceBuffer<rvec4>;
extern template class ThreadedForceBuffer<rvec4>;

} // namespace gmx

#endif
