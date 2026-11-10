# -*- cmake -*-
# [VULKAN BACKEND 2026-11-08] SPIR-V for the native Vulkan renderer (indra/llrender/vk, docs/VULKAN_BACKEND.md).
#
# Every GLSL program variant the viewer can load is stitched as LLShaderMgr::loadShaderFile stitches it, turned into
# Vulkan GLSL (scripts/shader_vulkan.py: loose uniforms into one std140 block per stage, sampler and varying layouts)
# and compiled with glslang -V into app_settings/shaders/spv/<key>.{vert,frag}.spv plus manifest.json, where <key> is
# the MD5 of the program's recipe that the viewer recomputes at run time (WolfVkShaderMirror). The folder lives in the
# source tree beside the .glsl files (git-ignored) so the ordinary packaging of app_settings/shaders ships it.
# The backend's own Vulkan GLSL (class1/wolfvk/*.glsl: the composite pass) is compiled the same way to spv/wolfvk_*.spv.
#
# glslang: the same binary ShaderCheck.cmake finds (GLSLANG_VALIDATOR; PATH, $VULKAN_SDK/bin, the CI's pinned install
# from .github/scripts/install-glslang.sh). Without it the step is skipped with a warning and the Vulkan renderer
# reports "no SPIR-V" for every program at run time (it still starts and falls back to drawing nothing of its own).
#
#   WOLF_VULKAN_SHADERS    ON   build the SPIR-V as part of the viewer build (target wolfvk_shaders)
#   WOLF_VULKAN_SHADERS_QUICK  OFF  the quick scenario set only (development)

include(Python)
include(ShaderCheck)   # GLSLANG_VALIDATOR lookup lives there

set(WOLF_VULKAN_SHADERS ON CACHE BOOL "Compile every shader program variant to Vulkan SPIR-V for the Vulkan renderer")
set(WOLF_VULKAN_SHADERS_QUICK OFF CACHE BOOL "Vulkan SPIR-V: the quick scenario set only")

macro (wolfvk_compile_shaders _target)
  if (WOLF_VULKAN_SHADERS)
    if (DEFINED ENV{GLSLANG_VALIDATOR} AND NOT GLSLANG_VALIDATOR)
      set(GLSLANG_VALIDATOR "$ENV{GLSLANG_VALIDATOR}" CACHE FILEPATH "glslangValidator")
    endif ()
    find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang PATHS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin" DOC "glslangValidator")
    if (NOT GLSLANG_VALIDATOR)
      message(WARNING "glslangValidator not found: no Vulkan SPIR-V is built (the Vulkan renderer will report missing shaders)")
    else ()
      set(WOLFVK_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/app_settings/shaders")
      set(WOLFVK_SPV_DIR "${WOLFVK_SHADERS_DIR}/spv")
      file(GLOB_RECURSE WOLFVK_GLSL_INPUTS CONFIGURE_DEPENDS "${WOLFVK_SHADERS_DIR}/*.glsl")
      set(WOLFVK_STAMP "${CMAKE_CURRENT_BINARY_DIR}/wolfvk_shaders.stamp")
      set(WOLFVK_ARGS --glslang "${GLSLANG_VALIDATOR}" --shaders "${WOLFVK_SHADERS_DIR}" --vulkan-out "${WOLFVK_SPV_DIR}" --no-fail
          --json "${CMAKE_CURRENT_BINARY_DIR}/wolfvk_shaders.json")
      if (WOLF_VULKAN_SHADERS_QUICK)
        list(APPEND WOLFVK_ARGS --quick)
      endif ()
      add_custom_command(
          OUTPUT "${WOLFVK_STAMP}"
          COMMAND ${CMAKE_COMMAND} -E make_directory "${WOLFVK_SPV_DIR}"
          COMMAND ${PYTHON_EXECUTABLE} "${SCRIPTS_DIR}/shader_check.py" ${WOLFVK_ARGS}
          COMMAND ${GLSLANG_VALIDATOR} -V -S vert -o "${WOLFVK_SPV_DIR}/wolfvk_composite.vert.spv" "${WOLFVK_SHADERS_DIR}/class1/wolfvk/compositeV.glsl"
          COMMAND ${GLSLANG_VALIDATOR} -V -S frag -o "${WOLFVK_SPV_DIR}/wolfvk_composite.frag.spv" "${WOLFVK_SHADERS_DIR}/class1/wolfvk/compositeF.glsl"
          COMMAND ${CMAKE_COMMAND} -E touch "${WOLFVK_STAMP}"
          DEPENDS "${SCRIPTS_DIR}/shader_check.py" "${SCRIPTS_DIR}/shader_vulkan.py" ${WOLFVK_GLSL_INPUTS}
          COMMENT "Compiling every shader program variant to Vulkan SPIR-V (scripts/shader_check.py --vulkan-out)"
          VERBATIM
          )
      add_custom_target(wolfvk_shaders ALL DEPENDS "${WOLFVK_STAMP}")
      add_dependencies(${_target} wolfvk_shaders)
    endif ()
  endif ()
endmacro (wolfvk_compile_shaders)
