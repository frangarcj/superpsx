
import sys
from PIL import Image
import os

def analyze_image(image_path):
    if not os.path.exists(image_path):
        print("Image not found")
        sys.exit(1)
        
    try:
        if image_path.endswith('.bin'):
            # Load raw VRAM dump (1024x512, 32-bit RGBA)
            # PS2 VRAM dump is likely BGRA or RGBA. Standard is RGBA (0xRRGGBBAA) or ABGR?
            # GS_PSM_32 stores as 32-bit.
            # Let's assume RGBA for now.
            with open(image_path, 'rb') as f:
                data = f.read()
            img = Image.frombytes('RGBA', (1024, 512), data)
            # Save for visual inspection
            dump_img_path = image_path + ".png"
            img.save(dump_img_path)
            print(f"converted dump to {dump_img_path}")
        else:
            img = Image.open(image_path)
    except Exception as e:
        print(f"Failed to open image: {e}")
        sys.exit(1)

    img = img.convert('RGB')
    width, height = img.size
    pixels = img.load()
    
    red_count = 0
    blue_count = 0
    white_count = 0
    other_count = 0
    
    # Sample pixels to be faster
    step = 10
    total_sampled = 0
    
    for y in range(0, height, step):
        for x in range(0, width, step):
            r, g, b = pixels[x, y]
            total_sampled += 1
            
            # Check for White (Background)
            if r > 240 and g > 240 and b > 240:
                white_count += 1
            # Check for Red (The Triangle)
            elif r > 200 and g < 50 and b < 50:
                red_count += 1
            # Check for Blue (Incorrect Swap)
            elif b > 200 and r < 50 and g < 50:
                blue_count += 1
            else:
                other_count += 1

    print(f"Analysis of {image_path}:")
    print(f"Total Sampled: {total_sampled}")
    print(f"White: {white_count} ({(white_count/total_sampled)*100:.1f}%)")
    print(f"Red: {red_count} ({(red_count/total_sampled)*100:.1f}%)")
    print(f"Blue: {blue_count} ({(blue_count/total_sampled)*100:.1f}%)")

    # Improved Shape Detection: Filter isolated noise
    # 1. Collect all "active" pixel coordinates
    active_pixels = []
    for y in range(0, height, step):
        for x in range(0, width, step):
            r, g, b = pixels[x, y]
            # Detect "emulator content" (Red, Green, Blue, or non-white/non-black if possible)
            # Actually, the emulator clears to White. The triangle is colored.
            # So anything NOT white is interesting.
            # But Desktop might be colored.
            # Let's focus on the Triangle colors (Red, Green, Blue, and mixes)
            # Check for significant color saturation?
            should_count = False
            if r > 100 and g < 100 and b < 100: should_count = True # Red-ish
            elif r < 100 and g > 100 and b < 100: should_count = True # Green-ish
            elif r < 100 and g < 100 and b > 100: should_count = True # Blue-ish
            # Gradient mixes:
            elif r > 50 and g > 50 and b < 100: should_count = True # Yellow?
            elif r > 50 and b > 50 and g < 100: should_count = True # Magenta?
            elif g > 50 and b > 50 and r < 100: should_count = True # Cyan?
            
            if should_count:
                active_pixels.append((x, y))

    if len(active_pixels) > 100:
        # 2. Find the main cluster (simple bounding box relative to median?)
        # Let's just take the bounding box of ALL active pixels and see if it helps.
        # If wallpaper is noisy, this might fail.
        # Better: Histogram/Grid density.
        
        xs = [p[0] for p in active_pixels]
        ys = [p[1] for p in active_pixels]
        
        # Filter outliers (simple percentile)
        xs.sort()
        ys.sort()
        trim = int(len(active_pixels) * 0.05) # Trim 5% outliers
        if trim > 0:
            xs = xs[trim:-trim]
            ys = ys[trim:-trim]
            
        if not xs or not ys:
             print("SHAPE: NO_CLUSTER_FOUND")
        else:
            min_x, max_x = xs[0], xs[-1]
            min_y, max_y = ys[0], ys[-1]
            
            bbox_w = max_x - min_x
            bbox_h = max_y - min_y
            bbox_area = bbox_w * bbox_h
            
            # Count pixels ACTUALLY in this bbox (from original set)
            count_in_bbox = 0
            for px, py in active_pixels:
                if min_x <= px <= max_x and min_y <= py <= max_y:
                    count_in_bbox += 1
            
            fill_ratio = (count_in_bbox * (step*step)) / (bbox_area + 1)
            
            print(f"Cluster BBox: ({min_x},{min_y}) to ({max_x},{max_y})")
            print(f"BBox Size: {bbox_w}x{bbox_h}")
            print(f"Fill Ratio: {fill_ratio:.2f}") # Triangle ~ 0.5, Rect ~ 1.0
            
            if fill_ratio > 0.8:
                print("SHAPE: RECTANGLE")
            elif 0.3 < fill_ratio < 0.7:
                print("SHAPE: TRIANGLE")
            else:
                print("SHAPE: AMBIGUOUS")
    else:
        print("SHAPE: NOT_ENOUGH_PIXELS")

    if red_count > blue_count and red_count > (total_sampled * 0.01):
        print("RESULT: RED_DETECTED")
    elif blue_count > red_count and blue_count > (total_sampled * 0.01):
        print("RESULT: BLUE_DETECTED")
    else:
        print("RESULT: INCONCLUSIVE")

