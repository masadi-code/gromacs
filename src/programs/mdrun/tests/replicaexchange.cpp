/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2013,2014,2015,2016,2018 by the GROMACS development team.
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
 * \brief
 * Tests for the mdrun replica-exchange functionality
 *
 * \author Mark Abraham <mark.j.abraham@gmail.com>
 * \ingroup module_mdrun_integration_tests
 */
#include "gmxpre.h"

#include "config.h"

#include <regex>

#include <gtest/gtest.h>

#include "gromacs/mdtypes/md_enums.h"
#include "gromacs/topology/ifunc.h"
#include "gromacs/utility/basenetwork.h"
#include "gromacs/utility/filestream.h"
#include "gromacs/utility/path.h"
#include "gromacs/utility/stringutil.h"

#include "testutils/refdata.h"
#include "testutils/testfilemanager.h"

#include "energycomparison.h"
#include "multisimtest.h"
#include "trajectorycomparison.h"

namespace gmx
{
namespace test
{

//! Convenience typedef
typedef MultiSimTest ReplicaExchangeEnsembleTest;

TEST_P(ReplicaExchangeEnsembleTest, ExitsNormally)
{
    mdrunCaller_->addOption("-replex", 1);
    runExitsNormallyTest();
}

/* Note, not all preprocessor implementations nest macro expansions
   the same way / at all, if we would try to duplicate less code. */

#if GMX_LIB_MPI
INSTANTIATE_TEST_SUITE_P(
        WithDifferentControlVariables,
        ReplicaExchangeEnsembleTest,
        ::testing::Combine(::testing::Values(NumRanksPerSimulation(1), NumRanksPerSimulation(2)),
                           ::testing::Values(IntegrationAlgorithm::MD),
                           ::testing::Values(TemperatureCoupling::VRescale),
                           ::testing::Values(PressureCoupling::No, PressureCoupling::Berendsen)));
#else
INSTANTIATE_TEST_SUITE_P(
        DISABLED_WithDifferentControlVariables,
        ReplicaExchangeEnsembleTest,
        ::testing::Combine(::testing::Values(NumRanksPerSimulation(1), NumRanksPerSimulation(2)),
                           ::testing::Values(IntegrationAlgorithm::MD),
                           ::testing::Values(TemperatureCoupling::VRescale),
                           ::testing::Values(PressureCoupling::No, PressureCoupling::Berendsen)));
#endif

//! Convenience typedef
typedef MultiSimTest ReplicaExchangeTerminationTest;

TEST_P(ReplicaExchangeTerminationTest, WritesCheckpointAfterMaxhTerminationAndThenRestarts)
{
    mdrunCaller_->addOption("-replex", 1);
    runMaxhTest();
}

INSTANTIATE_TEST_SUITE_P(InNvt,
                         ReplicaExchangeTerminationTest,
                         ::testing::Combine(::testing::Values(NumRanksPerSimulation(1)),
                                            ::testing::Values(IntegrationAlgorithm::MD),
                                            ::testing::Values(TemperatureCoupling::VRescale),
                                            ::testing::Values(PressureCoupling::No)));

/*! \brief Return replica exchange related output from logfile
 *
 * All replica exchange related output in log files start with 'Repl',
 * making extraction easy. This function also removes the printing of
 * energy differences, as the log files are compared exactly, and
 * energy differences will slightly vary between runs.
 *
 * \param logFileName  Name of log file
 * \return  Replica exchange related output in log file
 */
static std::string getReplicaExchangeOutputFromLogFile(const std::string& logFileName)
{
    TextInputFile logFile(logFileName);
    std::string   replExOutput;
    std::string   line;
    while (logFile.readLine(&line))
    {
        // All replica exchange output lines starts with "Repl"
        if (startsWith(line, "Repl"))
        {
            // This is an exact comparison, so we can't compare the energies which
            // are slightly different per run. Energies are tested later.
            const auto pos = line.find("dE_term");
            if (pos != std::string::npos)
            {
                line.replace(line.begin() + pos, line.end(), "[ not checked ]\n");
            }
            replExOutput.append(line);
        }
    }
    return replExOutput;
}

//! Convenience typedef
typedef MultiSimTest ReplicaExchangeRegressionTest;

/* Run replica exchange simulations, compare to reference data
 *
 * Reference data generated by
 *
 * GROMACS version:    2022-dev
 * Precision:          single and double (separate reference data)
 * Memory model:       64 bit
 * MPI library:        MPI
 * OpenMP support:     enabled (GMX_OPENMP_MAX_THREADS = 64)
 * GPU support:        disabled
 * SIMD instructions:  AVX2_256
 * FFT library:        fftw-3.3.9-sse2-avx
 * RDTSCP usage:       enabled
 * TNG support:        enabled
 * Hwloc support:      disabled
 * Tracing support:    disabled
 * C compiler:         /usr/local/bin/mpicc Clang 8.0.1
 * C++ compiler:       /usr/local/bin/mpic++ Clang 8.0.1
 *
 */
TEST_P(ReplicaExchangeRegressionTest, WithinTolerances)
{
    if (!mpiSetupValid())
    {
        // Can't test multi-sim without multiple simulations
        return;
    }

    if (size_ != 4)
    {
        // Results are depending on number of ranks, and we can't have reference
        // data for all cases. Restricting the regression tests to runs with 4 ranks.
        // This allows testing 4 replicas with single rank, or 2 replicas with 2 ranks each.
        return;
    }
    const auto& tcoupl = std::get<2>(GetParam());
    const auto& pcoupl = std::get<3>(GetParam());

    const int numSteps       = 16;
    const int exchangePeriod = 4;
    // grompp warns about generating velocities and using parrinello-rahman
    const int maxWarnings = (pcoupl == PressureCoupling::ParrinelloRahman ? 1 : 0);

    mdrunCaller_->addOption("-replex", exchangePeriod);
    // Seeds need to be reproducible for regression, but can be different per simulation
    mdrunCaller_->addOption("-reseed", 98713 + simulationNumber_);

    SimulationRunner runner(&fileManager_);
    runner.useTopGroAndNdxFromDatabase("tip3p5");

    runGrompp(&runner, numSteps, true, maxWarnings);
    ASSERT_EQ(0, runner.callMdrun(*mdrunCaller_));

#if GMX_LIB_MPI
    // Make sure all simulations are finished before checking the results.
    MPI_Barrier(MdrunTestFixtureBase::communicator_);
#endif

    // We only test simulation results on one rank to avoid problems with reference file access.
    if (rank_ == 0)
    {
        // Create reference data helper object
        TestReferenceData refData;

        // Specify how energy trajectory comparison must work
        const auto hasConservedField =
                !(tcoupl == TemperatureCoupling::No && pcoupl == PressureCoupling::No);
        // Tolerances copied from simulator tests
        EnergyTermsToCompare energyTermsToCompare{ {
                { interaction_function[F_EPOT].longname,
                  relativeToleranceAsPrecisionDependentUlp(60.0, 200, 160) },
                { interaction_function[F_EKIN].longname,
                  relativeToleranceAsPrecisionDependentUlp(60.0, 200, 160) },
        } };
        if (hasConservedField)
        {
            energyTermsToCompare.emplace(interaction_function[F_ECONSERVED].longname,
                                         relativeToleranceAsPrecisionDependentUlp(50.0, 100, 80));
        }
        if (pcoupl != PressureCoupling::No)
        {
            energyTermsToCompare.emplace("Volume",
                                         relativeToleranceAsPrecisionDependentUlp(10.0, 200, 160));
        }

        // Specify how trajectory frame matching must work.
        const TrajectoryFrameMatchSettings trajectoryMatchSettings{ true,
                                                                    true,
                                                                    true,
                                                                    ComparisonConditions::MustCompare,
                                                                    ComparisonConditions::MustCompare,
                                                                    ComparisonConditions::MustCompare,
                                                                    MaxNumFrames::compareAllFrames() };
        TrajectoryTolerances trajectoryTolerances = TrajectoryComparison::s_defaultTrajectoryTolerances;
        // By default, velocity tolerance is MUCH tighter than force tolerance
        trajectoryTolerances.velocities = trajectoryTolerances.forces;
        // Build the functor that will compare reference and test
        // trajectory frames in the chosen way.
        TrajectoryComparison trajectoryComparison{ trajectoryMatchSettings, trajectoryTolerances };

        // Loop over simulations
        for (int simulationNumber = 0; simulationNumber < (size_ / numRanksPerSimulation_);
             simulationNumber++)
        {
            TestReferenceChecker simulationChecker(refData.rootChecker().checkCompound(
                    "Simulation", formatString("Replica %d", simulationNumber)));

            const auto logFileName =
                    std::regex_replace(runner.logFileName_,
                                       std::regex(formatString("sim_%d", simulationNumber_)),
                                       formatString("sim_%d", simulationNumber));
            const auto energyFileName =
                    std::regex_replace(runner.edrFileName_,
                                       std::regex(formatString("sim_%d", simulationNumber_)),
                                       formatString("sim_%d", simulationNumber));
            const auto trajectoryFileName =
                    std::regex_replace(runner.fullPrecisionTrajectoryFileName_,
                                       std::regex(formatString("sim_%d", simulationNumber_)),
                                       formatString("sim_%d", simulationNumber));

            // Check log replica exchange related output (contains exchange statistics)
            auto replicaExchangeOutputChecker =
                    simulationChecker.checkCompound("ReplExOutput", "Output");
            const auto replExOutput = getReplicaExchangeOutputFromLogFile(logFileName);
            replicaExchangeOutputChecker.checkTextBlock(replExOutput, "Replica Exchange Output");

            // Check that the energies agree with the refdata within tolerance.
            checkEnergiesAgainstReferenceData(energyFileName, energyTermsToCompare, &simulationChecker);

            // Check that the trajectories agree with the refdata within tolerance.
            checkTrajectoryAgainstReferenceData(trajectoryFileName, trajectoryComparison, &simulationChecker);

        } // end loop over simulations
    }     // end testing simulations on one rank

#if GMX_LIB_MPI
    // Make sure testing is complete before returning - ranks delete temporary files on exit
    MPI_Barrier(MdrunTestFixtureBase::communicator_);
#endif
}

/*! \brief Helper struct printing custom test name
 *
 * Regression test results not only depend on the test parameters, but
 * also on the total number of ranks and the precision. Names must
 * reflect that to identify correct reference data.
 */
struct PrintReplicaExchangeParametersToString
{
    template<class ParamType>
    std::string operator()(const testing::TestParamInfo<ParamType>& parameter) const
    {
        auto testIdentifier =
                formatString("ReplExRegression_%s_%s_%s_%dRanks_%dRanksPerSimulation_%s",
                             enumValueToString(std::get<1>(parameter.param)),
                             enumValueToString(std::get<2>(parameter.param)),
                             enumValueToString(std::get<3>(parameter.param)),
                             gmx_node_num(),
                             static_cast<int>(std::get<0>(parameter.param)),
                             GMX_DOUBLE ? "d" : "s");
        // Valid GTest names cannot include hyphens
        testIdentifier.erase(std::remove(testIdentifier.begin(), testIdentifier.end(), '-'),
                             testIdentifier.end());
        return testIdentifier;
    }
};

#if GMX_LIB_MPI
INSTANTIATE_TEST_SUITE_P(
        ReplicaExchangeIsEquivalentToReferenceLeapFrog,
        ReplicaExchangeRegressionTest,
        ::testing::Combine(::testing::Values(NumRanksPerSimulation(1), NumRanksPerSimulation(2)),
                           ::testing::Values(IntegrationAlgorithm::MD),
                           ::testing::Values(TemperatureCoupling::VRescale, TemperatureCoupling::NoseHoover),
                           ::testing::Values(PressureCoupling::CRescale, PressureCoupling::ParrinelloRahman)),
        PrintReplicaExchangeParametersToString());
INSTANTIATE_TEST_SUITE_P(ReplicaExchangeIsEquivalentToReferenceVelocityVerlet,
                         ReplicaExchangeRegressionTest,
                         ::testing::Combine(::testing::Values(NumRanksPerSimulation(1),
                                                              NumRanksPerSimulation(2)),
                                            ::testing::Values(IntegrationAlgorithm::VV),
                                            ::testing::Values(TemperatureCoupling::NoseHoover),
                                            ::testing::Values(PressureCoupling::No)),
                         PrintReplicaExchangeParametersToString());
#else
INSTANTIATE_TEST_SUITE_P(
        DISABLED_ReplicaExchangeIsEquivalentToReferenceLeapFrog,
        ReplicaExchangeRegressionTest,
        ::testing::Combine(::testing::Values(NumRanksPerSimulation(1), NumRanksPerSimulation(2)),
                           ::testing::Values(IntegrationAlgorithm::MD),
                           ::testing::Values(TemperatureCoupling::VRescale, TemperatureCoupling::NoseHoover),
                           ::testing::Values(PressureCoupling::CRescale, PressureCoupling::ParrinelloRahman)),
        PrintReplicaExchangeParametersToString());
INSTANTIATE_TEST_SUITE_P(DISABLED_ReplicaExchangeIsEquivalentToReferenceVelocityVerlet,
                         ReplicaExchangeRegressionTest,
                         ::testing::Combine(::testing::Values(NumRanksPerSimulation(1),
                                                              NumRanksPerSimulation(2)),
                                            ::testing::Values(IntegrationAlgorithm::VV),
                                            ::testing::Values(TemperatureCoupling::NoseHoover),
                                            ::testing::Values(PressureCoupling::No)),
                         PrintReplicaExchangeParametersToString());
#endif
} // namespace test
} // namespace gmx
