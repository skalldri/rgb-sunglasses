#!/usr/bin/env python3

import argparse
import re
import lz4.block
import os
import sys

def extract_byte_array(file_path, variable_name):
    """Extract byte array data from a C file."""
    try:
        with open(file_path, 'r') as file:
            content = file.read()
    except FileNotFoundError:
        print(f"Error: Input file '{file_path}' not found.")
        sys.exit(1)
    
    # Pattern to match C byte array declaration
    # Matches: const char variable_name[] = { ... }; or const unsigned char variable_name[] = { ... };
    escaped_var_name = re.escape(variable_name)
    pattern = rf'const\s+(?:unsigned\s+)?char\s+{escaped_var_name}\s*\[\s*\]\s*=\s*\{{([^}}]+)\}}\s*;'
    
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        print(f"Error: Could not find byte array '{variable_name}' in file '{file_path}'.")
        sys.exit(1)
    
    array_content = match.group(1)
    
    # Extract hex values using regex
    hex_pattern = r'0x([0-9a-fA-F]{2})'
    hex_matches = re.findall(hex_pattern, array_content)
    
    if not hex_matches:
        print(f"Error: No hex values found in array '{variable_name}'.")
        sys.exit(1)
    
    # Convert hex strings to bytes
    byte_data = bytes([int(hex_val, 16) for hex_val in hex_matches])
    
    print(f"Extracted {len(byte_data)} bytes from '{variable_name}'")
    return byte_data

def compress_data(data):
    """Compress data using LZ4."""
    try:
        compressed = lz4.block.compress(data, store_size=False)
        print(f"Compressed {len(data)} bytes to {len(compressed)} bytes (ratio: {len(compressed)/len(data):.2%})")
        return compressed
    except Exception as e:
        print(f"Error compressing data: {e}")
        sys.exit(1)

def write_compressed_array(compressed_data, output_base, output_variable_name, original_size):
    """Write compressed data to both .c and .h files."""
    try:
        # Generate file names from base name
        c_file = f"{output_base}.c"
        h_file = f"{output_base}.h"
        
        # Generate variable names with new naming convention
        compressed_size_variable = f"g_{output_variable_name}_CompressedSize"
        uncompressed_size_variable = f"g_{output_variable_name}_UncompressedSize"

        # Write the .c file with definitions
        with open(c_file, 'w') as file:
            # Write the array definition
            file.write(f"const char {output_variable_name}[] = {{\n")
            
            # Write the compressed data as hex values, 16 per line
            for i in range(0, len(compressed_data), 16):
                chunk = compressed_data[i:i+16]
                hex_values = [f"0x{byte:02x}" for byte in chunk]
                line = ", ".join(hex_values)
                
                # Add trailing comma if not the last line
                if i + 16 < len(compressed_data):
                    line += ", "
                
                file.write(f"{line}\n")
            
            file.write("};\n\n")
            
            # Write the size variables
            file.write(f"const unsigned int {compressed_size_variable} = {len(compressed_data)};\n")
            file.write(f"const unsigned int {uncompressed_size_variable} = {original_size};\n")

        # Write the .h file with declarations
        with open(h_file, 'w') as file:
            # Write header guard
            guard_name = f"{output_variable_name.upper()}_H"
            file.write(f"#ifndef {guard_name}\n")
            file.write(f"#define {guard_name}\n\n")
            
            # Write #define constants for the sizes
            compressed_size_define = f"{output_variable_name.upper()}_COMPRESSED_SIZE"
            uncompressed_size_define = f"{output_variable_name.upper()}_UNCOMPRESSED_SIZE"
            file.write(f"#define {compressed_size_define} {len(compressed_data)}\n")
            file.write(f"#define {uncompressed_size_define} {original_size}\n\n")
            
            # Write extern declarations
            file.write(f"extern const char {output_variable_name}[];\n")
            file.write(f"extern const unsigned int {compressed_size_variable};\n")
            file.write(f"extern const unsigned int {uncompressed_size_variable};\n\n")
            
            # Close header guard
            file.write(f"#endif // {guard_name}\n")
            
        print(f"Written compressed array '{output_variable_name}' to '{c_file}' and '{h_file}'")
        print(f"Array size: {len(compressed_data)} bytes")
        print(f"Compressed size variable: '{compressed_size_variable}'")
        print(f"Uncompressed size variable: '{uncompressed_size_variable}'")
        
    except Exception as e:
        print(f"Error writing output files: {e}")
        sys.exit(1)

def main():
    parser = argparse.ArgumentParser(
        description="Extract a byte array from a C file, compress it with LZ4, and write it to .c and .h files.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -i input.c -v input_array -o output -n compressed_array
  %(prog)s --input config.c --variable tps25750x_lowRegion_i2c_array --output compressed_config --name compressed_data
        """
    )
    
    parser.add_argument(
        '-i', '--input',
        required=True,
        help='Input C file containing the byte array to compress'
    )
    
    parser.add_argument(
        '-v', '--variable',
        required=True,
        help='Name of the input byte array variable to extract'
    )
    
    parser.add_argument(
        '-o', '--output',
        required=True,
        help='Output base name for .c and .h files (extensions will be added automatically)'
    )
    
    parser.add_argument(
        '-n', '--name',
        required=True,
        help='Name of the output variable in the generated C file'
    )
    
    args = parser.parse_args()
    
    # Validate input file exists
    if not os.path.isfile(args.input):
        print(f"Error: Input file '{args.input}' does not exist.")
        sys.exit(1)
    
    # Create output directory if it doesn't exist
    output_dir = os.path.dirname(args.output)
    if output_dir and not os.path.exists(output_dir):
        os.makedirs(output_dir)
    
    # Extract byte array from input file
    print(f"Extracting byte array '{args.variable}' from '{args.input}'...")
    original_data = extract_byte_array(args.input, args.variable)
    
    # Compress the data
    print("Compressing data with LZ4...")
    compressed_data = compress_data(original_data)
    
    # Write compressed data to output files
    print(f"Writing compressed array to '{args.output}.c' and '{args.output}.h'...")
    write_compressed_array(compressed_data, args.output, args.name, len(original_data))
    
    print("Compression complete!")

if __name__ == "__main__":
    main()