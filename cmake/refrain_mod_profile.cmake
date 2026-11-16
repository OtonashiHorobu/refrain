# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu

# Build-time mod profile resolution.
#
# A profile (`profiles/<name>.toml`) is the single source of truth for the set
# of first-party mods compiled into the binary. The same resolved list drives
# three products:
#   1. the static mod sources compiled into `refrain-first-party-mods`,
#   2. the mod manifests/data packed into the built-in content image,
#   3. the profile name/build id embedded in the version information and the
#      default gamepack used by the build and by the release archive.
#
# The parser below reads the small TOML subset the profile files use (string
# scalars and arrays of strings, one key per line or a multi-line array). It is
# deliberately strict: an unknown key, a malformed value or an inconsistent
# gamepack fails the configure step instead of producing a binary whose three
# lists disagree.
#
# Values written by this module (parent scope):
#   REFRAIN_MOD_PROFILE_NAME        profile name (also embedded in the binary)
#   REFRAIN_MOD_PROFILE_DESCRIPTION one-line description
#   REFRAIN_MOD_GAMEPACK_RESOLVED   repo-relative default gamepack path
#   REFRAIN_RESOLVED_MODS           sorted mod ids, static + dynamic
#   REFRAIN_RESOLVED_DYNAMIC        sorted dynamic mod ids (subset of mods)
#   REFRAIN_RESOLVED_STATIC         sorted static mod ids
#   REFRAIN_MOD_BUILD_ID            16 hex chars over the resolved list
#   REFRAIN_MOD_LINK_MODE           static | mixed | dynamic
#   REFRAIN_MOD_PROFILE_FILE        the profile file that was read
#   REFRAIN_MOD_HAS_DYNAMIC         ON when at least one dynamic module is built

include_guard(GLOBAL)

# --- small TOML subset reader ---------------------------------------------

function(refrain_profile_strip_comments text out)
    # The profile files keep comments on their own lines. Removing them with a
    # single regex (instead of splitting the file into a CMake list) keeps the
    # text byte-for-byte, including arrays whose lines contain brackets.
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" result "${text}")
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

function(refrain_profile_string text key out)
    string(REGEX MATCH "(^|\n)[ \t]*${key}[ \t]*=[ \t]*\"([^\"]*)\"" match
           "${text}")
    if(NOT match)
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    set(${out} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(refrain_profile_array text key found out)
    string(REGEX MATCH "(^|\n)[ \t]*${key}[ \t]*=[ \t]*\\[[^]]*\\]" match
           "${text}")
    if(NOT match)
        set(${found} FALSE PARENT_SCOPE)
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    string(REGEX MATCHALL "\"[^\"]*\"" quoted "${match}")
    set(values "")
    foreach(item IN LISTS quoted)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${item}")
        list(APPEND values "${value}")
    endforeach()
    set(${found} TRUE PARENT_SCOPE)
    set(${out} "${values}" PARENT_SCOPE)
endfunction()

function(refrain_profile_fail path message)
    message(FATAL_ERROR "[Refrain] mod profile '${path}': ${message}")
endfunction()

# Read and validate one profile file. Returns the raw declared lists; the
# caller applies the cache overrides and validates the dependency closure.
function(refrain_read_mod_profile path out_prefix)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
                "[Refrain] mod profile '${path}' does not exist; profiles live "
                "in profiles/<name>.toml (use -DREFRAIN_MOD_PROFILE=<name>)")
    endif()
    file(READ "${path}" raw_text)
    refrain_profile_strip_comments("${raw_text}" text)

    # Reject unknown root keys. Only the profile schema is accepted so a
    # misspelled `dynamic`/`gamepack` key cannot silently change the build.
    string(REGEX MATCHALL "(^|\n)[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*="
           assignments "${text}")
    set(known_keys name description gamepack mods dynamic build_id link)
    foreach(assignment IN LISTS assignments)
        string(REGEX MATCH "[A-Za-z_][A-Za-z0-9_]*" key "${assignment}")
        list(FIND known_keys "${key}" key_index)
        if(key_index EQUAL -1)
            refrain_profile_fail(
                "${path}" "unknown key '${key}' (known keys: name, "
                "description, gamepack, mods, dynamic)")
        endif()
    endforeach()

    refrain_profile_string("${text}" name name)
    refrain_profile_string("${text}" description description)
    refrain_profile_string("${text}" gamepack gamepack)
    refrain_profile_array("${text}" mods mods_found mods)
    refrain_profile_array("${text}" dynamic dynamic_found dynamic)
    foreach(array_key IN ITEMS mods dynamic)
        if(array_key STREQUAL "mods")
            set(array_found "${mods_found}")
        else()
            set(array_found "${dynamic_found}")
        endif()
        string(REGEX MATCH "(^|\n)[ \t]*${array_key}[ \t]*=" array_key_line
               "${text}")
        if(array_key_line AND NOT array_found)
            refrain_profile_fail(
                "${path}" "'${array_key}' must be an array of mod id strings")
        endif()
    endforeach()

    if(name STREQUAL "")
        refrain_profile_fail("${path}" "missing 'name'")
    endif()
    if(gamepack STREQUAL "")
        refrain_profile_fail("${path}" "missing 'gamepack'")
    endif()
    if(NOT mods_found OR mods STREQUAL "")
        refrain_profile_fail("${path}" "missing or empty 'mods' array")
    endif()
    if(NOT dynamic_found)
        set(dynamic "")
    endif()

    # `name` must match the file stem so `--profile <name>` and the embedded
    # build information cannot disagree.
    get_filename_component(file_name "${path}" NAME)
    string(REGEX REPLACE "\\.toml$" "" stem "${file_name}")
    if(NOT name STREQUAL stem)
        refrain_profile_fail(
            "${path}" "name '${name}' does not match the file name '${stem}'")
    endif()

    if(NOT name MATCHES "^[a-z0-9][a-z0-9._-]*$")
        refrain_profile_fail(
            "${path}" "invalid profile name '${name}' (lowercase letters, "
            "digits, '.', '_' and '-')")
    endif()

    set(${out_prefix}_NAME "${name}" PARENT_SCOPE)
    set(${out_prefix}_DESCRIPTION "${description}" PARENT_SCOPE)
    set(${out_prefix}_GAMEPACK "${gamepack}" PARENT_SCOPE)
    set(${out_prefix}_MODS "${mods}" PARENT_SCOPE)
    set(${out_prefix}_DYNAMIC "${dynamic}" PARENT_SCOPE)
