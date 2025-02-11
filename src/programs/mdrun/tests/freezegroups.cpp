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
 * \brief End-to-end tests checking sanity of results of simulations
 *        containing freeze groups
 *
 * \author Pascal Merz <pascal.merz@me.com>
 * \ingroup module_mdrun_integration_tests
 */
#include "gmxpre.h"

#include "config.h"

#include "gromacs/topology/ifunc.h"
#include "gromacs/utility/stringutil.h"

#include "testutils/mpitest.h"
#include "testutils/simulationdatabase.h"
#include "testutils/testmatchers.h"

#include "moduletest.h"
#include "simulatorcomparison.h"
#include "trajectoryreader.h"

namespace gmx::test
{
namespace
{
/*! \brief Test fixture checking sanity of freeze group results
 *
 * This tests the sanity of simulation results containing fully and partially
 * frozen atoms. For fully frozen atoms, it checks that their reported position
 * is identical for all steps, and that their velocity is zero. For partially
 * frozen atoms (for simplicity only in z-direction), it checks that their
 * position is identical in the frozen dimension for all steps, and that their
 * velocity is zero in the frozen dimension.
 */
using FreezeGroupTestParams = std::tuple<std::string, std::string, std::string>;
class FreezeGroupTest : public MdrunTestFixture, public ::testing::WithParamInterface<FreezeGroupTestParams>
{
public:
    //! Check that the frozen positions don't change and velocities are zero
    static void checkFreezeGroups(const std::string&           trajectoryName,
                                  ArrayRef<const unsigned int> fullyFrozenAtoms,
                                  ArrayRef<const unsigned int> partiallyFrozenAtomsDimZ,
                                  const TrajectoryTolerances&  tolerances)
    {
        auto [fullyFrozenPositions, fullyFrozenVelocities] =
                getFrozenPositionsAndVelocities(trajectoryName, fullyFrozenAtoms);
        auto [partiallyFrozenPositions, partiallyFrozenVelocities] =
                getFrozenPositionsAndVelocities(trajectoryName, partiallyFrozenAtomsDimZ);
        GMX_RELEASE_ASSERT(fullyFrozenPositions.size() == fullyFrozenVelocities.size(),
                           "Position and velocity trajectory don't have the same length.");
        GMX_RELEASE_ASSERT(partiallyFrozenPositions.size() == partiallyFrozenVelocities.size(),
                           "Position and velocity trajectory don't have the same length.");
        GMX_RELEASE_ASSERT(fullyFrozenPositions.size() == partiallyFrozenPositions.size(),
                           "Fully and partially frozen trajectory don't have the same length.");
        const auto trajectorySize = fullyFrozenPositions.size();

        for (auto frameIdx = decltype(trajectorySize){ 0 }; frameIdx < trajectorySize; frameIdx++)
        {
            SCOPED_TRACE(formatString("Checking frame %lu", frameIdx + 1));
            if (frameIdx > 0)
            {
                checkFullyFrozenPositions(
                        fullyFrozenPositions[frameIdx], fullyFrozenPositions[frameIdx - 1], tolerances);
                checkZDimFrozenPositions(partiallyFrozenPositions[frameIdx],
                                         partiallyFrozenPositions[frameIdx - 1],
                                         tolerances);
            }
            checkFullyFrozenVelocities(fullyFrozenVelocities[frameIdx], tolerances);
            checkZDimFrozenVelocities(partiallyFrozenVelocities[frameIdx], tolerances);
        }
    }

