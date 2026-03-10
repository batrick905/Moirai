# ════ INSPECT ════════════════════════════════════════

# full media pipeline graph
media-ctl -p

# list all controls your driver exposes
v4l2-ctl -d /dev/video0 --list-ctrls

# list supported formats and resolutions
v4l2-ctl -d /dev/video0 --list-formats-ext

# run compliance tests against your driver
v4l2-compliance -d /dev/video0

# read proc stats
cat /proc/imx708_stats


# ════ CONTROLS ═══════════════════════════════════════

# set exposure (in lines)
v4l2-ctl -d /dev/video0 --set-ctrl=exposure=500

# set analogue gain
v4l2-ctl -d /dev/video0 --set-ctrl=analogue_gain=100

# flip image
v4l2-ctl -d /dev/video0 --set-ctrl=hflip=1
v4l2-ctl -d /dev/video0 --set-ctrl=vflip=1

# enable test pattern (no lens needed to verify streaming works)
v4l2-ctl -d /dev/video0 --set-ctrl=test_pattern=2


# ════ CAPTURE RAW FRAMES ═════════════════════════════

# single raw bayer frame at 1080p
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=2304,height=1296,pixelformat=RG10 \
  --stream-mmap \
  --stream-to=frame.raw \
  --stream-count=1

# 10 frames at full res
v4l2-ctl -d /dev/video0 \
  --set-fmt-video=width=4608,height=2592,pixelformat=RG10 \
  --stream-mmap \
  --stream-to=frames.raw \
  --stream-count=10


# ════ TAKE JPEG PHOTOS ═══════════════════════════════

# libcamera (handles ISP debayer → JPEG for you)
libcamera-jpeg -o photo.jpg

# full res
libcamera-jpeg -o photo.jpg --width 4608 --height 2592

# with exposure override
libcamera-jpeg -o photo.jpg --shutter 10000 --gain 2.0


# ════ RECORD VIDEO ═══════════════════════════════════

# H.264 video via libcamera
libcamera-vid -o video.h264 --width 2304 --height 1296 \
              --framerate 30 -t 10000   # 10 seconds

# stream to VLC over network
libcamera-vid -t 0 --inline -o - | \
  nc -l 8080 &
# on another machine:
# vlc tcp/h264://raspberrypi.local:8080


# ════ LIVE PREVIEW ═══════════════════════════════════

libcamera-hello --width 2304 --height 1296 -t 0   # -t 0 = forever


# ════ PYTHON / OPENCV ════════════════════════════════

python3 - <<'EOF'
import cv2
cap = cv2.VideoCapture("/dev/video0", cv2.CAP_V4L2)
cap.set(cv2.CAP_PROP_FRAME_WIDTH,  2304)
cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 1296)
ret, frame = cap.read()
if ret:
    cv2.imwrite("capture.jpg", frame)
    print("saved capture.jpg")
cap.release()
EOF
```

---

### How Everything Connects
```
imx708.c  ──builds──►  imx708.ko
                            │
                    sudo insmod imx708.ko
                            │
                     /dev/video0  ◄─── libcamera-jpeg -o photo.jpg
                     /dev/video0  ◄─── cv2.VideoCapture("/dev/video0")
                     /dev/video0  ◄─── v4l2-ctl --stream-mmap
                  /proc/imx708_stats ◄─ cat /proc/imx708_stats