#!/usr/bin/env python3
"""
Amiga ToolObject (.info) Icon Converter
Converts a PNG image into a valid AmigaOS icon file containing proper
ToolObject headers, gadget structures, color palettes, and bitplane data.
"""

import sys
import struct
from PIL import Image

def rgb_to_amiga_12bit(r, g, b):
    """Convert RGB888 to Amiga 12-bit color word (0x0RGB)."""
    return ((r & 0xF0) << 4) | ((g & 0xF0)) | (b >> 4)

def chunky_to_planar(image, num_colors):
    """
    Converts a quantized PIL Image into interleaved Amiga planar bitplane data.
    For N colors, log2(N) bitplanes are generated.
    """
    width, height = image.size
    pixels = list(image.getdata())

    # Number of bitplanes (e.g., 4 colors = 2 planes, 32 colors = 5 planes)
    if num_colors <= 4:
        depth = 2
    elif num_colors <= 16:
        depth = 4
    else:
        depth = 5

    row_bytes = ((width + 15) // 16) * 2
    plane_size = row_bytes * height
    planes = bytearray(plane_size * depth)

    for y in range(height):
        for x in range(width):
            idx = pixels[y * width + x]
            byte_idx = y * row_bytes + (x // 8)
            bit = 7 - (x % 8)

            for p in range(depth):
                if (idx >> p) & 1:
                    planes[(p * plane_size) + byte_idx] |= (1 << bit)

    return bytes(planes), depth

def create_amiga_info(png_path, output_info_path, is_high_res=False):
    """
    Reads a PNG file and generates a complete Amiga ToolObject (.info) file.
    """
    try:
        img = Image.open(png_path).convert("RGB")
    except Exception as e:
        print(f"Error opening image {png_path}: {e}")
        return False

    # Set icon dimensions and color depth according to Amiga standards
    if is_high_res:
        width, height = 96, 96
        num_colors = 32
        icon_type = 3
    else:
        width, height = 64, 48
        num_colors = 4
        icon_type = 3
    # 1 = WBDISK (Disk Icon)
    # 2 = WBDRAWER (Drawer Icon)
    # 3 = WBTOOL (Tool Icon)
    # 4 = WBGARBAGE
    # 5 = WBDEVICE
    # 6 = WBKICK
    # 7 = WBAPP

    # Resize with high-quality resampling and quantize to the target palette
    img_resized = img.resize((width, height), Image.Resampling.LANCZOS)
    img_quant = img_resized.quantize(colors=num_colors, method=Image.Quantize.MEDIANCUT)

    palette_flat = img_quant.getpalette()[:num_colors * 3]
    bitplane_data, depth = chunky_to_planar(img_quant, num_colors)

    # Calculate structure sizes and offsets
    # Standard Amiga ToolObject binary layout offsets
    gadget_width = width
    gadget_height = height

    with open(output_info_path, 'wb') as f:
        # 1. ToolObject Header
        f.write(struct.pack('>H', 0xE310))  # do_Magic
        f.write(struct.pack('>H', 1))       # do_Version

        # 2. Embedded Gadget Structure (struct Gadget)
        f.write(struct.pack('>I', 0))       # NextGadget (NULL)
        f.write(struct.pack('>HH', 0, 0))   # LeftEdge, TopEdge
        f.write(struct.pack('>HH', gadget_width, gadget_height))  # Width, Height
        f.write(struct.pack('>H', 0x110))   # Flags (GFLG_GADGHIMAGE | GFLG_RELVERIFY)
        f.write(struct.pack('>H', 8))       # Activation (GACT_RELVERIFY)
        f.write(struct.pack('>H', 0))       # GadgetType

        # Pointers in ToolObject stream are represented as relative or absolute offsets/placeholders
        # Pointing to embedded Image structure right after the gadget
        f.write(struct.pack('>I', 78))      # GadgetRender pointer placeholder
        f.write(struct.pack('>I', 0))       # SelectRender
        f.write(struct.pack('>I', 0))       # TextAttrs
        f.write(struct.pack('>I', 0))       # MutualExclude
        f.write(struct.pack('>I', 0))       # SpecialInfo
        f.write(struct.pack('>H', 0))       # GadgetID
        f.write(struct.pack('>I', 0))       # UserData

        # 3. ToolObject Fields (struct ToolObject continuation)
        f.write(struct.pack('>B', icon_type)) # do_Type (1 = WBTOOL)
        f.write(b'\x00')                    # Pad byte
        f.write(struct.pack('>I', 0))       # do_DefaultTool
        f.write(struct.pack('>I', 0))       # do_ToolTypes
        f.write(struct.pack('>ii', 20, 20)) # do_CurrentX, do_CurrentY
        f.write(struct.pack('>I', 0))       # do_DrawerData
        f.write(struct.pack('>I', 0))       # do_ToolWindow
        f.write(struct.pack('>i', 0))       # do_StackSize

        # 4. Embedded Image Structure (struct Image)
        f.write(struct.pack('>HH', 0, 0))   # LeftEdge, TopEdge
        f.write(struct.pack('>HH', width, height))  # Width, Height
        f.write(struct.pack('>H', depth))   # Depth
        f.write(struct.pack('>I', 110))     # ImageData pointer offset placeholder
        f.write(struct.pack('>B', 3))      # PlanePick
        f.write(struct.pack('>B', 00))      # PlaneOnOff
        f.write(struct.pack('>I', 150 + len(bitplane_data))) # NextImage pointer

        # 5. Color Palette Table (Amiga 12-bit RGB words)
        for i in range(0, len(palette_flat), 3):
            r = palette_flat[i]
            g = palette_flat[i+1]
            b = palette_flat[i+2]
            color_word = rgb_to_amiga_12bit(r, g, b)
            f.write(struct.pack('>H', color_word))

        # Pad remaining palette slots if less than max capacity
        remaining_colors = (1 << depth) - num_colors
        for _ in range(remaining_colors):
            f.write(struct.pack('>H', 0x0000))

        # 6. Planar Bitplane Pixel Data
        f.write(bitplane_data)

    print(f"Successfully created Amiga icon: {output_info_path} ({width}x{height}, {num_colors} colors, depth: {depth})")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python amiga_icon_converter.py <input.png> <output.info> [--hires]")
        sys.exit(1)

    input_png = sys.argv[1]
    output_info = sys.argv[2]
    hires = "--hires" in sys.argv

    create_amiga_info(input_png, output_info, is_high_res=hires)


### Usage Instructions
# 1. Save the code above as **`amiga_icon_converter.py`**.
# 2. Run it from your command line with Python 3 and Pillow installed (`pip install Pillow`):
#    * For the classic 4-color 64x48 icon:
#      ```bash
#      python amiga_icon_converter.py smashtool_icon_src.png SmashTool.info
#      ```
#    * For the high-res 32-color 96x96 icon:
#      ```bash
#      python amiga_icon_converter.py smashtool_icon_src.png SmashTool_HiRes.info --hires
#      ```
# 3. Copy the resulting `.info` file directly onto your Amiga OS partition alongside your `SmashTool` executable binary.
