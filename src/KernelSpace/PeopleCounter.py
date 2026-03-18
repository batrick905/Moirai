import cv2

# Replace camera capture with a static image
img = cv2.imread("NewCountTest.jpeg")

if img is None:
    print("Error: could not load image")
    exit()

hog = cv2.HOGDescriptor()
hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())

# Resize to help HOG find people at this scale
img = cv2.resize(img, (800, 600))

boxes, weights = hog.detectMultiScale(
    img,
    winStride=(4, 4),    # Smaller stride = more thorough scan
    padding=(8, 8),
    scale=1.03           # Finer scale steps = catches more sizes
)

print(f"People detected: {len(boxes)}")

for (x, y, w, h) in boxes:
    cv2.rectangle(img, (x, y), (x + w, y + h), (0, 255, 0), 2)

cv2.imwrite("output.jpg", img)
