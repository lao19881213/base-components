cmake_minimum_required (VERSION 3.1.0)
include(CheckCXXCompilerFlag)
project(base-components VERSION 0.0.1 LANGUAGES C CXX)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/askap-cmake)
    execute_process(COMMAND git pull 
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/askap-cmake)
else ()	
    execute_process(COMMAND git clone https://bitbucket.csiro.au/scm/askapsdp/askap-cmake.git
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif ()
set (CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/askap-cmake")

option (CXX11 "Compile as C++11 if possible" YES)
option (ENABLE_SHARED "Build shared libraries" YES)
option (ENABLE_RPATH "Include rpath in executables and shared libraries" YES)

if (CXX11)
    check_cxx_compiler_flag(-std=c++11 HAS_CXX11)
    if (HAS_CXX11)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
      set(CXX11 ON)
    else()
	    message(STATUS "CASA_BUILD requires a c++11 compatible compiler")
      option(CXX11 "" NO)
    endif()
endif()

# uninstall target
if(NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        IMMEDIATE @ONLY)

    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()



if (ENABLE_SHARED)
option (BUILD_SHARED_LIBS "" YES)
    if (ENABLE_RPATH)
        # Set RPATH to use for installed targets; append linker search path
        set(CMAKE_INSTALL_NAME_DIR "${CMAKE_INSTALL_PREFIX}/lib" )
        set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
        set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
    endif (ENABLE_RPATH)
endif(ENABLE_SHARED)


# Yanda Packages
# YandaSoft dependencies .. in order
include(install_yanda_depends)
set (EXTERNAL_INSTALL_DIR ${CMAKE_INSTALL_PREFIX})

find_package(Askap)

if (NOT ASKAP_FOUND)
	install_yanda_depends(base-askap)
	find_package(Askap REQUIRED)
endif ()


# find packages
find_package(log4cxx REQUIRED)
find_package(Casacore REQUIRED COMPONENTS  ms images mirlib coordinates fits lattices measures scimath scimath_f tables casa)
find_package(Components)
find_package(CPPUnit)

# include directories
include_directories(${log4cxx_INCLUDE_DIRS})
add_definitions(-DHAVE_LOG4CXX)
include_directories(${CASACORE_INCLUDE_DIRS})
add_definitions(-DHAVE_AIPSPP)
include_directories(${ASKAP_INCLUDE_DIRS})
include_directories(${COMPONENTS_INCLUDE_DIRS})

if (CPPUNIT_FOUND)
    include_directories(${CPPUNIT_INCLUDE_DIR})
endif ()

add_library(askap_components
askap/components/AskapComponentImager.cc
askap/components/ConstantSpectrum.cc
askap/components/SpectralIndex.cc
askap/components/SpectralModel.cc
)


install (FILES

askap/components/AskapComponentImager.h
askap/components/ComponentType.h
askap/components/ConstantSpectrum.h
askap/components/SpectralIndex.h
askap/components/SpectralModel.h
	
DESTINATION include/askap/components

)




target_link_libraries(
	askap_components
	${CASACORE_LIBRARIES}
	${ASKAP_LIBRARY}
	${log4cxx_LIBRARY}
	${COMPONENTS_LIBRARY}
#	${Boost_LIBRARIES}
#	${LofarCommon_LIBRARY}
)

if (CPPUNT_FOUND) 
    target_link_libraries(askap_components 
        ${CPPUNIT_LIBRARY}
    )
endif ()


install (
TARGETS askap_components
RUNTIME DESTINATION bin
LIBRARY DESTINATION lib
ARCHIVE DESTINATION lib
LIBRARY PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)
if (CPPUNIT_FOUND)
  include(CTest)
  enable_testing()

  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/tests/components)
  include_directories(${CMAKE_CURRENT_SOURCE_DIR})
  add_subdirectory(tests/components)

endif (CPPUNIT_FOUND)



