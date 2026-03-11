import numpy as np
import cv2
import time

WINDOW_NAME = "Camera Userspace"

_fps_last_time = time.time()
_fps_frame_count = 0
_fps_current = 0

def init_display():
    cv2.namedWindow(WINDOW_NAME, cv2.WINDOW_NORMAL)
    print("Display initialized")

def display_frame(frame, processing_on: bool):
    display = frame.copy()
    _draw_overlay(display, processing_on)
    cv2.imshow(WINDOW_NAME, display)
    _update_fps()

def check_keys():
    key = cv2.waitKey(1) & 0xFF
    if key == ord('q'):
        return "quit"
    
    if key == ord('p'):
        return"toggle"

    return None

def close_display():
    cv2.destroyAllWindows()
    print("Display closed")

def _draw_overlay(frame, processing_on: bool):
    status_text = "Processing: ON" if processing_on else "Processing: OFF"
    cv2.putText(frame, status_text, (10, 28), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
    cv2.putText(frame, f"FPS: {_fps_current:.2f}", (10, 60), cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)

def _update_fps():
    global _fps_last_time, _fps_frame_count, _fps_current
    _fps_frame_count += 1

    if _fps_frame_count >= 30.0:
        _time_eslapsed = time.time() - _fps_last_time
        _fps_current =  30 / _time_eslapsed
        _fps_frame_count = 0
        _fps_last_time = time.time()
