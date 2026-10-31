#!/usr/bin/env python3
"""Standalone tests for the saved-image ROI comparator."""

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
TOOL_PATH = ROOT / "tools" / "console_capture" / "check_capture_region.py"
SPEC = importlib.util.spec_from_file_location("check_capture_region", TOOL_PATH)
assert SPEC and SPEC.loader
TOOL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOL)


class CaptureRegionCheckTests(unittest.TestCase):
	def setUp(self):
		self.temporary = tempfile.TemporaryDirectory()
		self.directory = Path(self.temporary.name)
		self.reference = self.directory / "approved.png"
		self.capture = self.directory / "capture.png"
		Image.new("RGB", (8, 6), (10, 20, 30)).save(self.reference)

	def tearDown(self):
		self.temporary.cleanup()

	def save_capture(self, changes=()):
		image = Image.new("RGB", (8, 6), (10, 20, 30))
		for x, y, color in changes:
			image.putpixel((x, y), color)
		image.save(self.capture)

	def test_tolerance_and_differing_pixel_allowance(self):
		self.save_capture(((2, 2, (12, 24, 30)), (3, 2, (10, 20, 36))))
		result = TOOL.compare_capture_region(
			self.capture, self.reference, (1, 1, 4, 3), (2, 4, 5), 1)
		self.assertTrue(result["pass"])
		self.assertEqual(result["differing_pixels"], 1)
		self.assertEqual(result["compared_pixels"], 12)
		self.assertEqual(result["max_channel_delta"], [2, 4, 6])

	def test_difference_outside_roi_is_ignored(self):
		self.save_capture(((7, 5, (255, 0, 255)),))
		result = TOOL.compare_capture_region(
			self.capture, self.reference, (0, 0, 4, 4))
		self.assertTrue(result["pass"])
		self.assertEqual(result["differing_pixels"], 0)

	def test_bad_roi_fails_and_cli_returns_one(self):
		self.save_capture(((2, 2, (11, 20, 30)), (3, 2, (12, 20, 30))))
		command = [sys.executable, str(TOOL_PATH), str(self.capture),
			str(self.reference), "--roi", "1", "1", "4", "3",
			"--max-differing-pixels", "1"]
		completed = subprocess.run(command, text=True, capture_output=True, check=False)
		self.assertEqual(completed.returncode, 1)
		result = json.loads(completed.stdout)
		self.assertFalse(result["pass"])
		self.assertEqual(result["differing_pixels"], 2)

	def test_size_and_crop_bounds_are_strict_input_errors(self):
		Image.new("RGB", (7, 6), (10, 20, 30)).save(self.capture)
		with self.assertRaisesRegex(TOOL.CaptureCheckError, "image sizes differ"):
			TOOL.compare_capture_region(self.capture, self.reference, (0, 0, 2, 2))
		self.save_capture()
		with self.assertRaisesRegex(TOOL.CaptureCheckError, "exceeds image bounds"):
			TOOL.compare_capture_region(self.capture, self.reference, (7, 5, 2, 1))

	def test_rgba_alpha_is_compared_when_requested(self):
		approved = Image.new("RGBA", (2, 2), (1, 2, 3, 255))
		actual = approved.copy()
		actual.putpixel((1, 1), (1, 2, 3, 250))
		approved.save(self.reference)
		actual.save(self.capture)
		result = TOOL.compare_capture_region(
			self.capture, self.reference, (0, 0, 2, 2), (0, 0, 0, 4), 0)
		self.assertFalse(result["pass"])
		self.assertEqual(result["max_channel_delta"], [0, 0, 0, 5])


if __name__ == "__main__":
	unittest.main()
