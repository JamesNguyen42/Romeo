"""Renderer replay integration tests; no model, service, network or Python packages needed.

ROMEO_RENDER=/path/to/romeo-render python3 tests/renderer_preview_test.py -v
"""

import csv
import hashlib
import json
import os
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path


BINARY = Path(os.environ.get("ROMEO_RENDER", "build/romeo-render")).resolve()


def decode_rgba_png(path):
    """Decode stb's lossless RGBA PNG so replay pixels can be checked against native output."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    chunks, cursor = {}, 8
    while cursor < len(data):
        size = struct.unpack_from(">I", data, cursor)[0]
        kind = data[cursor + 4:cursor + 8]
        chunks[kind] = chunks.get(kind, b"") + data[cursor + 8:cursor + 8 + size]
        cursor += size + 12
    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunks[b"IHDR"])
    assert (depth, color, interlace) == (8, 6, 0)
    encoded = zlib.decompress(chunks[b"IDAT"])
    stride = width * 4
    pixels = bytearray(height * stride)
    for y in range(height):
        mode = encoded[y * (stride + 1)]
        row = encoded[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        for x, value in enumerate(row):
            pos = y * stride + x
            left = pixels[pos - 4] if x >= 4 else 0
            up = pixels[pos - stride] if y else 0
            corner = pixels[pos - stride - 4] if y and x >= 4 else 0
            predictor = left + up - corner
            distances = (abs(predictor - left), abs(predictor - up), abs(predictor - corner))
            paeth = (left, up, corner)[distances.index(min(distances))]
            pixels[pos] = (value + (0, left, up, (left + up) // 2, paeth)[mode]) & 255
    return width, height, pixels


class RendererPreviewTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not BINARY.is_file():
            raise RuntimeError(f"Build romeo-render first or set ROMEO_RENDER: {BINARY}")
        cls.temp = tempfile.TemporaryDirectory(prefix="romeo-preview-test-")
        cls.root = Path(cls.temp.name)
        cls.first = cls.run_renderer("first", 44, 1, 40)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    @classmethod
    def run_renderer(cls, name, frames, seed, previews=None, setup="A"):
        out = cls.root / name
        args = [str(BINARY), "--setup", setup, "--frames", str(frames), "--view-seed", str(seed), "--out", str(out)]
        if previews is not None:
            args.extend(["--preview-frames", str(previews)])
        subprocess.run(args, check=True, capture_output=True, text=True, timeout=120)
        return out

    def test_forty_real_images_reference_recorded_metrics(self):
        manifest = json.loads((self.first / "preview.json").read_text())
        summary = json.loads((self.first / "summary.json").read_text())
        with (self.first / "frames.csv").open() as source:
            rows = {int(row["frame"]): float(row["ms"]) for row in csv.DictReader(source)}
        self.assertEqual(manifest["capture"], "rerendered_after_benchmark")
        self.assertEqual((manifest["count"], len(manifest["frames"])), (40, 40))
        self.assertEqual((manifest["width"], manifest["height"]), (480, 270))
        self.assertEqual((manifest["source_width"], manifest["source_height"]), (1920, 1080))
        self.assertEqual((summary["view_seed"], summary["warmup_frames"], summary["measured_frames"]), (1, 40, 4))
        self.assertEqual(len(rows), 44)  # Replay must not add timing rows or summary samples.
        self.assertAlmostEqual(summary["max_ms"], max(rows[f] for f in range(40, 44)), places=3)
        positions, hashes = set(), set()
        for frame in manifest["frames"]:
            positions.add((frame["frame"], frame["view"]))
            self.assertEqual(frame["benchmark_frame_ms"], rows[frame["frame"]])
            self.assertGreater(frame["benchmark_frame_ms"], 0)
            png = (self.first / frame["file"]).read_bytes()
            self.assertEqual(struct.unpack_from(">II", png, 16), (480, 270))
            hashes.add(hashlib.sha256(png).hexdigest())
        self.assertEqual((len(positions), len(hashes)), (40, 40))
        events = [json.loads(line) for line in (self.first / "log.jsonl").read_text().splitlines()]
        preview_start = next(event for event in events if event["msg"] == "preview_start")
        self.assertLessEqual(summary["total_ms"], preview_start["t_ms"] + 0.1)

    def test_last_preview_matches_native_render_pixels(self):
        width, height, pixels = decode_rgba_png(self.first / "preview-39.png")
        raw = (self.first / "image.rgba").read_bytes()
        expected = bytearray()
        for y in range(height):
            for x in range(width):
                pos = ((y * 1080 // height) * 1920 + x * 1920 // width) * 4
                expected.extend(raw[pos:pos + 4])
        self.assertEqual(pixels, expected)

    def test_seed_repeats_and_changes_the_actual_scene_view(self):
        repeated = self.run_renderer("repeated", 44, 1)
        changed = self.run_renderer("changed", 44, 2, 1)
        first_pixels = (self.first / "image.rgba").read_bytes()
        self.assertEqual(first_pixels, (repeated / "image.rgba").read_bytes())
        self.assertNotEqual(first_pixels, (changed / "image.rgba").read_bytes())
        self.assertFalse((repeated / "preview.json").exists())
        self.assertEqual(json.loads((changed / "preview.json").read_text())["view_seed"], 2)

    def test_short_multiview_run_never_duplicates_samples(self):
        out = self.run_renderer("multiview", 2, 0, 40, "B")
        manifest = json.loads((out / "preview.json").read_text())
        positions = {(frame["frame"], frame["view"]) for frame in manifest["frames"]}
        self.assertEqual(manifest["count"], 8)
        self.assertEqual(positions, {(f, v) for f in range(2) for v in range(4)})

    def test_rejects_unbounded_or_malformed_arguments(self):
        for flag, value in [("--preview-frames", "41"), ("--preview-frames", "-1"),
                            ("--preview-frames", "1x"), ("--view-seed", "-1"),
                            ("--view-seed", "4294967296"), ("--view-seed", "")]:
            with self.subTest(flag=flag, value=value):
                result = subprocess.run([str(BINARY), flag, value], capture_output=True, timeout=5)
                self.assertEqual(result.returncode, 2)


if __name__ == "__main__":
    unittest.main()
