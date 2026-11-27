#!/usr/bin/env python3
"""Regression guard for lazy Vulkan render-pass construction."""

# Source: indra/newview/wolfvkscene.cpp:1139 and indra/llrender/vk/wolfvkrendertarget.cpp:301-347.
# A pipeline must be created with an actual render pass before the first G-buffer draw.
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCENE = (ROOT / "indra/newview/wolfvkscene.cpp").read_text(encoding="utf-8")
TARGET = (ROOT / "indra/llrender/vk/wolfvkrendertarget.h").read_text(encoding="utf-8")


def main() -> None:
    assert "bool ensurePass(std::string& error);" in TARGET
    start = SCENE.index("bool WolfVkScene::recordGBufferDraw")
    end = SCENE.index("void WolfVkScene::drawFullbright", start)
    record = SCENE[start:end]
    assert "gbuffer.ensurePass" in record
    assert record.index("gbuffer.ensurePass") < record.index("mPipelines.get(program, state, gbuffer.renderPass())")


if __name__ == "__main__":
    main()
