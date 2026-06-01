#!/usr/bin/env python3
"""
Font Atlas Generator

This script generates a font atlas image containing ASCII characters from 33 to 126 (! to ~).
All characters are rendered in white on a black background in a single horizontal line.
"""

import argparse
import sys
from PIL import Image, ImageDraw, ImageFont


def generate_font_atlas(font_path, output_path, char_height, image_mode='1'):
    """
    Generate a font atlas image from a font file.
    
    Args:
        font_path (str): Path to the font file
        output_path (str): Path where the output image will be saved
        char_height (int): Height of characters in pixels
        image_mode (str): Image mode - '1' for 1-bit, 'L' for grayscale
    """
    # ASCII characters from 33 (!) to 126 (~)
    characters = [chr(i) for i in range(33, 127)]
    
    try:
        # Load the font
        font = ImageFont.truetype(font_path, char_height)
    except Exception as e:
        print(f"Error loading font '{font_path}': {e}")
        sys.exit(1)
    
    # Create a temporary image to measure text dimensions
    temp_img = Image.new(image_mode, (1, 1), color=0)  # 0 = black in both modes
    temp_draw = ImageDraw.Draw(temp_img)
    
    # Calculate the total width needed for all characters
    total_width = 0
    char_widths = []
    max_height = 0
    
    for char in characters:
        bbox = temp_draw.textbbox((0, 0), char, font=font, spacing=0)
        char_width = bbox[2] - bbox[0]
        char_height_actual = bbox[3] - bbox[1]
        
        char_widths.append(char_width)
        total_width += char_width
        max_height = max(max_height, char_height_actual)
    
    # Check that all characters have the same width (monospaced requirement)
    if len(set(char_widths)) > 1:
        unique_widths = sorted(set(char_widths))
        print(f"Error: Font is not monospaced. Character widths vary: {unique_widths}")
        print("Character width details:")
        for i, char in enumerate(characters):
            if char_widths[i] != char_widths[0]:  # Show only non-uniform widths
                print(f"  '{char}' (ASCII {ord(char)}): {char_widths[i]} pixels")
        print(f"Expected all characters to have the same width for a proper font atlas.")
        sys.exit(1)
    
    char_width = char_widths[0]  # All characters have the same width
    print(f"Font validation passed: All characters are {char_width} pixels wide")
    
    # Create the final image with calculated dimensions
    # Set colors based on image mode
    if image_mode == '1':
        bg_color = 0    # Black
        fg_color = 1    # White
    else:  # 'L' mode
        bg_color = 0    # Black
        fg_color = 255  # White
    
    atlas_img = Image.new(image_mode, (total_width, max_height), color=bg_color)
    draw = ImageDraw.Draw(atlas_img)
    
    # Draw each character
    x_offset = 0
    for i, char in enumerate(characters):
        # Draw the character in white
        draw.text((x_offset, -1), char, font=font, fill=fg_color, spacing=0)
        x_offset += char_widths[i]
    
    # Save the atlas image
    try:
        atlas_img.save(output_path)
        print(f"Font atlas saved to: {output_path}")
        print(f"Atlas dimensions: {total_width}x{max_height} pixels")
        print(f"Characters: {len(characters)} (ASCII 33-126)")
    except Exception as e:
        print(f"Error saving image to '{output_path}': {e}")
        sys.exit(1)


def analyze_font_sizes(font_path):
    """
    Analyze a font to find all sizes where it renders as truly monospaced.
    
    Args:
        font_path (str): Path to the font file
    """
    # ASCII characters from 33 (!) to 126 (~)
    characters = [chr(i) for i in range(33, 127)]
    
    print(f"Analyzing font: {font_path}")
    print("Scanning font sizes 6-32 pixels for monospaced compatibility...")
    print()
    
    monospaced_sizes = []
    
    for size in range(6, 33):
        try:
            # Load the font at this size
            font = ImageFont.truetype(font_path, size)
        except Exception as e:
            print(f"Warning: Could not load font at size {size}: {e}")
            continue
        
        # Create a temporary image to measure text dimensions
        temp_img = Image.new('RGB', (1, 1), color='black')
        temp_draw = ImageDraw.Draw(temp_img)
        
        # Measure all character widths
        char_widths = []
        max_height = 0
        
        for char in characters:
            bbox = temp_draw.textbbox((0, 0), char, font=font, spacing=0)
            char_width = bbox[2] - bbox[0]
            char_height_actual = bbox[3] - bbox[1]
            
            char_widths.append(char_width)
            max_height = max(max_height, char_height_actual)
        
        # Check if all characters have the same width
        unique_widths = set(char_widths)
        if len(unique_widths) == 1:
            char_width = char_widths[0]
            monospaced_sizes.append((size, char_width, max_height))
            print(f"✓ Size {size:2d}: {char_width:2d}x{max_height:2d} pixels (monospaced)")
        else:
            width_range = f"{min(unique_widths)}-{max(unique_widths)}"
            print(f"✗ Size {size:2d}: {width_range:>5}x{max_height:2d} pixels (variable width)")
    
    print()
    if monospaced_sizes:
        print("Summary of monospaced font sizes:")
        print("Font Size | Character Size | Atlas Dimensions")
        print("----------|----------------|------------------")
        for font_size, char_width, char_height in monospaced_sizes:
            total_width = char_width * len(characters)
            print(f"{font_size:8d}  | {char_width:6d}x{char_height:<6d} | {total_width:6d}x{char_height:<6d}")
    else:
        print("No monospaced font sizes found in the range 6-32 pixels.")
        print("This font may not be suitable for font atlas generation.")


def main():
    """Main function to parse arguments and execute the appropriate mode."""
    parser = argparse.ArgumentParser(
        description="Font Atlas Generator and Analyzer"
    )
    
    subparsers = parser.add_subparsers(dest='mode', help='Operating mode')
    subparsers.required = True
    
    # Generate mode
    generate_parser = subparsers.add_parser(
        'generate', 
        help='Generate a font atlas image'
    )
    generate_parser.add_argument(
        '--font-path',
        required=True,
        help='Path to the font file (TTF, OTF, etc.)'
    )
    generate_parser.add_argument(
        '--output-path',
        required=True,
        help='Path for the output image file'
    )
    generate_parser.add_argument(
        '--char-height',
        type=int,
        required=True,
        help='Height of characters in pixels'
    )
    generate_parser.add_argument(
        '--image-mode',
        choices=['1', 'L'],
        default='1',
        help='Image mode: "1" for 1-bit black/white, "L" for 8-bit grayscale (default: 1)'
    )
    
    # Analyze mode
    analyze_parser = subparsers.add_parser(
        'analyze',
        help='Analyze font to find monospaced sizes'
    )
    analyze_parser.add_argument(
        '--font-path',
        required=True,
        help='Path to the font file (TTF, OTF, etc.)'
    )
    
    args = parser.parse_args()
    
    if args.mode == 'generate':
        # Validate inputs
        if args.char_height <= 0:
            print("Error: Character height must be a positive integer")
            sys.exit(1)
        
        # Generate the font atlas
        generate_font_atlas(args.font_path, args.output_path, args.char_height, args.image_mode)
    
    elif args.mode == 'analyze':
        # Analyze the font for monospaced sizes
        analyze_font_sizes(args.font_path)


if __name__ == '__main__':
    main()