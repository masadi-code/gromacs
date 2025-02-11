/*
 * This file is part of the GROMACS molecular simulation package.
 *
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
 * \brief
 * Test routines that parse mdp fields from grompp input and writes
 * mdp back out.
 *
 * In particular these will provide test coverage as we refactor to
 * use a new Options-based key-value-style mdp implementation to
 * support a more modular mdrun.
 *
 * \author Mark Abraham <mark.j.abraham@gmail.com>
 */
#include "gmxpre.h"

#include "gromacs/gmxpreprocess/readir.h"

#include <string>

#include <gtest/gtest.h>

#include "gromacs/fileio/warninp.h"
#include "gromacs/mdrun/mdmodules.h"
#include "gromacs/mdtypes/inputrec.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/stringutil.h"
#include "gromacs/utility/textreader.h"
#include "gromacs/utility/textwriter.h"
#include "gromacs/utility/unique_cptr.h"

#include "testutils/refdata.h"
#include "testutils/testasserts.h"
#include "testutils/testfilemanager.h"
namespace gmx
{
namespace test
{

class GetIrTest : public ::testing::Test
{
public:
    GetIrTest() : wi_(init_warning(FALSE, 0)), wiGuard_(wi_)
    {
        snew(opts_.include, STRLEN);
        snew(opts_.define, STRLEN);
    }
    ~GetIrTest() override
    {
        done_inputrec_strings();
        sfree(opts_.include);
        sfree(opts_.define);
    }

    //! Tells whether warnings and/or errors are expected from inputrec parsing and checking, and whether we should compare the output
    enum class TestBehavior
    {
        NoErrorAndCompareOutput,   //!< Expect no warnings/error and compare output
        ErrorAndCompareOutput,     //!< Expect at least one warning/error and compare output
        ErrorAndDoNotCompareOutput //!< Expect at least one warning/error and do not compare output
    };

    /*! \brief Test mdp reading and writing
     *
     * \todo Modernize read_inp and write_inp to use streams,
     * which will make these tests run faster, because they don't
     * use disk files. */
    void runTest(const std::string& inputMdpFileContents,
                 const TestBehavior testBehavior = TestBehavior::NoErrorAndCompareOutput)
    {
        const bool expectError   = testBehavior != TestBehavior::NoErrorAndCompareOutput;
        const bool compareOutput = testBehavior != TestBehavior::ErrorAndDoNotCompareOutput;

        std::string inputMdpFilename = fileManager_.getTemporaryFilePath("input.mdp");
        std::string outputMdpFilename;
        if (compareOutput)
        {
            outputMdpFilename = fileManager_.getTemporaryFilePath("output.mdp");
        }

        TextWriter::writeFileFromString(inputMdpFilename, inputMdpFileContents);

        get_ir(inputMdpFilename.c_str(),
               outputMdpFilename.empty() ? nullptr : outputMdpFilename.c_str(),
               &mdModules_,
               &ir_,
               &opts_,
               WriteMdpHeader::no,
               wi_);

        check_ir(inputMdpFilename.c_str(), mdModules_.notifiers(), &ir_, &opts_, wi_);
        // Now check
        bool failure = warning_errors_exist(wi_);
        EXPECT_EQ(failure, expectError);

        if (compareOutput)
        {
            TestReferenceData    data;
            TestReferenceChecker checker(data.rootChecker());
            checker.checkBoolean(failure, "Error parsing mdp file");
            warning_reset(wi_);

            auto outputMdpContents = TextReader::readFileToString(outputMdpFilename);
            checker.checkString(outputMdpContents, "OutputMdpFile");
        }
    }

