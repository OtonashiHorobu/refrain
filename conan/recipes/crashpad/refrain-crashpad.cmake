# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Conan CMakeDeps build module for the in-repo crashpad recipe.
#
# Upstream installs only libraries; the `crashpad_handler` executable is added
# by the recipe into `<package>/bin`.  CMakeDeps has no first-class support for
# executable components, so expose the handler as an imported executable that
# consumers can resolve with `$<TARGET_FILE:crashpad::crashpad_handler>`.
#
# The package folder is derived from this module's location:
#   <package>/lib/cmake/refrain-crashpad.cmake  ->  <package>

if(NOT TARGET crashpad::crashpad_handler)
    get_filename_component(_refrain_crashpad_pkg "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    add_executable(crashpad::crashpad_handler IMPORTED)
    if(WIN32)
        set(_refrain_crashpad_handler "${_refrain_crashpad_pkg}/bin/crashpad_handler.exe")
    else()
        set(_refrain_crashpad_handler "${_refrain_crashpad_pkg}/bin/crashpad_handler")
    endif()
    set_target_properties(crashpad::crashpad_handler PROPERTIES
        IMPORTED_LOCATION "${_refrain_crashpad_handler}")
    set(CRASHPAD_HANDLER_EXECUTABLE "${_refrain_crashpad_handler}" CACHE INTERNAL
        "crashpad_handler executable provided by the Refrain crashpad Conan recipe")
    unset(_refrain_crashpad_handler)
    unset(_refrain_crashpad_pkg)
endif()
