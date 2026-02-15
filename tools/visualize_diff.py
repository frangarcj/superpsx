from PIL import Image
import sys

def visualize(path):
    try:
        img = Image.open(path).convert('RGB')
        # Resize to terminal size (e.g. 80x40)
        w, h = img.size
        aspect = h / w
        cols = 80
        rows = int(cols * aspect * 0.5) # Char height ~ 2x width
        
        img_small = img.resize((cols, rows), Image.Resampling.NEAREST)
        pixels = img_small.load()
        
        print(f"ASCII Diff Visualization of {path}:")
        print("  " + "-" * cols)
        for y in range(rows):
            line = " |"
            for x in range(cols):
                r, g, b = pixels[x, y]
                # If pixel is NOT black (diff), print symbol
                if r > 20 or g > 20 or b > 20:
                    line += "#"
                else:
                    line += " "
            line += "|"
            print(line)
        print("  " + "-" * cols)

    except Exception as e:
        print(f"Visualization failed: {e}")

if __name__ == "__main__":
    visualize(sys.argv[1])
