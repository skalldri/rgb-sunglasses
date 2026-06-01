# C Byte Array LZ4 Compressor

This Python script extracts byte arrays from C source files, compresses them using LZ4, and writes the compressed data back to a new C file in the same format.

## Features

- Extracts C byte arrays with regex pattern matching
- Compresses data using LZ4 for efficient storage
- Maintains the same C array format (16 hex values per line)
- Comprehensive error handling
- Command-line interface with help documentation

## Requirements

- Python 3.6+
- `lz4` Python package

## Installation

Install the required dependency:

```bash
pip install lz4
```

## Usage

```bash
python compress_byte_array.py -i INPUT_FILE -v VARIABLE_NAME -o OUTPUT_FILE -n OUTPUT_VARIABLE
```

### Arguments

- `-i, --input`: Input C file containing the byte array to compress
- `-v, --variable`: Name of the input byte array variable to extract
- `-o, --output`: Output C file to write the compressed array
- `-n, --name`: Name of the output variable in the generated C file

### Examples

1. Basic usage:
```bash
python compress_byte_array.py -i input.c -v my_array -o compressed.c -n compressed_array
```

2. Real example with TPS25750 config:
```bash
python compress_byte_array.py \
    -i misc/tps25750/tps25750-config.c \
    -v tps25750x_lowRegion_i2c_array \
    -o compressed_tps25750.c \
    -n compressed_tps25750_data
```

## Input Format

The script expects C byte arrays in this format:

```c
const char array_name[] = {
0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 
0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};
```

## Output Format

The compressed data is written in the same format:

```c
const char compressed_array_name[] = {
0x04, 0x22, 0x4d, 0x18, 0x68, 0x40, 0x00, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x61, 
0x25, 0x00, 0x00, 0xff, 0x0a, 0x01, 0x00, 0xe0, 0xac, 0xfe, 0xff, 0xff, 0xff, 0x00, 0x10, 0x00
};
```

## Compression Results

For the example TPS25750 configuration:
- Original size: 14,592 bytes
- Compressed size: 9,592 bytes  
- Compression ratio: 65.73%
- Space savings: 34.27%

## Error Handling

The script provides clear error messages for common issues:
- Input file not found
- Variable name not found in input file
- Invalid hex data in array
- File write permissions

## Decompression

To decompress the data in your C code, you'll need an LZ4 decompression library. The compressed data can be decompressed using standard LZ4 decompression functions.

Example with lz4 library:
```c
#include <lz4.h>

// Decompress the data
char decompressed_data[ORIGINAL_SIZE];
int decompressed_size = LZ4_decompress_safe(
    compressed_array_name, 
    decompressed_data, 
    sizeof(compressed_array_name), 
    ORIGINAL_SIZE
);
```

## Testing

The project includes comprehensive tests using pytest:

### Running Tests

From the repository root:
```bash
# Run all tests
pytest

# Run with verbose output
pytest -v

# Run with detailed output showing print statements
pytest -v -s

# Run only the compression tests
pytest scripts/compress_byte_array/test_compression.py -v
```

### Test Coverage

The test suite includes:

1. **Round-trip compression test** - Verifies that data can be compressed and decompressed correctly
2. **Error handling tests** - Ensures proper error handling for invalid files and variables
3. **Compression efficiency test** - Verifies that compression actually reduces file size
4. **Integration test** - Tests the complete workflow from extraction to compressed output

All tests use relative paths and generate compressed files on-demand, so no pre-existing files are required.

### Manual Testing

You can also run the test script directly:
```bash
python scripts/compress_byte_array/test_compression.py
```

## Testing