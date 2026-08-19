cmake_minimum_required(VERSION 3.28)

# Copies or verifies the CUDA runtime closure required by Roundtable. This is
# deliberately a script-mode helper: file(GET_RUNTIME_DEPENDENCIES) can inspect
# the Toolkit DLLs (and, after link, roundtable.exe) without hard-coding CUDA's
# versioned DLL suffixes.

foreach(_required_var IN ITEMS
        ROUNDTABLE_CUDA_SEED_MANIFEST
        ROUNDTABLE_CUDA_DESTINATION)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "DeployCudaRuntime: ${_required_var} is required")
    endif()
endforeach()

if(NOT EXISTS "${ROUNDTABLE_CUDA_SEED_MANIFEST}")
    message(FATAL_ERROR
        "DeployCudaRuntime: seed manifest not found: ${ROUNDTABLE_CUDA_SEED_MANIFEST}")
endif()
include("${ROUNDTABLE_CUDA_SEED_MANIFEST}")

foreach(_required_var IN ITEMS
        ROUNDTABLE_CUDA_SOURCE_DIR
        ROUNDTABLE_CUDA_RUNTIME_SEEDS)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR
            "DeployCudaRuntime: generated manifest is missing ${_required_var}")
    endif()
endforeach()

if(NOT DEFINED ROUNDTABLE_CUDA_MODE)
    set(ROUNDTABLE_CUDA_MODE DEPLOY)
endif()
string(TOUPPER "${ROUNDTABLE_CUDA_MODE}" ROUNDTABLE_CUDA_MODE)
if(NOT ROUNDTABLE_CUDA_MODE STREQUAL "DEPLOY" AND
   NOT ROUNDTABLE_CUDA_MODE STREQUAL "VERIFY")
    message(FATAL_ERROR
        "DeployCudaRuntime: ROUNDTABLE_CUDA_MODE must be DEPLOY or VERIFY")
endif()

file(TO_CMAKE_PATH "${ROUNDTABLE_CUDA_SOURCE_DIR}" _cuda_source_dir)
file(TO_CMAKE_PATH "${ROUNDTABLE_CUDA_DESTINATION}" _cuda_destination)
if(NOT IS_DIRECTORY "${_cuda_source_dir}")
    message(FATAL_ERROR
        "DeployCudaRuntime: CUDA runtime directory not found: ${_cuda_source_dir}")
endif()

if(ROUNDTABLE_CUDA_MODE STREQUAL "DEPLOY")
    file(MAKE_DIRECTORY "${_cuda_destination}")
elseif(NOT IS_DIRECTORY "${_cuda_destination}")
    message(FATAL_ERROR
        "DeployCudaRuntime: package directory not found: ${_cuda_destination}")
endif()

file(REAL_PATH "${_cuda_source_dir}" _cuda_source_real)
file(REAL_PATH "${_cuda_destination}" _cuda_destination_real)
file(TO_CMAKE_PATH "${_cuda_source_real}" _cuda_source_real)
file(TO_CMAKE_PATH "${_cuda_destination_real}" _cuda_destination_real)

function(_roundtable_path_is_within child parent out_var)
    file(TO_CMAKE_PATH "${child}" _child)
    file(TO_CMAKE_PATH "${parent}" _parent)
    string(TOLOWER "${_child}" _child)
    string(TOLOWER "${_parent}" _parent)
    string(REGEX REPLACE "/+$" "" _child "${_child}")
    string(REGEX REPLACE "/+$" "" _parent "${_parent}")
    string(FIND "${_child}/" "${_parent}/" _prefix_at)
    if(_prefix_at EQUAL 0)
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_roundtable_is_cuda_runtime_name name out_var)
    string(TOLOWER "${name}" _name)
    if(_name MATCHES
       "^(cudart|cublas|cufft|curand|cusolver|cusparse|nvjitlink|nvrtc|nvfatbin|nvjpeg|npp|cupti)[a-z0-9_.-]*[.]dll$")
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(_roundtable_fail_on_unresolved_cuda context)
    foreach(_dependency IN LISTS ARGN)
        get_filename_component(_name "${_dependency}" NAME)
        string(TOLOWER "${_name}" _name_lower)
        _roundtable_is_cuda_runtime_name("${_name}" _is_cuda_name)
        # nvcuda.dll is the NVIDIA display-driver API. It is intentionally not
        # redistributable and must never be copied from the Toolkit package.
        if(_is_cuda_name AND NOT _name_lower STREQUAL "nvcuda.dll")
            message(FATAL_ERROR
                "DeployCudaRuntime: unresolved CUDA dependency while inspecting ${context}: ${_name}")
        endif()
    endforeach()
