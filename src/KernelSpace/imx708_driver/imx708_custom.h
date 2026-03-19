#ifndef IMX708_CUSTOM_H
#define IMX708_CUSTOM_H

#include <linux/i2c.h>
#include <linux/clk.h>
#include <linux/cdev.h>
#include <linux/wait.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>

/* ── Chip identity ── */
#define IMX708_CHIP_ID              0x0708
#define IMX708_REG_CHIP_ID          0x0016

/* ── Streaming ── */
#define IMX708_REG_MODE_SELECT      0x0100
#define IMX708_MODE_STANDBY         0x00
#define IMX708_MODE_STREAMING       0x01

/* ── Exposure ── */
#define IMX708_REG_EXPOSURE_HI      0x0202
#define IMX708_REG_EXPOSURE_LO      0x0203
#define IMX708_EXPOSURE_MIN         1
#define IMX708_EXPOSURE_STEP        1

/* ── Analogue gain ── */
#define IMX708_REG_AGAIN_HI         0x0204
#define IMX708_REG_AGAIN_LO         0x0205
#define IMX708_AGAIN_MIN            0
#define IMX708_AGAIN_MAX            978
#define IMX708_AGAIN_STEP           1
#define IMX708_AGAIN_DEFAULT        0

/* ── Digital gain ── */
#define IMX708_REG_DGAIN_HI         0x020E
#define IMX708_REG_DGAIN_LO         0x020F
#define IMX708_DGAIN_MIN            256
#define IMX708_DGAIN_MAX            4095
#define IMX708_DGAIN_DEFAULT        256

/* ── Frame/line length ── */
#define IMX708_REG_FRAME_LEN_HI     0x0340
#define IMX708_REG_FRAME_LEN_LO     0x0341
#define IMX708_REG_LINE_LEN_HI      0x0342
#define IMX708_REG_LINE_LEN_LO      0x0343

/* ── Flip ── */
#define IMX708_REG_ORIENTATION      0x0101

/* ── Test pattern ── */
#define IMX708_REG_TEST_PATTERN     0x0600

/* ── Driver identity ── */
#define IMX708_DRIVER_NAME          "imx708-custom"
#define IMX708_PROC_NAME            "imx708_custom_stats"
#define IMX708_CDEV_NAME            "imx708_ctrl"

/* ── ioctl commands for /dev/imx708_ctrl ── */
#define IMX708_IOC_MAGIC            'I'
#define IMX708_IOC_GET_CHIP_ID      _IOR(IMX708_IOC_MAGIC, 1, unsigned int)
#define IMX708_IOC_GET_STREAMING    _IOR(IMX708_IOC_MAGIC, 2, unsigned int)
#define IMX708_IOC_SET_EXPOSURE     _IOW(IMX708_IOC_MAGIC, 3, unsigned int)
#define IMX708_IOC_SET_GAIN         _IOW(IMX708_IOC_MAGIC, 4, unsigned int)
#define IMX708_IOC_GET_STATS        _IOR(IMX708_IOC_MAGIC, 5, unsigned int)

struct imx708_mode {
	u32 width, height;
	u32 line_length;
	u32 frame_length;
	u32 vblank_min;
	u64 link_freq;
	const struct reg_sequence *regs;
	u32 num_regs;
};

struct imx708_dev {
	struct i2c_client           *client;
	struct v4l2_subdev           sd;
	struct media_pad             pad;
	struct v4l2_ctrl_handler     ctrl_handler;
	struct v4l2_mbus_framefmt    fmt;
	struct mutex                 lock;

	/* power */
	struct regulator            *vana;
	struct regulator            *vdig;
	struct regulator            *vddl;
	struct clk                  *xclk;

	/* V4L2 controls */
	struct v4l2_ctrl            *exposure;
	struct v4l2_ctrl            *again;
	struct v4l2_ctrl            *dgain;
	struct v4l2_ctrl            *vblank;
	struct v4l2_ctrl            *hflip;
	struct v4l2_ctrl            *vflip;

	const struct imx708_mode    *cur_mode;
	bool                         streaming;
	u64                          frame_count;

	/* /proc entry */
	struct proc_dir_entry       *proc_entry;

	/*
	 * Char device — /dev/imx708_ctrl
	 * Userspace can open/read/write/ioctl this directly.
	 * read()  blocks on wait_queue until frame_count increments.
	 * write() accepts "exposure=N" / "gain=N" commands.
	 */
	struct cdev                  cdev;
	struct class                *cdev_class;
	dev_t                        cdev_num;
	wait_queue_head_t            wait_queue;
	u64                          last_frame_seen;  /* for blocking read */
	bool                         write_busy;       /* for blocking write */
	wait_queue_head_t            write_queue;
};

#endif /* IMX708_CUSTOM_H */