def compare_images(img1_path, img2_path):
    try:
        # Handle bin files for img1
        if img1_path.endswith('.bin'):
            with open(img1_path, 'rb') as f:
                data = f.read()
            img1 = Image.frombytes('RGBA', (1024, 512), data)
        else:
            img1 = Image.open(img1_path)

        img2 = Image.open(img2_path)
        
        img1 = img1.convert('RGB')
        img2 = img2.convert('RGB')
        
        if img1.size != img2.size:
            print(f"Size mismatch: {img1.size} vs {img2.size}")
            # Resize img2 to match img1 (VRAM dump is always 1024x512)
            # The reference png might be smaller or different.
            img2 = img2.resize(img1.size)
            print(f"Resized reference to {img1.size}")

        # Simple pixel diff
        diff_pixels = 0
        total_pixels = img1.width * img1.height
        
        pixels1 = img1.load()
        pixels2 = img2.load()
        
        for y in range(img1.height):
            for x in range(img1.width):
                r1, g1, b1 = pixels1[x, y]
                r2, g2, b2 = pixels2[x, y]
                
                # Tolerant comparison (rendering differences)
                if abs(r1 - r2) > 10 or abs(g1 - g2) > 10 or abs(b1 - b2) > 10:
                    diff_pixels += 1
        
        match_rate = ((total_pixels - diff_pixels) / total_pixels) * 100
        print(f"COMPARISON RESULT:")
        print(f"Match Rate: {match_rate:.2f}%")
        print(f"Different Pixels: {diff_pixels}")
        
        if match_rate > 95.0:
            print("PASS: Images are practically identical.")
        else:
            print("FAIL: Images differ significantly.")
            # Create diff image
            diff_img = Image.new('RGB', img1.size)
            diff_pixels = diff_img.load()
            for y in range(img1.height):
                for x in range(img1.width):
                    r1, g1, b1 = pixels1[x, y]
                    r2, g2, b2 = pixels2[x, y]
                    if abs(r1 - r2) > 10 or abs(g1 - g2) > 10 or abs(b1 - b2) > 10:
                        diff_pixels[x, y] = (255, 0, 0) # Highlight difference in Red
                    else:
                        diff_pixels[x, y] = (0, 0, 0) # Black for match
            
            diff_path = img1_path + ".diff.png"
            diff_img.save(diff_path)
            print(f"Saved difference image to {diff_path}")

    except Exception as e:
        print(f"Comparison failed: {e}")
        sys.exit(1)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python analyze.py <image> [reference_image]")
        sys.exit(1)
    
    if len(sys.argv) == 3:
        compare_images(sys.argv[1], sys.argv[2])
    else:
        analyze_image(sys.argv[1])
