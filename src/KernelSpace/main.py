from config import DEVICE, FRAME_W, FRAME_H, FRAME_FORMAT
from reader import open_device, read_frame, close_device
from processor import process_frame
from display import init_display, show_frame, check_keys, close_display


def main():
    print(f"[INFO] Opening device: {DEVICE}")

    _
    device_handle = open_device(DEVICE, FRAME_W, FRAME_H)

    if device_handle is None:
        print("[ERROR] Could not open device")
        return

    init_display()
    print("[INFO] Stream running — press 'q' to quit | 'p' to toggle processing")

    show_processed = True

    while True:
        
        frame = read_frame(device_handle)

        if(frame is None):
            break

        output = process_frame(frame) if show_processed else frame

        show_frame(output, show_processed)

        key = check_keys()

        if(key == "quit"):        
            break
        if(key == "toggle"):       
            show_processed = not show_processed

    close_device(device_handle)
    close_display()


if __name__ == "__main__":
    main()