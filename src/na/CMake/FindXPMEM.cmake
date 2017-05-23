# - Try to find XPMEM
# Once done this will define
#  XPMEM_FOUND - System has XPMEM
#  XPMEM_INCLUDE_DIRS - The XPMEM include directories
#  XPMEM_LIBRARIES - The libraries needed to use XPMEM

find_package(PkgConfig)
pkg_check_modules(PC_XPMEM QUIET cray-xpmem)

find_path(XPMEM_INCLUDE_DIR xpmem.h
  HINTS ${PC_XPMEM_INCLUDEDIR} ${PC_XPMEM_INCLUDE_DIRS})

find_library(XPMEM_LIBRARY NAMES xpmem
  HINTS ${PC_XPMEM_LIBDIR} ${PC_XPMEM_LIBRARY_DIRS})

set(XPMEM_LIBRARIES ${XPMEM_LIBRARY})
set(XPMEM_INCLUDE_DIRS ${XPMEM_INCLUDE_DIR})

include(FindPackageHandleStandardArgs)
# handle the QUIETLY and REQUIRED arguments and set XPMEM_FOUND to TRUE
# if all listed variables are TRUE
find_package_handle_standard_args(XPMEM DEFAULT_MSG
                                  XPMEM_LIBRARY XPMEM_INCLUDE_DIR)

mark_as_advanced(XPMEM_INCLUDE_DIR XPMEM_LIBRARY)
