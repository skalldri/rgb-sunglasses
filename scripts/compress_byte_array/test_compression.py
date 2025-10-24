#!/usr/bin/env python3

import pytest
import os
import sys
import tempfile
import re
import lz4.block
from pathlib import Path

# Add the current directory to the path so we can import the compression module
sys.path.insert(0, os.path.dirname(__file__))

# Import the compression functions directly
from compress_byte_array import extract_byte_array, compress_data, write_compressed_array


def test_compression_round_trip():
    """Test that compression and decompression work correctly with round-trip verification."""
    
    # Get the path relative to the repository root
    repo_root = Path(__file__).parent.parent.parent
    input_file = repo_root / "misc" / "tps25750" / "tps25750-config.c"
    variable_name = "tps25750x_lowRegion_i2c_array"
    
    # Verify the input file exists
    assert input_file.exists(), f"Input file not found: {input_file}"
    
    # Extract the original data
    original_data = extract_byte_array(str(input_file), variable_name)
    assert len(original_data) > 0, "No data extracted from input file"
    
    # Compress the data
    compressed_data = compress_data(original_data)
    assert len(compressed_data) > 0, "Compression failed"
    assert len(compressed_data) < len(original_data), "Compressed data should be smaller than original"
    
    # Create a temporary directory and base name for the compressed output
    with tempfile.TemporaryDirectory() as temp_dir:
        temp_output_base = os.path.join(temp_dir, "test_output")
        temp_c_file = f"{temp_output_base}.c"
        temp_h_file = f"{temp_output_base}.h"
        
        # Write compressed data to temporary files
        output_variable_name = "test_compressed_data"
        write_compressed_array(compressed_data, temp_output_base, output_variable_name, len(original_data))
        
        # Verify both output files were created and have content
        assert os.path.exists(temp_c_file), "Output .c file was not created"
        assert os.path.exists(temp_h_file), "Output .h file was not created"
        assert os.path.getsize(temp_c_file) > 0, "Output .c file is empty"
        assert os.path.getsize(temp_h_file) > 0, "Output .h file is empty"
        
        # Read the compressed data back from the .c file
        with open(temp_c_file, 'r') as f:
            compressed_content = f.read()
        
        # Extract hex values from the output file
        hex_pattern = r'0x([0-9a-fA-F]{2})'
        hex_matches_compressed = re.findall(hex_pattern, compressed_content)
        assert len(hex_matches_compressed) > 0, "No hex values found in output file"
        
        # Convert back to bytes
        read_compressed_data = bytes([int(hex_val, 16) for hex_val in hex_matches_compressed])
        
        # Verify the written compressed data matches what we compressed
        assert read_compressed_data == compressed_data, "Written compressed data doesn't match original compressed data"
        
        # Decompress the data
        decompressed_data = lz4.block.decompress(read_compressed_data, len(original_data))
        
        # Verify the decompressed data matches the original
        assert decompressed_data == original_data, "Decompressed data doesn't match original data"
        
        # Calculate and verify compression statistics
        compression_ratio = len(compressed_data) / len(original_data)
        assert compression_ratio < 1.0, "Compression ratio should be less than 1.0"
        
        print(f"✅ Compression test passed!")
        print(f"Original size: {len(original_data)} bytes")
        print(f"Compressed size: {len(compressed_data)} bytes")
        print(f"Compression ratio: {compression_ratio:.2%}")
        print(f"Space savings: {(1 - compression_ratio):.2%}")


def test_extract_byte_array_invalid_file():
    """Test that extract_byte_array handles invalid files correctly."""
    
    with pytest.raises(SystemExit):
        extract_byte_array("nonexistent_file.c", "test_array")


def test_extract_byte_array_invalid_variable():
    """Test that extract_byte_array handles invalid variable names correctly."""
    
    repo_root = Path(__file__).parent.parent.parent
    input_file = repo_root / "misc" / "tps25750" / "tps25750-config.c"
    
    # Verify the input file exists first
    assert input_file.exists(), f"Input file not found: {input_file}"
    
    with pytest.raises(SystemExit):
        extract_byte_array(str(input_file), "nonexistent_variable")


def test_compression_reduces_size():
    """Test that compression actually reduces the size of the data."""
    
    repo_root = Path(__file__).parent.parent.parent
    input_file = repo_root / "misc" / "tps25750" / "tps25750-config.c"
    variable_name = "tps25750x_lowRegion_i2c_array"
    
    # Verify the input file exists
    assert input_file.exists(), f"Input file not found: {input_file}"
    
    # Extract and compress the data
    original_data = extract_byte_array(str(input_file), variable_name)
    compressed_data = compress_data(original_data)
    
    # Verify compression reduces size
    assert len(compressed_data) < len(original_data), f"Compression didn't reduce size: {len(compressed_data)} >= {len(original_data)}"
    
    # Verify a reasonable compression ratio (should be better than 80%)
    compression_ratio = len(compressed_data) / len(original_data)
    assert compression_ratio < 0.8, f"Compression ratio too poor: {compression_ratio:.2%}"


if __name__ == "__main__":
    # Run the tests when executed directly
    test_compression_round_trip()
    test_compression_reduces_size()
    print("All tests passed!")