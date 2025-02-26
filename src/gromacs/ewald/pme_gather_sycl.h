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
 *  \brief Implements PME GPU gather in SYCL.
 *
 *  \author Andrey Alekseenko <al42and@gmail.com>
 */

#include "gromacs/gpu_utils/gmxsycl.h"

#include "gromacs/gpu_utils/syclutils.h"

#include "pme_grid.h"
#include "pme_gpu_types_host.h"

struct PmeGpuGridParams;
struct PmeGpuAtomParams;
struct PmeGpuDynamicParams;

template<int order, bool wrapX, bool wrapY, int numGrids, bool readGlobal, ThreadsPerAtom threadsPerAtom, int subGroupSize>
class PmeGatherKernel : public ISyclKernelFunctor
{
public:
    PmeGatherKernel();
    void            setArg(size_t argIndex, void* arg) override;
    cl::sycl::event launch(const KernelLaunchConfig& config, const DeviceStream& deviceStream) override;

private:
    PmeGpuGridParams*    gridParams_;
    PmeGpuAtomParams*    atomParams_;
    PmeGpuDynamicParams* dynamicParams_;
    void                 reset();
};
