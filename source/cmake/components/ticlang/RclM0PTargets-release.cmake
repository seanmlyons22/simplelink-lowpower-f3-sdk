#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "Rcl::rcl_cc23x0r5" for configuration "Release"
set_property(TARGET Rcl::rcl_cc23x0r5 APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(Rcl::rcl_cc23x0r5 PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/source/ti/drivers/rcl/lib/ticlang/m0p/rcl_cc23x0r5.a"
  )

list(APPEND _cmake_import_check_targets Rcl::rcl_cc23x0r5 )
list(APPEND _cmake_import_check_files_for_Rcl::rcl_cc23x0r5 "${_IMPORT_PREFIX}/source/ti/drivers/rcl/lib/ticlang/m0p/rcl_cc23x0r5.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)