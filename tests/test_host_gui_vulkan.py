"""Real offscreen Vulkan GUI pixels, atlas lifecycle and attachment formats."""
import json
import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]
IMGUI = ROOT / "third_party/imgui"



@pytest.fixture(scope="module")
def gui_vulkan_contract(tmp_path_factory, vulkan_replay):
    if not shutil.which("c++") or not shutil.which("cc"):
        pytest.skip("A C/C++ toolchain are required for the Vulkan GUI contract")
    output = tmp_path_factory.mktemp("gui-vulkan")
    volk = output / "volk.o"
    subprocess.run(["cc", "-c", str(ROOT / "third_party/parallel-psx/volk/volk.c"), "-o", str(volk)],
                   cwd=ROOT, check=True, capture_output=True, timeout=90)
    binary = output / "gui-vulkan-contract"
    command = ["c++", "-std=c++14", "-O0", "-DIMGUI_IMPL_VULKAN_USE_VOLK", "-DVK_NO_PROTOTYPES",
               "-Ibackends/host", f"-I{IMGUI}", "-Ithird_party/parallel-psx", "-Ithird_party/parallel-psx/volk",
               "tests/gui_vulkan_harness.cpp", "backends/host/gui_vulkan.cpp",
               str(IMGUI / "backends/imgui_impl_vulkan.cpp"),
               *[str(IMGUI / source) for source in ("imgui.cpp", "imgui_draw.cpp", "imgui_widgets.cpp", "imgui_tables.cpp")],
               str(volk), "-ldl", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True, timeout=180)
    return binary


def test_vulkan_gui_pixels_formats_font_rebuild_resize_and_reinitialize(gui_vulkan_contract):
    # Test the official backend with synthetic glyphs; the actual UI supplies its
    # approved Chakra/Spleen atlas through the exact same upload/descriptor path.
    env = dict(os.environ)
    layers = list(Path("/usr/share/vulkan/explicit_layer.d").glob("*khronos*validation*.json"))
    if layers:
        env.update(VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation",
                   VK_LAYER_ENABLES="VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT")
    run = subprocess.run([str(gui_vulkan_contract)], env=env, cwd=ROOT,
                         text=True, capture_output=True, timeout=90)
    assert run.returncode == 0, run.stderr
    result = json.loads(run.stdout)
    assert result == {"passed": True, "frames": 12, "formats": ["RGBA", "BGRA"],
                      "font_rebuild": True, "resize": True, "texture_text": True, "reinitializations": 2}
    assert "Validation Error" not in run.stderr and "SYNC-HAZARD" not in run.stderr
