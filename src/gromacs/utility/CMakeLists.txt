#
# This file is part of the GROMACS molecular simulation package.
#
# Copyright (c) 2010,2011,2012,2013,2014 by the GROMACS development team.
# Copyright (c) 2015,2017,2018,2019,2020 by the GROMACS development team.
# Copyright (c) 2021, by the GROMACS development team, led by
# Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
# and including many others, as listed in the AUTHORS file in the
# top-level source directory and at http://www.gromacs.org.
#
# GROMACS is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public License
# as published by the Free Software Foundation; either version 2.1
# of the License, or (at your option) any later version.
#
# GROMACS is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with GROMACS; if not, see
# http://www.gnu.org/licenses, or write to the Free Software Foundation,
# Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
#
# If you want to redistribute modifications to GROMACS, please
# consider that scientific software is very special. Version
# control is crucial - bugs must be traceable. We will be happy to
# consider code for inclusion in the official distribution, but
# derived work must not be called official GROMACS. Details are found
# in the README & COPYING files - if they are missing, get the
# official version at http://www.gromacs.org.
#
# To help us fund GROMACS development, we humbly ask that you cite
# the research papers on the package. Check out http://www.gromacs.org.

add_library(utility INTERFACE)

file(GLOB UTILITY_SOURCES *.cpp)
if (GMX_GPU_CUDA)
    gmx_add_libgromacs_sources(cuda_version_information.cu)
endif()
if (GMX_GPU_SYCL)
    _gmx_add_files_to_property(SYCL_SOURCES
        sycl_version_information.cpp
    )
endif()
set(LIBGROMACS_SOURCES ${LIBGROMACS_SOURCES} ${UTILITY_SOURCES} PARENT_SCOPE)

if(GMX_INSTALL_LEGACY_API)
  install(FILES
          baseversion.h
          classhelpers.h
          enumerationhelpers.h
	  fileptr.h
	  futil.h
	  flags.h
	  pleasecite.h
          programcontext.h
          range.h
          smalloc.h
	  stringutil.h
          unique_cptr.h
          DESTINATION include/gromacs/utility)
endif()

# Public interface for modules, including dependencies and interfaces
#target_include_directories(utility PUBLIC
target_include_directories(utility INTERFACE
                           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
#target_link_libraries(utility PUBLIC
target_link_libraries(utility INTERFACE
                      legacy_api
                      )

# TODO: when utility is an OBJECT target
#target_link_libraries(utility PUBLIC legacy_api)
#target_link_libraries(utility PRIVATE common)

# Source files have the following private module dependencies.
# TODO: Explicitly link specific modules.
#target_link_libraries(utility PRIVATE legacy_modules)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()