endfunction()

# --- validation -----------------------------------------------------------

function(refrain_validate_mod_id path id)
    if(NOT id MATCHES "^[a-z0-9][a-z0-9._-]*$")
        refrain_profile_fail("${path}" "invalid mod id '${id}'")
    endif()
    set(mod_dir "${PROJECT_SOURCE_DIR}/mods/${id}")
    if(NOT IS_DIRECTORY "${mod_dir}")
        refrain_profile_fail(
            "${path}"
            "mod '${id}' has no directory under mods/; add the mod or remove "
            "it from the profile")
    endif()
    if(NOT EXISTS "${mod_dir}/manifest.toml")
        refrain_profile_fail("${path}"
                             "mod '${id}' has no manifest.toml")
    endif()
endfunction()

# Every mod listed by `mods` must be present in the gamepack's `[mods].list`,
# and every pin must be built: the default gamepack and the profile are two
# halves of one contract, and the build refuses to produce a binary that
# cannot start its own default composition.
function(refrain_validate_default_gamepack profile_path gamepack_path
         mods_list)
    if(NOT EXISTS "${gamepack_path}")
        refrain_profile_fail(
            "${profile_path}"
            "default gamepack '${gamepack_path}' does not exist (relative to "
            "the source root)")
    endif()
    file(READ "${gamepack_path}" raw_text)
    refrain_profile_strip_comments("${raw_text}" text)
    # Isolate the [mods] table, not the per-mod [mods."<id>"] tables.
    string(FIND "${text}" "\n[mods]\n" mods_pos)
    if(mods_pos EQUAL -1)
        refrain_profile_fail("${profile_path}"
                             "default gamepack has no [mods] table")
    endif()
    string(SUBSTRING "${text}" ${mods_pos} -1 mods_table)
    # The [mods] table's own `list` array is the pin set. Per-mod settings
    # tables follow it and never declare `list`.
    string(REGEX MATCH "list[ \t]*=[ \t]*\\[[^]]*\\]" list_block
           "${mods_table}")
    if(NOT list_block)
        refrain_profile_fail("${profile_path}"
                             "default gamepack has no [mods].list array")
    endif()
    string(REGEX MATCHALL "\"([^\"@]+)@" pins "${list_block}")
    set(pinned "")
    foreach(pin IN LISTS pins)
        string(REGEX REPLACE "^\"(.*)@$" "\\1" id "${pin}")
        list(APPEND pinned "${id}")
    endforeach()
    foreach(id IN LISTS pinned)
        list(FIND mods_list "${id}" index)
        if(index EQUAL -1)
            refrain_profile_fail(
                "${profile_path}"
                "default gamepack pins '${id}' but the profile does not build "
                "it; add the mod to the profile or point the profile at a "
                "matching gamepack")
        endif()
    endforeach()
    foreach(id IN LISTS mods_list)
        list(FIND pinned "${id}" index)
        if(index EQUAL -1)
            refrain_profile_fail(
                "${profile_path}"
                "mod '${id}' is built but the default gamepack does not pin "
                "it; the same list must drive code, image and gamepack")
        endif()
    endforeach()