endfunction()

# Start with the two direct link-time imports (cudart and cuBLAS), then walk
# each Toolkit DLL independently. Filtering resolved paths back to the Toolkit
# bin directory excludes Windows system DLLs while retaining transitive CUDA
# dependencies such as cublasLt.
set(_cuda_runtime_paths "${ROUNDTABLE_CUDA_RUNTIME_SEEDS}")
set(_cuda_pending "${ROUNDTABLE_CUDA_RUNTIME_SEEDS}")
set(_cuda_visited "")

while(_cuda_pending)
    list(POP_FRONT _cuda_pending _current_runtime)
    if(_current_runtime IN_LIST _cuda_visited)
        continue()
    endif()
    if(NOT EXISTS "${_current_runtime}")
        message(FATAL_ERROR
            "DeployCudaRuntime: required CUDA runtime DLL not found: ${_current_runtime}")
    endif()
    list(APPEND _cuda_visited "${_current_runtime}")

    file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES "${_current_runtime}"
        DIRECTORIES "${_cuda_source_real}"
        RESOLVED_DEPENDENCIES_VAR _runtime_resolved
        UNRESOLVED_DEPENDENCIES_VAR _runtime_unresolved
        CONFLICTING_DEPENDENCIES_PREFIX _runtime_conflicts)
    _roundtable_fail_on_unresolved_cuda(
        "${_current_runtime}" ${_runtime_unresolved})

    foreach(_resolved IN LISTS _runtime_resolved)
        file(REAL_PATH "${_resolved}" _resolved_real)
        _roundtable_path_is_within(
            "${_resolved_real}" "${_cuda_source_real}" _from_cuda_toolkit)
        if(_from_cuda_toolkit AND NOT _resolved_real IN_LIST _cuda_runtime_paths)
            list(APPEND _cuda_runtime_paths "${_resolved_real}")
            list(APPEND _cuda_pending "${_resolved_real}")
        endif()
    endforeach()
endwhile()

# Once the executable exists, inspect it too. This closes the package over any
# future CUDA DLL imported directly by Roundtable or a newly linked backend,
# rather than silently relying on the build machine's PATH.
if(DEFINED ROUNDTABLE_CUDA_EXECUTABLE AND
   NOT "${ROUNDTABLE_CUDA_EXECUTABLE}" STREQUAL "")
    if(NOT EXISTS "${ROUNDTABLE_CUDA_EXECUTABLE}")
        message(FATAL_ERROR
            "DeployCudaRuntime: executable not found: ${ROUNDTABLE_CUDA_EXECUTABLE}")
    endif()

    file(GET_RUNTIME_DEPENDENCIES
        EXECUTABLES "${ROUNDTABLE_CUDA_EXECUTABLE}"
        DIRECTORIES "${_cuda_destination_real}" "${_cuda_source_real}"
        RESOLVED_DEPENDENCIES_VAR _exe_resolved
        UNRESOLVED_DEPENDENCIES_VAR _exe_unresolved
        CONFLICTING_DEPENDENCIES_PREFIX _exe_conflicts)
    _roundtable_fail_on_unresolved_cuda(
        "${ROUNDTABLE_CUDA_EXECUTABLE}" ${_exe_unresolved})

    foreach(_resolved IN LISTS _exe_resolved)
        get_filename_component(_resolved_name "${_resolved}" NAME)
        file(REAL_PATH "${_resolved}" _resolved_real)
        _roundtable_path_is_within(
            "${_resolved_real}" "${_cuda_source_real}" _from_cuda_toolkit)

        # Expected CUDA DLLs normally resolve beside roundtable.exe after the
        # pre-link copy target. Map them back to the Toolkit copy for hashing
        # and install reuse. A newly imported CUDA DLL resolves from the
        # Toolkit directory and follows the first branch.
        set(_toolkit_peer "${_cuda_source_real}/${_resolved_name}")
        if(_from_cuda_toolkit)
            set(_cuda_candidate "${_resolved_real}")
        elseif(EXISTS "${_toolkit_peer}")
            _roundtable_is_cuda_runtime_name("${_resolved_name}" _is_cuda_name)
            if(_is_cuda_name)
                set(_cuda_candidate "${_toolkit_peer}")
            else()
                unset(_cuda_candidate)
            endif()
        else()
            unset(_cuda_candidate)
        endif()

        if(DEFINED _cuda_candidate AND
           NOT _cuda_candidate IN_LIST _cuda_runtime_paths)
            list(APPEND _cuda_runtime_paths "${_cuda_candidate}")
            list(APPEND _cuda_pending "${_cuda_candidate}")
        endif()
        unset(_cuda_candidate)
    endforeach()
