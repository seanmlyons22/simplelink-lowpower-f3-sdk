#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "ThirdPartyFatFs::fatfs_m0p" for configuration "Release"
set_property(TARGET ThirdPartyFatFs::fatfs_m0p APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(ThirdPartyFatFs::fatfs_m0p PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/source/third_party/fatfs/lib/gcc/m0p/fatfs.a"
  )

list(APPEND _cmake_import_check_targets ThirdPartyFatFs::fatfs_m0p )
list(APPEND _cmake_import_check_files_for_ThirdPartyFatFs::fatfs_m0p "${_IMPORT_PREFIX}/source/third_party/fatfs/lib/gcc/m0p/fatfs.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
