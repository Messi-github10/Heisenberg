import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
configuration = sys.argv[1] if len(sys.argv) > 1 else "Release"
program = root / "build" / "Tests" / "Framework" / configuration / "HeisenbergFrameworkTests.exe"
log = root / "build" / "aces-vulkan-validation.log"
environment = os.environ.copy()
environment["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
filters = "NativePreviewTest.*:GradeRendererTest.*:CompositionRendererInteropTest.*:ExportEngineTest.H264AndH265ExportAndFullyDecode:GpuFixture.*"
with log.open("w", encoding="utf-8") as output:
    result = subprocess.run(
        [str(program), "--gtest_color=no", "--gtest_filter=" + filters],
        cwd=root,
        env=environment,
        stdout=output,
        stderr=subprocess.STDOUT,
        timeout=180,
    )
messages = log.read_text(encoding="utf-8", errors="replace").splitlines()
issues = [line for line in messages if "Validation Error" in line or "VUID-" in line
          or "Vulkan validation:" in line]
print("Vulkan integration exit:", result.returncode, "validation error lines:", len(issues))
print("\n".join(messages[-6:]))
if result.returncode or issues:
    print("\n".join(issues[:20]))
    raise SystemExit(1)
