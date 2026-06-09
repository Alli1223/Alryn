# Compiles GLSL shaders to SPIR-V with glslc and wires them as a build
# dependency of a target.  Output .spv files are staged next to the runtime
# binaries (build/bin/shaders) so they can be loaded with a relative path.
#
# Usage:
#   alryn_compile_shaders(<target>
#       SHADERS  a.vert b.frag ...
#       [OUTPUT_DIR <dir>])

find_program(ALRYN_GLSLC
    NAMES glslc
    HINTS ${Vulkan_GLSLC_EXECUTABLE} ENV VULKAN_SDK
    PATH_SUFFIXES bin)

function(alryn_compile_shaders target)
    set(oneValue OUTPUT_DIR)
    set(multiValue SHADERS)
    cmake_parse_arguments(ARG "" "${oneValue}" "${multiValue}" ${ARGN})

    if(NOT ARG_OUTPUT_DIR)
        set(ARG_OUTPUT_DIR ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/shaders)
    endif()

    if(NOT ALRYN_GLSLC)
        message(WARNING "glslc not found - shaders for '${target}' will NOT be compiled")
        return()
    endif()

    file(MAKE_DIRECTORY ${ARG_OUTPUT_DIR})

    set(spv_outputs "")
    foreach(src ${ARG_SHADERS})
        if(NOT IS_ABSOLUTE ${src})
            set(src ${CMAKE_CURRENT_SOURCE_DIR}/${src})
        endif()
        get_filename_component(fname ${src} NAME)
        set(out ${ARG_OUTPUT_DIR}/${fname}.spv)
        add_custom_command(
            OUTPUT  ${out}
            COMMAND ${ALRYN_GLSLC} --target-env=vulkan1.3 -O ${src} -o ${out}
            DEPENDS ${src}
            COMMENT "glslc ${fname} -> ${fname}.spv"
            VERBATIM)
        list(APPEND spv_outputs ${out})
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${spv_outputs})
    add_dependencies(${target} ${target}_shaders)
endfunction()