endif()

# Expand any additional direct CUDA imports discovered on roundtable.exe.
while(_cuda_pending)
    list(POP_FRONT _cuda_pending _current_runtime)
    if(_current_runtime IN_LIST _cuda_visited)
        continue()
    endif()
    if(NOT EXISTS "${_current_runtime}")
        message(FATAL_ERROR
            "DeployCudaRuntime: required CUDA runtime DLL not found: ${_current_runtime}")
    endif()
    list(APPEND _cuda_visited "${_current_runtime}")

    file(GET_RUNTIME_DEPENDENCIES
        LIBRARIES "${_current_runtime}"
        DIRECTORIES "${_cuda_source_real}"
        RESOLVED_DEPENDENCIES_VAR _runtime_resolved
        UNRESOLVED_DEPENDENCIES_VAR _runtime_unresolved
        CONFLICTING_DEPENDENCIES_PREFIX _runtime_conflicts)
    _roundtable_fail_on_unresolved_cuda(
        "${_current_runtime}" ${_runtime_unresolved})

    foreach(_resolved IN LISTS _runtime_resolved)
        file(REAL_PATH "${_resolved}" _resolved_real)
        _roundtable_path_is_within(
            "${_resolved_real}" "${_cuda_source_real}" _from_cuda_toolkit)
        if(_from_cuda_toolkit AND NOT _resolved_real IN_LIST _cuda_runtime_paths)
            list(APPEND _cuda_runtime_paths "${_resolved_real}")
            list(APPEND _cuda_pending "${_resolved_real}")
        endif()
    endforeach()
endwhile()

list(REMOVE_DUPLICATES _cuda_runtime_paths)

# Build a stable name -> source mapping and reject duplicate basenames. DLL
# lookup is filename-based, so two different files with the same name would be
# an ambiguous, non-reproducible package.
set(_cuda_runtime_names "")
foreach(_runtime IN LISTS _cuda_runtime_paths)
    get_filename_component(_name "${_runtime}" NAME)
    if(_name IN_LIST _cuda_runtime_names)
        message(FATAL_ERROR
            "DeployCudaRuntime: duplicate CUDA runtime filename in closure: ${_name}")
    endif()
    list(APPEND _cuda_runtime_names "${_name}")
    string(MAKE_C_IDENTIFIER "${_name}" _name_id)
    set(_cuda_source_${_name_id} "${_runtime}")
endforeach()
list(SORT _cuda_runtime_names)

set(_package_manifest "${_cuda_destination_real}/roundtable-cuda-runtime.txt")

