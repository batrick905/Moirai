import cv2
from ultralytics import YOLO

# ── capture from IMX708 via your custom driver ────────────────
cap = cv2.VideoCapture("/dev/video0", cv2.CAP_V4L2)
cap.set(cv2.CAP_PROP_FRAME_WIDTH,  2304)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1296)

ret, img = cap.read()
cap.release()

if not ret:
    print("Error: could not capture from /dev/video0")
    exit()

cv2.imwrite("capture.jpg", img)
print("captured frame from IMX708")

# ── count people ──────────────────────────────────────────────
model = YOLO("yolov8n.pt")
results = model(img)
people = [b for b in results[0].boxes if int(b.cls) == 0]

print(f"People detected: {len(people)}")

annotated = results[0].plot()
cv2.imwrite("output.jpg", annotated)
print("saved output.jpg")