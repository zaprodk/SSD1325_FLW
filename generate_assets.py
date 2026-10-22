import sys
import subprocess
import os
import glob
import re
import time
import io

# Auto-install Pillow if missing
try:
    from PIL import Image, ImageFilter, ImageDraw, ImageFont
except ImportError:
    print("Installing missing dependency: Pillow...")
    subprocess.check_call([sys.executable, "-m", "pip", "install", "Pillow"])
    from PIL import Image, ImageFilter, ImageDraw, ImageFont

# ==============================================================================
# PROJECT DIRECTORY RESOLUTION
# ==============================================================================
def find_project_dir():
    if "__file__" in globals():
        p = os.path.dirname(os.path.abspath(__file__))
        if os.path.exists(os.path.join(p, "assets")):
            return p
    p = os.getcwd()
    if os.path.exists(os.path.join(p, "assets")):
        return p
    return p

PROJECT_DIR = find_project_dir()
ASSETS_DIR = os.path.join(PROJECT_DIR, "assets")
WORDS_FILE_PRIMARY = os.path.join(ASSETS_DIR, "flw_uk.txt")
WORDS_FILE_FALLBACK = os.path.join(ASSETS_DIR, "words.txt")
OUTPUT_HEADER = os.path.join(PROJECT_DIR, "include", "generated_assets.h")

# ==============================================================================
# CONFIGURATION SETTINGS
# ==============================================================================
AUTO_BRIGHTNESS = True
ROTATION_ANGLE = 90

TARGET_PORTRAIT_WIDTH = 64
TARGET_PORTRAIT_HEIGHT = 128

MAX_SAFE_WIDTH = 62   # 1px margin left & right
MAX_SAFE_HEIGHT = 126 # 1px margin top & bottom
# ==============================================================================

def sanitize_var_name(filename):
    clean = re.sub(r'[^a-zA-Z0-9]', '_', os.path.splitext(filename)[0])
    return clean

def render_ttf_canvas(char, font_obj):
    big_canvas = Image.new("L", (300, 300), 0)
    draw = ImageDraw.Draw(big_canvas)
    draw.text((20, 20), char, fill=255, font=font_obj)
    
    bbox = big_canvas.getbbox()
    if not bbox:
        bbox = (0, 0, 10, 10)
        
    cropped = big_canvas.crop(bbox)
    cw, ch = cropped.size

    # Height-anchored scaling
    scale_h = MAX_SAFE_HEIGHT / float(ch)
    new_h = MAX_SAFE_HEIGHT
    calc_w = int(round(cw * scale_h))

    # Squish wide characters horizontally if exceeding MAX_SAFE_WIDTH
    new_w = MAX_SAFE_WIDTH if calc_w > MAX_SAFE_WIDTH else calc_w

    resized = cropped.resize((new_w, new_h), Image.Resampling.LANCZOS)

    canvas = Image.new("L", (TARGET_PORTRAIT_WIDTH, TARGET_PORTRAIT_HEIGHT), 0)
    offset_x = (TARGET_PORTRAIT_WIDTH - new_w) // 2
    offset_y = (TARGET_PORTRAIT_HEIGHT - new_h) // 2
    canvas.paste(resized, (offset_x, offset_y))
    return canvas

def process_pil_image(img):
    if AUTO_BRIGHTNESS:
        _, max_val = img.getextrema()
        if 0 < max_val < 255:
            scale_factor = 255.0 / max_val
            img = img.point(lambda p: min(255, int(p * scale_factor)))

    canvas_rot = img.rotate(ROTATION_ANGLE, expand=True)
    
    if hasattr(canvas_rot, "get_flattened_data"):
        pixels_8bit = list(canvas_rot.get_flattened_data())
    else:
        pixels_8bit = list(canvas_rot.getdata())

    # Smooth 4-bit nibble packing
    pixels = [min(15, int(round(p / 17.0))) for p in pixels_8bit]

    bytes_data = []
    for i in range(0, len(pixels), 2):
        left_pixel = pixels[i] & 0x0F
        right_pixel = pixels[i+1] & 0x0F
        bytes_data.append((right_pixel << 4) | left_pixel)

    return bytes_data

