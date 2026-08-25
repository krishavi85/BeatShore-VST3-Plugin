"""Generates BeatShore.ico (multi-resolution Windows icon) from the real
BeatShore brand logo at assets/beatshore-logo.png -- not a placeholder or
a generic icon. Crops the circular badge mark out of the full logo
(excluding the "BeatShore" wordmark, which doesn't read at small icon
sizes), centers it on a square canvas, and rasterizes it at every size
Windows actually uses: taskbar/Explorer, Start menu, Add/Remove
Programs, and the installer wizard.

Usage: python generate_icon.py
Requires Pillow (pip install Pillow).
"""
from PIL import Image
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE_LOGO = os.path.join(HERE, "..", "beatshore-logo.png")
OUTPUT_ICO = os.path.join(HERE, "BeatShore.ico")
OUTPUT_PNG_1024 = os.path.join(HERE, "BeatShore-badge-1024.png")

# Bounding box of just the circular badge mark within the full logo
# (1024x1024), found by scanning the alpha channel for the badge's
# content cluster and excluding the "BeatShore" wordmark cluster below
# it -- see the badge-cropping steps in the session that produced this
# script for how these coordinates were derived (not eyeballed blindly:
# confirmed via per-row/per-column opaque-pixel density scans).
BADGE_BOX = (230, 228, 718, 568)

# Every size Windows actually requests an app icon at, per the standard
# Windows icon size set (Explorer/taskbar small and large, Start menu
# tiles, high-DPI variants) -- not just "a 256px icon and let Windows
# downscale it," which produces visibly worse results at the small sizes
# than rendering each one directly from the high-res source.
ICO_SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def main():
    logo = Image.open(SOURCE_LOGO).convert("RGBA")
    badge = logo.crop(BADGE_BOX)

    side = max(badge.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    x_off = (side - badge.width) // 2
    y_off = (side - badge.height) // 2
    canvas.paste(badge, (x_off, y_off), badge)

    # Keep a high-res master PNG around too -- useful for the installer
    # wizard image, a future plugin-editor splash, or re-deriving other
    # sizes later without re-cropping the original logo.
    master = canvas.resize((1024, 1024), Image.LANCZOS)
    master.save(OUTPUT_PNG_1024)

    # Pillow's ICO writer wants the base image plus a sizes list; it
    # generates each requested size from the base image itself, so pass
    # the largest, highest-quality version as the base for best results
    # at every embedded size.
    canvas.resize((256, 256), Image.LANCZOS).save(
        OUTPUT_ICO, format="ICO", sizes=[(s, s) for s in ICO_SIZES]
    )
    print(f"Wrote {OUTPUT_ICO} with sizes: {ICO_SIZES}")
    print(f"Wrote {OUTPUT_PNG_1024} (1024x1024 master)")


if __name__ == "__main__":
    main()