    //! Check that fully frozen frame velocities are zero
    static void checkFullyFrozenVelocities(ArrayRef<const RVec>        velocities,
                                           const TrajectoryTolerances& tolerances)
    {
        SCOPED_TRACE("Checking fully frozen velocity frame");
        std::vector<RVec> zeroVelocities(velocities.size(), RVec{ 0, 0, 0 });
        EXPECT_THAT(zeroVelocities, Pointwise(RVecEq(tolerances.velocities), velocities));
    }
    //! Check that z-dimension frozen frame velocities are zero
    static void checkZDimFrozenVelocities(ArrayRef<const RVec>        velocities,
                                          const TrajectoryTolerances& tolerances)
    {
        SCOPED_TRACE("Checking z-dimension frozen velocity frame");
        std::vector<real> zVelocities;
        for (const auto& v : velocities)
        {
            zVelocities.emplace_back(v[ZZ]);
        }
        std::vector<real> zeroVelocities(zVelocities.size(), 0);
        EXPECT_THAT(zeroVelocities, Pointwise(RealEq(tolerances.velocities), zVelocities));
    }
    //! Check that fully frozen frame positions are static
    static void checkFullyFrozenPositions(ArrayRef<const RVec>        positions,
                                          ArrayRef<const RVec>        previousPositions,
                                          const TrajectoryTolerances& tolerances)
    {
        SCOPED_TRACE("Checking fully frozen position frame");
        EXPECT_THAT(previousPositions, Pointwise(RVecEq(tolerances.coordinates), positions));
    }
    //! Check that z-dimension frozen frame positions are zero
    static void checkZDimFrozenPositions(ArrayRef<const RVec>        positions,
                                         ArrayRef<const RVec>        previousPositions,
                                         const TrajectoryTolerances& tolerances)
    {
        SCOPED_TRACE("Checking z-dimension frozen position frame");
        std::vector<real> zPositions;
        for (const auto& p : positions)
        {
            zPositions.emplace_back(p[ZZ]);
        }
        std::vector<real> zPrevPositions;
        for (const auto& p : previousPositions)
        {
            zPrevPositions.emplace_back(p[ZZ]);
        }
        EXPECT_THAT(zPrevPositions, Pointwise(RealEq(tolerances.coordinates), zPositions));
    }

    static std::tuple<std::vector<std::vector<RVec>>, std::vector<std::vector<RVec>>>
    getFrozenPositionsAndVelocities(const std::string& trajectoryName, ArrayRef<const unsigned int> frozenAtoms)
    {
        std::vector<std::vector<RVec>> positions;
        std::vector<std::vector<RVec>> velocities;

        TrajectoryFrameReader trajectoryFrameReader(trajectoryName);
        while (trajectoryFrameReader.readNextFrame())
        {
            const auto frame = trajectoryFrameReader.frame();
            positions.emplace_back();
            velocities.emplace_back();
            for (const auto& index : frozenAtoms)
            {
                positions.back().emplace_back(frame.x().at(index));
                velocities.back().emplace_back(frame.v().at(index));
            }
        }

        return { std::move(positions), std::move(velocities) };
    }
};

TEST_P(FreezeGroupTest, WithinTolerances)
{
    const auto& params         = GetParam();
    const auto& integrator     = std::get<0>(params);
    const auto& tcoupling      = std::get<1>(params);
    const auto& pcoupling      = std::get<2>(params);
    const auto& simulationName = "alanine_vacuo";

    constexpr std::array<unsigned int, 5>  backbone   = { 4, 6, 8, 14, 16 };
    constexpr std::array<unsigned int, 13> sideChainH = { 0,  1,  2,  3,  9,  10, 11,
                                                          12, 13, 18, 19, 20, 21 };

    if (integrator == "md-vv" && pcoupling == "parrinello-rahman")
    {
        // Parrinello-Rahman is not implemented in md-vv
        return;
    }

    // Prepare mdp input
    auto mdpFieldValues = prepareMdpFieldValues(simulationName, integrator, tcoupling, pcoupling);
    mdpFieldValues["nsteps"]      = "8";
    mdpFieldValues["nstxout"]     = "4";
    mdpFieldValues["nstvout"]     = "4";
    mdpFieldValues["freezegrps"]  = "Backbone SideChain";
    mdpFieldValues["freezedim"]   = "Y Y Y N N Y";
    mdpFieldValues["constraints"] = "all-bonds";

    // Run grompp
    runner_.useTopGroAndNdxFromDatabase(simulationName);
    runner_.useStringAsMdpFile(prepareMdpFileContents(mdpFieldValues));
    // Allow one warning for COMM removal + partially frozen atoms
    runGrompp(&runner_, { SimulationOptionTuple("-maxwarn", "1") });
    // Run mdrun
    runMdrun(&runner_);

    // Check frozen atoms
    checkFreezeGroups(runner_.fullPrecisionTrajectoryFileName_,
                      backbone,
                      sideChainH,
                      TrajectoryComparison::s_defaultTrajectoryTolerances);
}

INSTANTIATE_TEST_SUITE_P(FreezeWorks,
                         FreezeGroupTest,
                         ::testing::Combine(::testing::Values("md", "md-vv", "sd", "bd"),
                                            ::testing::Values("no"),
                                            ::testing::Values("no")));
} // namespace
} // namespace gmx::test