def write_header_with_retries(filepath, write_callback, max_retries=10, delay=0.5):
    """
    Renders header content in memory first. Skips disk rewrite if nothing changed
    (preventing rebuilds), and retries if locked by Dropbox/IDE.
    """
    buffer = io.StringIO()
    write_callback(buffer)
    new_content = buffer.getvalue()

    if os.path.exists(filepath):
        try:
            with open(filepath, "r", encoding="utf-8") as existing_file:
                if existing_file.read() == new_content:
                    print("[Asset Pipeline] Assets unchanged. Skipping header rewrite.")
                    return
        except Exception:
            pass

    for attempt in range(1, max_retries + 1):
        try:
            if os.path.exists(filepath):
                try:
                    os.chmod(filepath, 0o777)
                except Exception:
                    pass

            with open(filepath, "w", encoding="utf-8") as f:
                f.write(new_content)

            print(f"[Asset Pipeline] Assets updated and written to '{filepath}'.")
            return

        except PermissionError:
            if attempt < max_retries:
                print(f"[Asset Pipeline] Lock detected on '{filepath}'. Retrying in {delay}s ({attempt}/{max_retries})...")
                time.sleep(delay)
            else:
                print(f"\n[Asset Pipeline] ERROR: Could not write to '{filepath}' after {max_retries} attempts.")
                raise

def generate():
    os.makedirs(ASSETS_DIR, exist_ok=True)

    target_words_file = WORDS_FILE_PRIMARY if os.path.exists(WORDS_FILE_PRIMARY) else WORDS_FILE_FALLBACK
    words = []
    if os.path.exists(target_words_file):
        with open(target_words_file, "r", encoding="utf-8-sig") as wf:
            for line in wf:
                clean_word = re.sub(r'[^A-Z]', '', line.strip().upper())
                if len(clean_word) == 4:
                    words.append(clean_word)
        print(f"[Asset Pipeline] Loaded {len(words)} words from {target_words_file}")

    font_paths = sorted(
        glob.glob(os.path.join(ASSETS_DIR, "*.[tT][tT][fF]")) +
        glob.glob(os.path.join(ASSETS_DIR, "*.[oO][tT][fF]"))
    )

    if not font_paths:
        raise RuntimeError(f"ERROR: No TTF/OTF fonts found in '{ASSETS_DIR}/' directory!")

    required_chars = [str(i) for i in range(10)] + [chr(c) for c in range(ord('A'), ord('Z') + 1)]

    def write_content(f):
        f.write("// Auto-generated by generate_assets.py. Do not edit.\n")
        f.write("#ifndef GENERATED_ASSETS_H\n#define GENERATED_ASSETS_H\n\n")
        f.write("#include <pgmspace.h>\n#include <stdint.h>\n\n")

        processed_fonts = []

        for font_path in font_paths:
            font_filename = os.path.basename(font_path)
            font_var_prefix = sanitize_var_name(font_filename)
            font_display_name = os.path.splitext(font_filename)[0].replace("_", " ").replace("-", " ")

            try:
                font_obj = ImageFont.truetype(font_path, size=140)
            except Exception as e:
                print(f"[Asset Pipeline] Failed to load font '{font_filename}': {e}")
                continue

            print(f"[Asset Pipeline] Processing font: '{font_filename}'...")

            for char in required_chars:
                var_name = f"img_{font_var_prefix}_{char}"
                canvas = render_ttf_canvas(char, font_obj)
                bytes_data = process_pil_image(canvas)

                f.write(f"const uint8_t {var_name}[4096] PROGMEM = {{\n  ")
                for i, byte_val in enumerate(bytes_data):
                    f.write(f"0x{byte_val:02X}")
                    if i < len(bytes_data) - 1:
                        f.write(", ")
                    if (i + 1) % 16 == 0:
                        f.write("\n  ")
                f.write("};\n\n")

            processed_fonts.append({
                "var_prefix": font_var_prefix,
                "display_name": font_display_name
            })

        f.write(f"const uint8_t FONT_COUNT = {len(processed_fonts)};\n\n")
        f.write("const char* const FONT_NAMES[] = {\n")
        for font in processed_fonts:
            f.write(f'  "{font["display_name"]}",\n')
        f.write("};\n\n")

        f.write("const uint8_t* const FONT_GLYPHS[][36] PROGMEM = {\n")
        for font in processed_fonts:
            f.write("  {\n")
            prefix = font["var_prefix"]
            for char in required_chars:
                f.write(f"    img_{prefix}_{char},\n")
            f.write("  },\n")
        f.write("};\n\n")

        f.write(f"const char FLW_WORDS[{max(1, len(words))}][8] PROGMEM = {{\n")
        if len(words) > 0:
            for i, w in enumerate(words):
                f.write(f'  "{w}"')
                if i < len(words) - 1:
                    f.write(",")
                f.write("\n")
        else:
            f.write('  "TIME"\n')
        f.write("};\n\n")
        f.write(f"const uint16_t FLW_WORD_COUNT = {max(1, len(words))};\n\n")

        f.write("#endif // GENERATED_ASSETS_H\n")

        print(f"[Asset Pipeline] Successfully generated assets for {len(processed_fonts)} fonts.")

    os.makedirs(os.path.dirname(OUTPUT_HEADER), exist_ok=True)
    write_header_with_retries(OUTPUT_HEADER, write_content)

if __name__ == "__main__":
    generate()