if(ROUNDTABLE_CUDA_MODE STREQUAL "DEPLOY")
    # Remove only files named by the previous generated manifest. This cleans
    # an obsolete CUDA major version without touching unrelated user/build DLLs.
    if(EXISTS "${_package_manifest}")
        file(STRINGS "${_package_manifest}" _previous_names)
        foreach(_old_name IN LISTS _previous_names)
            string(STRIP "${_old_name}" _old_name)
            if(_old_name STREQUAL "" OR _old_name MATCHES "^#")
                continue()
            endif()
            get_filename_component(_old_basename "${_old_name}" NAME)
            _roundtable_is_cuda_runtime_name("${_old_basename}" _is_cuda_name)
            if(_is_cuda_name AND
               _old_basename STREQUAL _old_name AND
               NOT _old_basename IN_LIST _cuda_runtime_names)
                file(REMOVE "${_cuda_destination_real}/${_old_basename}")
            endif()
        endforeach()
    endif()

    foreach(_name IN LISTS _cuda_runtime_names)
        string(MAKE_C_IDENTIFIER "${_name}" _name_id)
        set(_source "${_cuda_source_${_name_id}}")
        set(_destination "${_cuda_destination_real}/${_name}")
        file(COPY_FILE "${_source}" "${_destination}"
            ONLY_IF_DIFFERENT RESULT _copy_result)
        if(NOT _copy_result STREQUAL "0")
            message(FATAL_ERROR
                "DeployCudaRuntime: failed to copy ${_name}: ${_copy_result}")
        endif()
        file(SIZE "${_source}" _source_size)
        file(SIZE "${_destination}" _destination_size)
        if(NOT _source_size EQUAL _destination_size)
            message(FATAL_ERROR
                "DeployCudaRuntime: copied size mismatch for ${_name}")
        endif()
    endforeach()

    set(_manifest_contents
        "# Roundtable CUDA Toolkit ${ROUNDTABLE_CUDA_TOOLKIT_VERSION} runtime closure\n")
    string(APPEND _manifest_contents
        "# nvcuda.dll is supplied by the NVIDIA display driver and is not bundled.\n")
    foreach(_name IN LISTS _cuda_runtime_names)
        string(APPEND _manifest_contents "${_name}\n")
    endforeach()
    set(_manifest_tmp "${_package_manifest}.tmp")
    file(WRITE "${_manifest_tmp}" "${_manifest_contents}")
    file(RENAME "${_manifest_tmp}" "${_package_manifest}" RESULT _rename_result)
    if(NOT _rename_result STREQUAL "0")
        message(FATAL_ERROR
            "DeployCudaRuntime: failed to publish runtime manifest: ${_rename_result}")
    endif()

    message(STATUS
        "Bundled CUDA ${ROUNDTABLE_CUDA_TOOLKIT_VERSION} runtime: ${_cuda_runtime_names}")
else()
    if(NOT EXISTS "${_package_manifest}")
        message(FATAL_ERROR
            "DeployCudaRuntime: package manifest missing: ${_package_manifest}")
    endif()
    file(STRINGS "${_package_manifest}" _packaged_names)
    list(FILTER _packaged_names EXCLUDE REGEX "^#")
    list(FILTER _packaged_names EXCLUDE REGEX "^$")
    list(SORT _packaged_names)
    if(NOT _packaged_names STREQUAL _cuda_runtime_names)
        message(FATAL_ERROR
            "DeployCudaRuntime: package manifest does not match linked CUDA closure. "
            "Expected '${_cuda_runtime_names}', found '${_packaged_names}'")
    endif()

    foreach(_name IN LISTS _cuda_runtime_names)
        string(MAKE_C_IDENTIFIER "${_name}" _name_id)
        set(_source "${_cuda_source_${_name_id}}")
        set(_destination "${_cuda_destination_real}/${_name}")
        if(NOT EXISTS "${_destination}")
            message(FATAL_ERROR
                "DeployCudaRuntime: packaged CUDA DLL missing: ${_destination}")
        endif()
        file(SHA256 "${_source}" _source_sha256)
        file(SHA256 "${_destination}" _destination_sha256)
        if(NOT _source_sha256 STREQUAL _destination_sha256)
            message(FATAL_ERROR
                "DeployCudaRuntime: packaged CUDA DLL differs from Toolkit source: ${_name}")
        endif()
    endforeach()

    message(STATUS
        "Verified CUDA ${ROUNDTABLE_CUDA_TOOLKIT_VERSION} runtime package: ${_cuda_runtime_names}")
endif()
