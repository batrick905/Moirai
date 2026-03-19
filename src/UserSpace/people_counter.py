"""
people_counter.py
Multi-process people counter using the IMX708 custom LKM.

Process 1 (main)    — opens /dev/imx708_ctrl, sends controls via write(),
                      reads stats via read() (blocking), reads /proc file
Process 2 (capture) — takes photo via rpicam-still (goes through your LKM)
Process 3 (counter) — receives image, counts people with HOG detector
"""

import multiprocessing
import subprocess
import os
import time
import cv2
import sys

CTRL_DEV   = "/dev/imx708_ctrl"
PHOTO_PATH = "/tmp/imx708_capture.jpg"
OUT_PATH   = "/tmp/imx708_detected.jpg"

# ── Process 2: capture ────────────────────────────────────────────────────────
def capture_process(ready_event):
    """
    Takes a single photo using rpicam-still.
    rpicam-still drives the sensor through your imx708_custom.ko LKM.
    Sets ready_event when the photo is saved.
    """
    print(f"[capture] PID {os.getpid()} — taking photo via IMX708 LKM")

    ret = subprocess.run([
        "rpicam-still",
        "-o", PHOTO_PATH,
        "--width",  "1536",
        "--height", "864",
        "--nopreview",
        "-t", "1"
    ], capture_output=True)

    if ret.returncode == 0 and os.path.exists(PHOTO_PATH):
        print(f"[capture] photo saved to {PHOTO_PATH}")
        ready_event.set()
    else:
        print(f"[capture] ERROR: rpicam-still failed")
        print(ret.stderr.decode())
        ready_event.set()  # unblock counter even on failure

# ── Process 3: count people ───────────────────────────────────────────────────
def counter_process(ready_event, result_queue):
    """
    Waits for capture to finish (blocks on ready_event),
    then runs HOG people detection on the saved photo.
    """
    print(f"[counter] PID {os.getpid()} — waiting for photo")

    ready_event.wait()  # BLOCKS until capture process signals

    if not os.path.exists(PHOTO_PATH):
        print("[counter] no photo found")
        result_queue.put(0)
        return

    img = cv2.imread(PHOTO_PATH)
    if img is None:
        print("[counter] could not read image")
        result_queue.put(0)
        return

    hog = cv2.HOGDescriptor()
    hog.setSVMDetector(cv2.HOGDescriptor_getDefaultPeopleDetector())

    resized = cv2.resize(img, (800, 600))
    boxes, _ = hog.detectMultiScale(
        resized, winStride=(4, 4), padding=(8, 8), scale=1.03
    )

    # draw boxes on detected people
    for (x, y, w, h) in boxes:
        cv2.rectangle(resized, (x, y), (x + w, y + h), (0, 255, 0), 2)

    cv2.imwrite(OUT_PATH, resized)
    print(f"[counter] saved annotated image to {OUT_PATH}")

    result_queue.put(len(boxes))

# ── Process 1: main ───────────────────────────────────────────────────────────
def main():
    print(f"[main] PID {os.getpid()} — IMX708 people counter")
    print(f"[main] using LKM interface: {CTRL_DEV}")

    # check driver is loaded
    if not os.path.exists(CTRL_DEV):
        print(f"ERROR: {CTRL_DEV} not found")
        print("Load the driver first: sudo insmod imx708_custom.ko")
        sys.exit(1)

    # open char device
    fd = os.open(CTRL_DEV, os.O_RDWR)
    print(f"[main] open({CTRL_DEV}) OK")

    # write: set exposure
    os.write(fd, b"exposure=500\n")
    print(f"[main] write() — set exposure=500")

    # write: set gain
    os.write(fd, b"gain=100\n")
    print(f"[main] write() — set gain=100")

    # read /proc stats (non-blocking)
    with open("/proc/imx708_custom_stats", "r") as f:
        stats = f.read()
    print(f"\n── /proc/imx708_custom_stats ──\n{stats}")

    # read from char device (blocking — waits for driver response)
    print("[main] read(/dev/imx708_ctrl) — blocking...")
    data = os.read(fd, 256)
    print(f"[main] read() returned:\n{data.decode(errors='replace')}")

    # start capture and counter as separate processes
    ready_event  = multiprocessing.Event()
    result_queue = multiprocessing.Queue()

    p_capture = multiprocessing.Process(
        target=capture_process,
        args=(ready_event,),
        daemon=True
    )
    p_counter = multiprocessing.Process(
        target=counter_process,
        args=(ready_event, result_queue),
        daemon=True
    )

    p_capture.start()
    p_counter.start()
    print(f"\n[main] capture PID={p_capture.pid}")
    print(f"[main] counter PID={p_counter.pid}")

    p_capture.join()
    p_counter.join()

    count = result_queue.get()
    print(f"\nThere are {count} people here")

    os.close(fd)

if __name__ == "__main__":
    main()