import numpy as np
import cv2

def process_frame(frame):
    # Placeholder for frame processing logic
    return _placeholder(frame)

def _placeholder(frame):
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    edges = cv2.Canny(gray, 50, 150)
    edges_bgr = cv2.cvtColor(edges, cv2.COLOR_GRAY2BGR)
    return np.hstack([frame, edges_bgr])