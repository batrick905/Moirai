import cv2

def open_device(device: str, width:int, height:int):
    cap = cv2.VideoCapture(device)
    if not cap.isOpened():
        raise Exception("Could not open video device")
        return None
        pass

    cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
    cap.set(cv2.CAP_PROP_FPS, 30)

    print(f"Device {device} opened with resolution {width}x{height}")
    return cap         
    

def read_frame(handle):
    ret, frame = handle.read()
    if not ret:
        raise Exception("Could not read frame")
    return frame

def close_device(handle):
    handle.release()
    print("Device closed")


