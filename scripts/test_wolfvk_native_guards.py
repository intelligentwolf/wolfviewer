#!/usr/bin/env python3
"""Source-level regression guards for Native Vulkan (Verification Gate 1).

Verifies that when LLWindow::sWolfNativeVulkan / gGLManager.mIsNativeVulkan is active:
  1. LLWindowSDL creates an SDL_WINDOW_VULKAN window and does not call SDL_GL_CreateContext or SDL_GL_SwapWindow.
  2. LLViewerWindow initializes WolfVkNative::init() before LLViewerShaderMgr::setShaders().
  3. LLVertexBuffer::initClass() skips GL VBO pool & GL worker threads.
  4. LLImageGL::initClass() skips GL PBO & GL texture threads.
  5. LLRender::init() skips GL VAO / pixel store / glGetFloatv calls.
  6. WolfVkBridge::initAfterGL() returns immediately when WolfVkNative::isActive() (hybrid bridge unreachable).
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def main() -> None:
    sdl = read("indra/llwindow/llwindowsdl2.cpp")
    assert "SDL_WINDOW_VULKAN" in sdl
    assert "SDL_Vulkan_GetDrawableSize" in sdl
    assert "if (LLWindow::sWolfNativeVulkan)" in sdl
    assert "if (mWindow && !LLWindow::sWolfNativeVulkan)" in sdl

    vw = read("indra/newview/llviewerwindow.cpp")
    assert "WolfVkNative::init(mWindow" in vw
    assert vw.index("WolfVkNative::init(mWindow") < vw.index("LLViewerShaderMgr::instance()->setShaders()")

    vb = read("indra/llrender/llvertexbuffer.cpp")
    vb_init = vb[vb.index("void LLVertexBuffer::initClass"):vb.index("void LLVertexBuffer::unbind")]
    assert "if (gGLManager.mIsNativeVulkan)" in vb_init
    assert vb_init.index("if (gGLManager.mIsNativeVulkan)") < vb_init.index("sVBOPool = new")

    img = read("indra/llrender/llimagegl.cpp")
    img_init = img[img.index("void LLImageGL::initClass"):img.index("void LLImageGL::allocateConversionBuffer")]
    assert "if (gGLManager.mIsNativeVulkan)" in img_init
    assert img_init.index("if (gGLManager.mIsNativeVulkan)") < img_init.index("glGenBuffers")

    ren = read("indra/llrender/llrender.cpp")
    ren_init = ren[ren.index("bool LLRender::init("):ren.index("void LLRender::initVertexBuffer")]
    assert "if (gGLManager.mIsNativeVulkan)" in ren_init
    assert ren_init.index("if (gGLManager.mIsNativeVulkan)") < ren_init.index("glGenVertexArrays")

    bridge = read("indra/newview/wolfvkbridge.cpp")
    init_after = bridge[bridge.index("void initAfterGL("):bridge.index("bool isActive()")]
    assert "if (WolfVkNative::isActive())" in init_after
    assert init_after.index("if (WolfVkNative::isActive())") < init_after.index("probeInfo(")

    cap = bridge[bridge.index("void captureWorld()"):bridge.index("void captureWorld()") + 200]
    assert "if (WolfVkNative::isActive()) return;" in cap

    print("PASS: all native Vulkan source-level guards verified")


if __name__ == "__main__":
    main()