    TestFileManager                    fileManager_;
    t_inputrec                         ir_;
    MDModules                          mdModules_;
    t_gromppopts                       opts_;
    warninp_t                          wi_;
    unique_cptr<warninp, free_warning> wiGuard_;
};

TEST_F(GetIrTest, HandlesDifferentKindsOfMdpLines)
{
    const char* inputMdpFile[] = { "; File to run my simulation",
                                   "title = simulation",
                                   "define = -DBOOLVAR -DVAR=VALUE",
                                   ";",
                                   "xtc_grps = System ; was Protein",
                                   "include = -I/home/me/stuff",
                                   "",
                                   "tau-t = 0.1 0.3",
                                   "ref-t = ;290 290",
                                   "tinit = 0.3",
                                   "init_step = 0",
                                   "nstcomm = 100",
                                   "integrator = steep" };
    runTest(joinStrings(inputMdpFile, "\n"));
}

TEST_F(GetIrTest, RejectsNonCommentLineWithNoEquals)
{
    const char* inputMdpFile = "title simulation";
    GMX_EXPECT_DEATH_IF_SUPPORTED(runTest(inputMdpFile), "No '=' to separate");
}

TEST_F(GetIrTest, AcceptsKeyWithoutValue)
{
    // Users are probably using lines like this
    const char* inputMdpFile = "xtc_grps = ";
    runTest(inputMdpFile);
}

TEST_F(GetIrTest, RejectsValueWithoutKey)
{
    const char* inputMdpFile = "= -I/home/me/stuff";
    GMX_EXPECT_DEATH_IF_SUPPORTED(runTest(inputMdpFile), "No .mdp parameter name was found");
}

TEST_F(GetIrTest, RejectsEmptyKeyAndEmptyValue)
{
    const char* inputMdpFile = " = ";
    GMX_EXPECT_DEATH_IF_SUPPORTED(runTest(inputMdpFile),
                                  "No .mdp parameter name or value was found");
}

TEST_F(GetIrTest, AcceptsDefineParametersWithValuesIncludingAssignment)
{
    const char* inputMdpFile[] = {
        "define = -DBOOL -DVAR=VALUE",
    };
    runTest(joinStrings(inputMdpFile, "\n"));
}

TEST_F(GetIrTest, AcceptsEmptyLines)
{
    const char* inputMdpFile = "";
    runTest(inputMdpFile);
}

TEST_F(GetIrTest, MtsCheckNstcalcenergy)
{
    const char* inputMdpFile[] = {
        "mts = yes", "mts-levels = 2", "mts-level2-factor = 2", "nstcalcenergy = 5"
    };
    runTest(joinStrings(inputMdpFile, "\n"), TestBehavior::ErrorAndDoNotCompareOutput);
}

TEST_F(GetIrTest, MtsCheckNstenergy)
{
    const char* inputMdpFile[] = {
        "mts = yes", "mts-levels = 2", "mts-level2-factor = 2", "nstenergy = 5"
    };
    runTest(joinStrings(inputMdpFile, "\n"), TestBehavior::ErrorAndDoNotCompareOutput);
}

TEST_F(GetIrTest, MtsCheckNstpcouple)
{
    const char* inputMdpFile[] = { "mts = yes",
                                   "mts-levels = 2",
                                   "mts-level2-factor = 2",
                                   "pcoupl = Berendsen",
                                   "nstpcouple = 5" };
    runTest(joinStrings(inputMdpFile, "\n"), TestBehavior::ErrorAndDoNotCompareOutput);
}

TEST_F(GetIrTest, MtsCheckNstdhdl)
{
    const char* inputMdpFile[] = {
        "mts = yes", "mts-level2-factor = 2", "free-energy = yes", "nstdhdl = 5"
    };
    runTest(joinStrings(inputMdpFile, "\n"), TestBehavior::ErrorAndDoNotCompareOutput);
}

// These tests observe how the electric-field keys behave, since they
// are currently the only ones using the new Options-style handling.
TEST_F(GetIrTest, AcceptsElectricField)
{
    const char* inputMdpFile = "electric-field-x = 1.2 0 0 0";
    runTest(inputMdpFile);
}

TEST_F(GetIrTest, AcceptsElectricFieldPulsed)
{
    const char* inputMdpFile = "electric-field-y = 3.7 2.0 6.5 1.0";
    runTest(inputMdpFile);
}

TEST_F(GetIrTest, AcceptsElectricFieldOscillating)
{
    const char* inputMdpFile = "electric-field-z = 3.7 7.5 0 0";
    runTest(inputMdpFile);
}

TEST_F(GetIrTest, RejectsDuplicateOldAndNewKeys)
{
    const char* inputMdpFile[] = { "verlet-buffer-drift = 1.3", "verlet-buffer-tolerance = 2.7" };
    GMX_EXPECT_DEATH_IF_SUPPORTED(runTest(joinStrings(inputMdpFile, "\n")),
                                  "A parameter is present with both");
}

TEST_F(GetIrTest, AcceptsImplicitSolventNo)
{
    const char* inputMdpFile = "implicit-solvent = no";
    runTest(inputMdpFile);
}

TEST_F(GetIrTest, RejectsImplicitSolventYes)
{
    const char* inputMdpFile = "implicit-solvent = yes";
    GMX_EXPECT_DEATH_IF_SUPPORTED(runTest(inputMdpFile), "Invalid enum");
}

TEST_F(GetIrTest, AcceptsMimic)
{
    const char* inputMdpFile[] = { "integrator = mimic", "QMMM-grps = QMatoms" };
    runTest(joinStrings(inputMdpFile, "\n"));
}

} // namespace test
} // namespace gmx