endfunction()

# --- build identity -------------------------------------------------------

function(refrain_compute_mod_build_id name link gamepack mods_list
         dynamic_list out)
    set(text "refrain.mod-profile.v1\n")
    string(APPEND text "name=${name}\n")
    string(APPEND text "link=${link}\n")
    foreach(id IN LISTS mods_list)
        string(APPEND text "mod=${id}\n")
    endforeach()
    foreach(id IN LISTS dynamic_list)
        string(APPEND text "dynamic=${id}\n")
    endforeach()
    string(APPEND text "gamepack=${gamepack}\n")
    string(SHA256 digest "${text}")
    string(SUBSTRING "${digest}" 0 16 build_id)
    set(${out} "${build_id}" PARENT_SCOPE)
endfunction()

# --- entry point ----------------------------------------------------------

# Resolve the profile selected by -DREFRAIN_MOD_PROFILE and the explicit
# overrides, validate it and publish REFRAIN_RESOLVED_* in the parent scope.
function(refrain_resolve_mod_profile)
    set(profiles_dir "${PROJECT_SOURCE_DIR}/profiles")
    if(NOT REFRAIN_MOD_PROFILES_DIR STREQUAL "")
        set(profiles_dir "${REFRAIN_MOD_PROFILES_DIR}")
    endif()
    set(profile_name "${REFRAIN_MOD_PROFILE}")
    set(profile_path "${profiles_dir}/${profile_name}.toml")

    refrain_read_mod_profile("${profile_path}" declared)

    set(mods "${declared_MODS}")
    if(NOT REFRAIN_MODS STREQUAL "")
        string(REPLACE "," ";" explicit_mods "${REFRAIN_MODS}")
        set(mods "")
        foreach(id IN LISTS explicit_mods)
            string(STRIP "${id}" id)
            if(NOT id STREQUAL "")
                list(APPEND mods "${id}")
            endif()
        endforeach()
        message(STATUS
                "[Refrain] REFRAIN_MODS overrides the profile's mod list "
                "(${profile_name})")
    endif()

    set(dynamic "${declared_DYNAMIC}")
    if(NOT REFRAIN_MOD_DYNAMIC STREQUAL "")
        string(REPLACE "," ";" explicit_dynamic "${REFRAIN_MOD_DYNAMIC}")
        set(dynamic "")
        foreach(id IN LISTS explicit_dynamic)
            string(STRIP "${id}" id)
            if(NOT id STREQUAL "")
                list(APPEND dynamic "${id}")
            endif()
        endforeach()
        message(STATUS
                "[Refrain] REFRAIN_MOD_DYNAMIC overrides the profile's dynamic "
                "list (${profile_name})")
    endif()

    set(gamepack "${declared_GAMEPACK}")
    if(NOT REFRAIN_MOD_GAMEPACK STREQUAL "")
        set(gamepack "${REFRAIN_MOD_GAMEPACK}")
    endif()

    list(REMOVE_DUPLICATES mods)
    list(REMOVE_DUPLICATES dynamic)
    list(SORT mods)
    list(SORT dynamic)

    foreach(id IN LISTS mods)
        refrain_validate_mod_id("${profile_path}" "${id}")
    endforeach()
    foreach(id IN LISTS dynamic)
        list(FIND mods "${id}" index)
        if(index EQUAL -1)
            refrain_profile_fail(
                "${profile_path}"
                "dynamic mod '${id}' is not in the profile's mods list")
        endif()
        file(GLOB sources CONFIGURE_DEPENDS
             "${PROJECT_SOURCE_DIR}/mods/${id}/src/*.cpp")
        if(NOT sources)
            refrain_profile_fail(
                "${profile_path}"
                "dynamic mod '${id}' has no mods/${id}/src/*.cpp; only a mod "
                "with code can be built as a shared module")
        endif()
    endforeach()

    # Resolve the link mode. `auto` keeps the historical behaviour for
    # profiles without a dynamic list: everything is linked statically.
    set(link_mode "${REFRAIN_MOD_LINK}")
    if(link_mode STREQUAL "auto")
        if("${dynamic}" STREQUAL "")
            set(link_mode "static")
        else()
            set(link_mode "mixed")
        endif()
    endif()
    if(NOT link_mode MATCHES "^(static|mixed|dynamic)$")
        message(FATAL_ERROR
                "[Refrain] REFRAIN_MOD_LINK must be auto, static, mixed or "
                "dynamic (got '${REFRAIN_MOD_LINK}')")
    endif()
    if(link_mode STREQUAL "static" AND NOT "${dynamic}" STREQUAL "")
        message(STATUS
                "[Refrain] REFRAIN_MOD_LINK=static builds the profile's "
                "dynamic mods as static code")
        set(dynamic "")
    elseif(link_mode STREQUAL "dynamic")
        # Every mod with code would become a module, which needs a shared
        # runtime library so that module and host share one set of process
        # globals (logger, allocator, service registry). That split is not done
        # yet, so a mode that silently produced unloadable modules is refused;
        # an explicit per-profile `dynamic` list (mixed mode) is supported.
        message(FATAL_ERROR
                "[Refrain] REFRAIN_MOD_LINK=dynamic (every mod as a shared "
                "module) needs the shared runtime library, which does not "
                "exist yet. Select the modules explicitly with a profile's "
                "`dynamic` list (mixed mode).")
    endif()
    if(NOT "${dynamic}" STREQUAL "" AND NOT (UNIX AND NOT APPLE))
        message(FATAL_ERROR
                "[Refrain] dynamic first-party modules need an ELF target; "
                "profile '${profile_name}' selects ${dynamic} as dynamic. Use "
                "-DREFRAIN_MOD_LINK=static for this platform.")
    endif()
    if(NOT "${dynamic}" STREQUAL "" AND REFRAIN_HARDEN)
        # Hardening compiles every engine target with -fvisibility=hidden, so
        # the host executable would not export the runtime symbols the modules
        # resolve at load time. Fail at configure time instead of producing an
        # archive whose modules cannot open.
        message(FATAL_ERROR
                "[Refrain] dynamic first-party modules and REFRAIN_HARDEN are "
                "incompatible: hidden symbol visibility removes the host "
                "runtime exports the modules need. Build the dynamic profile "
                "without -DREFRAIN_HARDEN=ON.")
    endif()

    set(static_mods ${mods})
    foreach(id IN LISTS dynamic)
        list(REMOVE_ITEM static_mods "${id}")
    endforeach()

    if(IS_ABSOLUTE "${gamepack}")
        set(gamepack_file "${gamepack}")
    else()
        set(gamepack_file "${PROJECT_SOURCE_DIR}/${gamepack}")
    endif()
    refrain_validate_default_gamepack("${profile_path}" "${gamepack_file}"
                                      "${mods}")
    refrain_compute_mod_build_id("${profile_name}" "${link_mode}" "${gamepack}"
                                 "${mods}" "${dynamic}" build_id)

    set(REFRAIN_MOD_PROFILE_NAME "${profile_name}" PARENT_SCOPE)
    set(REFRAIN_MOD_PROFILE_DESCRIPTION "${declared_DESCRIPTION}" PARENT_SCOPE)
    set(REFRAIN_MOD_GAMEPACK_RESOLVED "${gamepack}" PARENT_SCOPE)
    set(REFRAIN_RESOLVED_MODS "${mods}" PARENT_SCOPE)
    set(REFRAIN_RESOLVED_DYNAMIC "${dynamic}" PARENT_SCOPE)
    set(REFRAIN_RESOLVED_STATIC "${static_mods}" PARENT_SCOPE)
    set(REFRAIN_MOD_BUILD_ID "${build_id}" PARENT_SCOPE)
    set(REFRAIN_MOD_LINK_MODE "${link_mode}" PARENT_SCOPE)
    set(REFRAIN_MOD_PROFILE_FILE "${profile_path}" PARENT_SCOPE)
    if("${dynamic}" STREQUAL "")
        set(REFRAIN_MOD_HAS_DYNAMIC OFF PARENT_SCOPE)
    else()
        set(REFRAIN_MOD_HAS_DYNAMIC ON PARENT_SCOPE)
    endif()
endfunction()
