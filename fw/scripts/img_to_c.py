from PIL import Image, ImageSequence, GifImagePlugin
import argparse

def parse_args():
    parser = argparse.ArgumentParser(
                    prog='Image (Sequence) to C file',
                    description='Convert image sequence to a C file',
                    epilog='LOL what a project')
    
    parser.add_argument('-i', '--input', required=True)
    parser.add_argument('-o', '--output', required=True)
    parser.add_argument('--resize', action='store_true', default=False, help="Resize to 40x12 for glasses")

    return parser.parse_args()

display_width = 40
display_height = 12

def main():
    args = parse_args()

    img = Image.open(args.input)

    print(f"Dims: {img.width}x{img.height}")

    for frame_num, frame in enumerate(ImageSequence.Iterator(img)):
        if args.resize:
            frame.resize((display_width, display_height), resample=Image.Resampling.NEAREST)

        frame = frame.convert("RGB")
        

if __name__ == "__main__":
    main()