# -*- cmake -*-
#
# Shader pre-compile check (scripts/shader_check.py).
#
# Every GLSL program the viewer can load is stitched the way LLShaderMgr::loadShaderFile
# stitches it at run time (version line, injected defines, attached shared objects, every
# shader level the fallback can reach) and compiled and linked with glslangValidator, so a
# file/define combination that does not build fails the build here and not a resident's
# viewer (ticket 643: the Water vertex shader on a GTX 1070 with every optional graphics
# feature switched off).
#
#   SHADER_CHECK           ON   run the check as part of the build: target check_shaders,
#                               which the viewer binary depends on. It re-runs only when a
#                               .glsl file or the script changes.
#   SHADER_CHECK_REQUIRED  OFF  fail the configure when glslangValidator cannot be found
#                               (the CI build sets this ON after installing glslang).
#   SHADER_CHECK_QUICK     OFF  the four main settings scenarios on two GPU profiles only.
#   GLSLANG_VALIDATOR           glslangValidator; found on PATH, in $VULKAN_SDK/bin, or
#                               given by the GLSLANG_VALIDATOR environment variable.
#
# `ctest -R shader_check` runs the same check from the build directory.

include(Python)

set(SHADER_CHECK ON CACHE BOOL "Compile and link every shader program variant with glslangValidator as part of the build")
set(SHADER_CHECK_REQUIRED OFF CACHE BOOL "Fail the configure when glslangValidator is not found (the shader check must run)")
set(SHADER_CHECK_QUICK OFF CACHE BOOL "Shader check: the four main scenarios on two GPU profiles instead of the full set")

macro (check_shaders _target)
  if (SHADER_CHECK)
    if (DEFINED ENV{GLSLANG_VALIDATOR} AND NOT GLSLANG_VALIDATOR)
      set(GLSLANG_VALIDATOR "$ENV{GLSLANG_VALIDATOR}" CACHE FILEPATH "glslangValidator for the shader check")
    endif ()
    find_program(GLSLANG_VALIDATOR
                 NAMES glslangValidator glslang
                 PATHS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin"
                 DOC "glslangValidator for the shader check")
    if (NOT GLSLANG_VALIDATOR)
      if (SHADER_CHECK_REQUIRED)
        message(FATAL_ERROR "SHADER_CHECK_REQUIRED is ON but glslangValidator was not found. Install glslang "
                            "(https://github.com/KhronosGroup/glslang/releases) or set GLSLANG_VALIDATOR.")
      else ()
        message(WARNING "glslangValidator not found: the shader pre-compile check (scripts/shader_check.py) will "
                        "not run. Install glslang, or configure with -DSHADER_CHECK:BOOL=OFF to silence this.")
      endif ()
    else ()
      message(STATUS "Shader check: ${GLSLANG_VALIDATOR}")
      set(SHADER_CHECK_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app_settings/shaders")
      file(GLOB_RECURSE SHADER_CHECK_INPUTS CONFIGURE_DEPENDS "${SHADER_CHECK_SHADERS_DIR}/*.glsl")
      set(SHADER_CHECK_STAMP "${CMAKE_CURRENT_BINARY_DIR}/shader_check.stamp")
      set(SHADER_CHECK_ARGS
          --glslang "${GLSLANG_VALIDATOR}"
          --shaders "${SHADER_CHECK_SHADERS_DIR}"
          --json "${CMAKE_CURRENT_BINARY_DIR}/shader_check.json")
      if (SHADER_CHECK_QUICK)
        list(APPEND SHADER_CHECK_ARGS --quick)
      endif ()
      add_custom_command(
          OUTPUT "${SHADER_CHECK_STAMP}"
          COMMAND ${PYTHON_EXECUTABLE} "${SCRIPTS_DIR}/shader_check.py" ${SHADER_CHECK_ARGS}
          COMMAND ${CMAKE_COMMAND} -E touch "${SHADER_CHECK_STAMP}"
          DEPENDS "${SCRIPTS_DIR}/shader_check.py" ${SHADER_CHECK_INPUTS}
          COMMENT "Checking that every shader program variant compiles and links (scripts/shader_check.py)"
          VERBATIM
          )
      add_custom_target(check_shaders ALL DEPENDS "${SHADER_CHECK_STAMP}")
      # The viewer binary waits for the check, so a broken shader stops the build early.
      add_dependencies(${_target} check_shaders)
      enable_testing()
      add_test(NAME shader_check
               COMMAND ${PYTHON_EXECUTABLE} "${SCRIPTS_DIR}/shader_check.py" ${SHADER_CHECK_ARGS})
    endif ()
  endif ()
endmacro (check_shaders)
