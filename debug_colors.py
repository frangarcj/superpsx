from PIL import Image
import sys

def analyze(path):
    img = Image.open(path).convert('RGB')
    pixels = img.load()
    w, h = img.size
    
    black = 0
    white = 0
    red = 0
    green = 0
    blue = 0
    other = 0
    
    other_colors = {}
    
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            if r < 10 and g < 10 and b < 10:
                black += 1
            elif r > 240 and g > 240 and b > 240:
                white += 1
            elif r > 200 and g < 50 and b < 50:
                red += 1
            elif g > 200 and r < 50 and b < 50:
                green += 1
            elif b > 200 and r < 50 and g < 50:
                blue += 1
            else:
                other += 1
                if (r,g,b) in other_colors:
                    other_colors[(r,g,b)] += 1
                else:
                    other_colors[(r,g,b)] = 1
                
    total = w * h
    print(f"File: {path}")
    print(f"Black: {black/total*100:.1f}%")
    print(f"White: {white/total*100:.1f}%")
    print(f"Red:   {red/total*100:.1f}%")
    print(f"Green: {green/total*100:.1f}%")
    print(f"Blue:  {blue/total*100:.1f}%")
    print(f"Other: {other/total*100:.1f}%")
    if other > 0:
        sorted_others = sorted(other_colors.items(), key=lambda item: item[1], reverse=True)
        print("Top 5 Other Colors:")
        for color, count in sorted_others[:5]:
             print(f"  {color}: {count} ({count/total*100:.2f}%)")

analyze(sys.argv[1])
if len(sys.argv) > 2:
    analyze(sys.argv[2])
