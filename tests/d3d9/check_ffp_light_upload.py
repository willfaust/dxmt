#!/usr/bin/env python3
"""Exercise the production FFP light packer with recycled NaN-filled bytes.

Host check only; actual shader/rendering verification requires a device.
An optional source path permits running the identical test against a baseline.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(sys.argv[1]) if len(sys.argv) > 1 else root / "src/d3d9/d3d9_device.cpp"
text = source.read_text()
anchor = "    std::memcpy(ffp_base + 240, amb, sizeof(amb));"
start = text.index(anchor) + len(anchor)
end = text.index("    // Texture matrices", start)
production = text[start:end]

fixture = r"""
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
struct Color { float r, g, b, a; };
struct Vector { float x, y, z; };
struct D3DLIGHT9 {
  uint32_t Type;
  Color Diffuse, Specular, Ambient;
  Vector Position, Direction;
  float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
};
static_assert(sizeof(D3DLIGHT9) == 104 && offsetof(D3DLIGHT9, Diffuse) == 4);
struct Ffp { alignas(D3DLIGHT9) float lights[8][26]; };
struct Pod { Ffp *ffp; uint32_t ffp_light_count; };
static void pack(char *ffp_base, const Pod &pod) {
""" + production + r"""
}
int main() {
  Ffp ffp{};
  for (unsigned i = 0; i < 8; ++i) {
    D3DLIGHT9 light{};
    light.Type = 3;
    light.Diffuse = {float(i)+.1f, float(i)+.2f, float(i)+.3f, .4f};
    light.Specular = {.5f, .6f, .7f, .8f};
    light.Ambient = {.9f, 1.f, 1.1f, 1.2f};
    light.Position = {1.f, 2.f, 3.f};
    light.Direction = {4.f, 5.f, 6.f};
    light.Range = 7.f; light.Falloff = 8.f;
    light.Attenuation0 = 9.f; light.Attenuation1 = 10.f; light.Attenuation2 = 11.f;
    light.Theta = .6f; light.Phi = .8f;
    std::memcpy(ffp.lights[i], &light, sizeof(light));
  }
  unsigned cases = 0;
  for (unsigned count = 0; count <= 8; ++count) {
    alignas(16) char buffer[1280];
    std::memset(buffer, 0xff, sizeof(buffer)); // NaNs in every stale float slot.
    pack(buffer, Pod{&ffp, count});
    for (unsigned i = 0; i < 256; ++i)
      if (static_cast<unsigned char>(buffer[i]) != 0xff) return 1;
    for (unsigned i = 1152; i < sizeof(buffer); ++i)
      if (static_cast<unsigned char>(buffer[i]) != 0xff) return 2;
    for (unsigned i = 0; i < 8; ++i) {
      float actual[28];
      std::memcpy(actual, buffer + 256 + i * 112, sizeof(actual));
      if (i >= count) {
        for (float value : actual) {
          if (value != 0.f || !std::isfinite(value)) {
            std::fprintf(stderr, "FAIL: count=%u inactive slot=%u retains stale bytes\n", count, i);
            return 3;
          }
        }
      } else {
        float expected[28] = {
          float(i)+.1f, float(i)+.2f, float(i)+.3f, .4f,
          .5f,.6f,.7f,.8f,.9f,1.f,1.1f,1.2f,
          1.f,2.f,3.f,7.f,4.f,5.f,6.f,3.f,9.f,10.f,11.f,8.f,
          std::cos(.3f),std::cos(.4f),0.f,0.f
        };
        for (unsigned lane = 0; lane < 28; ++lane)
          if (!std::isfinite(actual[lane]) || std::fabs(actual[lane] - expected[lane]) > 1e-6f) return 4;
      }
    }
    ++cases;
  }
  std::printf("PASS: %u active-count cases, stale NaN slots cleared, active light packing and bounds preserved\n", cases);
}
"""
with tempfile.TemporaryDirectory(prefix="dxmt-ffp-light-") as directory:
    cpp = Path(directory) / "light_upload.cpp"
    binary = Path(directory) / "light_upload"
    cpp.write_text(fixture)
    subprocess.run(["clang++", "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
