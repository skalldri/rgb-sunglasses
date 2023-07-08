import pywav
import argparse

def parse_args():
    parser = argparse.ArgumentParser(
                    prog='Raw PCM file to WAV',
                    description='Convert raw PCM data from devkit into WAV file',
                    epilog='LOL what a project')
    
    parser.add_argument('-i', '--input', required=True) 
    parser.add_argument('-o', '--output', required=True)

    return parser.parse_args()

def main():
    args = parse_args()

    pcm_bytes = bytearray()
    filecontents = None

    with open(args.input, "r") as input:
        filecontents = input.readlines()

    pcm_block_found = False
    
    for line in filecontents:
        line = line.strip() # Remove extra whitespace

        if pcm_block_found:
            if line.startswith("*** STOP"):
                break # No more PCM data to process

            # Hexdump produces fixed width lines that look like this:
            # 00000000: 00 00 00 00 00 00 ff ff  fd ff 3c 00 38 00 76 00 |........ ..<.8.v.|
            # Extract the 
            # 00 00 00 00 00 00 ff ff  fd ff 3c 00 38 00 76 00
            # part
            line = line[10:-19]

            # Patch the center of each line containing two spaces: '59 fd  5a fd'
            line = line.replace("  ", " ") 

            bs = bytes.fromhex(line)
            pcm_bytes.extend(bs)

        elif line.startswith("*** START"):
            pcm_block_found = True
    
    # for b in pcm_bytes:
    #     print("{:02X}".format(b))

    # first parameter is the file name to write the wave data
    # second parameter is the number of channels, the value can be 1 (mono) or 2 (stereo)
    # third parameter is the sample rate, 16000 samples per second
    # fourth paramaer is the bits per sample, 16 bits per sample
    # fifth parameter is the audio format, 1 means PCM with no compression.
    wave_write = pywav.WavWrite(args.output, 1, 16000, 16, 1)
    wave_write.write(pcm_bytes)
    wave_write.close()


if __name__ == "__main__":
    main()