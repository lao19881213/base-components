// This is an automatically generated file. Please DO NOT edit!
/// @file
///
/// Package config file. ONLY include in ".cc" files never in header files!

// std include
#include <string>

#ifndef ASKAP_COMPONENTS_H
#define ASKAP_COMPONENTS_H

  /// The name of the package
#define ASKAP_PACKAGE_NAME "components"

/// askap namespace
namespace askap {
  /// @return version of the package
  std::string getAskapPackageVersion_components();
}

  /// The version of the package
#define ASKAP_PACKAGE_VERSION askap::getAskapPackageVersion_components()

#endif
