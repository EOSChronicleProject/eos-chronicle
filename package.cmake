set(CPACK_GENERATOR "TGZ")
find_program(DPKG_FOUND "dpkg")
find_program(RPMBUILD_FOUND "rpmbuild")
if(DPKG_FOUND)
   list(APPEND CPACK_GENERATOR "DEB")
endif()
if(RPMBUILD_FOUND)
   list(APPEND CPACK_GENERATOR "RPM")
endif()

set(CPACK_PACKAGE_FILE_NAME "${CMAKE_PROJECT_NAME}-${CMAKE_PROJECT_VERSION}-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")

if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.9" AND EXISTS /etc/os-release)
   #if we're doing the build on Ubuntu or RHELish, add the platform version in to the package name
   file(READ /etc/os-release OS_RELEASE LIMIT 4096)
   if(OS_RELEASE MATCHES "\n?ID=\"?ubuntu" AND OS_RELEASE MATCHES "\n?VERSION_ID=\"?([0-9.]+)")
      string(APPEND CPACK_PACKAGE_FILE_NAME "-ubuntu${CMAKE_MATCH_1}")
   elseif(OS_RELEASE MATCHES "\n?ID=\"?rhel" AND OS_RELEASE MATCHES "\n?VERSION_ID=\"?([0-9]+)")
           string(APPEND CPACK_PACKAGE_FILE_NAME "-el${CMAKE_MATCH_1}")
   elseif(OS_RELEASE MATCHES "\n?ID_LIKE=\"?([a-zA-Z0-9 ]*)" AND CMAKE_MATCH_1 MATCHES "rhel" AND OS_RELEASE MATCHES "\n?VERSION_ID=\"?([0-9]+)")
      string(APPEND CPACK_PACKAGE_FILE_NAME "-el${CMAKE_MATCH_1}")
   endif()
endif()
string(APPEND CPACK_PACKAGE_FILE_NAME "-${CMAKE_SYSTEM_PROCESSOR}")


set(CPACK_PACKAGE_CONTACT "cc32d9@gmail.com")
set(CPACK_PACKAGE_VENDOR "cc32d9")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "EOSIO State History decoder")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/EOSChronicleProject/eos-chronicle")

set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_BASE_PACKAGE_SECTION "utils")

set(CPACK_STRIP_FILES ON)

#turn some knobs to try and make package paths cooperate with GNUInstallDirs a little better
set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE OFF)
