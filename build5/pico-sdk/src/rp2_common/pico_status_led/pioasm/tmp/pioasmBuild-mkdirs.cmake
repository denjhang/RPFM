# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/d/working/vscode-projects/YM2163-Midi/RPFM/pico-sdk-2.2.0/tools/pioasm")
  file(MAKE_DIRECTORY "/d/working/vscode-projects/YM2163-Midi/RPFM/pico-sdk-2.2.0/tools/pioasm")
endif()
file(MAKE_DIRECTORY
  "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pioasm"
  "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pioasm-install"
  "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pico-sdk/src/rp2_common/pico_status_led/pioasm/tmp"
  "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
  "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pico-sdk/src/rp2_common/pico_status_led/pioasm/src"
  "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/d/working/vscode-projects/YM2163-Midi/RPFM/build5/pico-sdk/src/rp2_common/pico_status_led/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
