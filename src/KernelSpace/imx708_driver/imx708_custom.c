// SPDX-License-Identifier: GPL-2.0
/*
 * IMX708 Custom Camera Sensor Driver
 * Sony IMX708 12.3MP stacked CMOS sensor
 *
 * Exposes two interfaces:
 *   /dev/v4l-subdev0  — V4L2 subdev (sensor config, streaming)
 *   /dev/imx708_ctrl  — char device  (blocking read/write/ioctl)
 *   /proc/imx708_custom_stats — stats
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include "imx708_custom.h"

/* ════════════════════════════════════════════════════════════
   Direct I2C helpers
   ════════════════════════════════════════════════════════════ */

static int imx708_write_reg(struct imx708_dev *sensor, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xFF, val };
	int ret = i2c_master_send(sensor->client, buf, 3);
	if (ret < 0) {
		dev_err(&sensor->client->dev,
			"i2c write 0x%04x = 0x%02x failed: %d\n",
			reg, val, ret);
		return ret;
	}
	return 0;
}

static int imx708_read_reg(struct imx708_dev *sensor, u16 reg, u8 *val)
{
	u8 addr[2] = { reg >> 8, reg & 0xFF };
	int ret;

	ret = i2c_master_send(sensor->client, addr, 2);
	if (ret < 0) {
		dev_err(&sensor->client->dev,
			"i2c read addr 0x%04x failed: %d\n", reg, ret);
		return ret;
	}
	ret = i2c_master_recv(sensor->client, val, 1);
	if (ret < 0) {
		dev_err(&sensor->client->dev,
			"i2c read data 0x%04x failed: %d\n", reg, ret);
		return ret;
	}
	return 0;
}

static int imx708_update_bits(struct imx708_dev *sensor,
			       u16 reg, u8 mask, u8 val)
{
	u8 current_val;
	int ret = imx708_read_reg(sensor, reg, &current_val);
	if (ret)
		return ret;
	current_val = (current_val & ~mask) | (val & mask);
	return imx708_write_reg(sensor, reg, current_val);
}

static int imx708_write_regs(struct imx708_dev *sensor,
			      const struct reg_sequence *regs, u32 n)
{
	u32 i;
	int ret;
	for (i = 0; i < n; i++) {
		ret = imx708_write_reg(sensor, regs[i].reg, regs[i].def);
		if (ret)
			return ret;
	}
	return 0;
}

/* ════════════════════════════════════════════════════════════
   Register sequences for each mode
   ════════════════════════════════════════════════════════════ */

static const struct reg_sequence mode_4608x2592_regs[] = {
	{ 0x0136, 0x18 }, { 0x0137, 0x00 },
	{ 0x3C7E, 0x01 }, { 0x3C7F, 0x07 },
	{ 0x0300, 0x01 }, { 0x0301, 0x00 },
	{ 0x0302, 0x20 }, { 0x0303, 0x00 },
	{ 0x0305, 0x04 }, { 0x0306, 0x01 },
	{ 0x0307, 0x30 },
	{ 0x0340, 0x0A }, { 0x0341, 0xF0 },
	{ 0x0342, 0x4E }, { 0x0343, 0x20 },
	{ 0x0344, 0x00 }, { 0x0345, 0x00 },
	{ 0x0346, 0x00 }, { 0x0347, 0x00 },
	{ 0x0348, 0x11 }, { 0x0349, 0xFF },
	{ 0x034A, 0x0A }, { 0x034B, 0x1F },
	{ 0x0408, 0x00 }, { 0x0409, 0x00 },
	{ 0x040A, 0x00 }, { 0x040B, 0x00 },
	{ 0x040C, 0x12 }, { 0x040D, 0x00 },
	{ 0x040E, 0x0A }, { 0x040F, 0x20 },
	{ 0x0820, 0x0F }, { 0x0821, 0xA0 },
	{ 0x0822, 0x00 }, { 0x0823, 0x00 },
};

static const struct reg_sequence mode_2304x1296_regs[] = {
	{ 0x0136, 0x18 }, { 0x0137, 0x00 },
	{ 0x0340, 0x05 }, { 0x0341, 0x54 },
	{ 0x0342, 0x27 }, { 0x0343, 0x10 },
	{ 0x0344, 0x00 }, { 0x0345, 0x00 },
	{ 0x0346, 0x00 }, { 0x0347, 0x00 },
	{ 0x0348, 0x11 }, { 0x0349, 0xFF },
	{ 0x034A, 0x0A }, { 0x034B, 0x1F },
	{ 0x0380, 0x00 }, { 0x0381, 0x02 },
	{ 0x0382, 0x00 }, { 0x0383, 0x00 },
	{ 0x0384, 0x00 }, { 0x0385, 0x02 },
	{ 0x0386, 0x00 }, { 0x0387, 0x00 },
	{ 0x0900, 0x01 }, { 0x0901, 0x22 },
	{ 0x0820, 0x0F }, { 0x0821, 0xA0 },
	{ 0x0822, 0x00 }, { 0x0823, 0x00 },
};

static const struct imx708_mode supported_modes[] = {
	{
		.width        = 4608,
		.height       = 2592,
		.line_length  = 19984,
		.frame_length = 2800,
		.vblank_min   = 58,
		.link_freq    = 450000000ULL,
		.regs         = mode_4608x2592_regs,
		.num_regs     = ARRAY_SIZE(mode_4608x2592_regs),
	},
	{
		.width        = 2304,
		.height       = 1296,
		.line_length  = 10000,
		.frame_length = 1364,
		.vblank_min   = 22,
		.link_freq    = 450000000ULL,
		.regs         = mode_2304x1296_regs,
		.num_regs     = ARRAY_SIZE(mode_2304x1296_regs),
	},
};

/* ════════════════════════════════════════════════════════════
   /proc stats
   ════════════════════════════════════════════════════════════ */

static int imx708_proc_show(struct seq_file *m, void *v)
{
	struct imx708_dev *sensor = m->private;

	mutex_lock(&sensor->lock);
	seq_printf(m, "frames_captured : %llu\n", sensor->frame_count);
	seq_printf(m, "streaming       : %d\n",   sensor->streaming);
	seq_printf(m, "width           : %u\n",   sensor->cur_mode->width);
	seq_printf(m, "height          : %u\n",   sensor->cur_mode->height);
	seq_printf(m, "exposure        : %d\n",   sensor->exposure->val);
	seq_printf(m, "analogue_gain   : %d\n",   sensor->again->val);
	seq_printf(m, "digital_gain    : %d\n",   sensor->dgain->val);
	mutex_unlock(&sensor->lock);

	return 0;
}

static int imx708_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, imx708_proc_show, pde_data(inode));
}

static const struct proc_ops imx708_proc_ops = {
	.proc_open    = imx708_proc_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ════════════════════════════════════════════════════════════
   Char device — /dev/imx708_ctrl
   ════════════════════════════════════════════════════════════ */

static int imx708_cdev_open(struct inode *inode, struct file *file)
{
	struct imx708_dev *sensor =
		container_of(inode->i_cdev, struct imx708_dev, cdev);
	file->private_data = sensor;
	dev_info(&sensor->client->dev, "imx708_ctrl opened\n");
	return 0;
}

static int imx708_cdev_release(struct inode *inode, struct file *file)
{
	struct imx708_dev *sensor = file->private_data;
	dev_info(&sensor->client->dev, "imx708_ctrl closed\n");
	return 0;
}

/*
 * read() — blocks until a new frame arrives (frame_count increments),
 * then returns current stats as a text string.
 *
 * If sensor is not streaming, returns stats immediately without blocking.
 */
static ssize_t imx708_cdev_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct imx708_dev *sensor = file->private_data;
	char stats[256];
	int len;
	int ret;

	/* block until frame_count changes from what we last saw */
	if (sensor->streaming) {
		ret = wait_event_interruptible(sensor->wait_queue,
			sensor->frame_count != sensor->last_frame_seen);
		if (ret)
			return -ERESTARTSYS;
	}

	mutex_lock(&sensor->lock);
	sensor->last_frame_seen = sensor->frame_count;
	len = snprintf(stats, sizeof(stats),
		"frames_captured : %llu\n"
		"streaming       : %d\n"
		"width           : %u\n"
		"height          : %u\n"
		"exposure        : %d\n"
		"analogue_gain   : %d\n"
		"digital_gain    : %d\n",
		sensor->frame_count,
		sensor->streaming,
		sensor->cur_mode->width,
		sensor->cur_mode->height,
		sensor->exposure->val,
		sensor->again->val,
		sensor->dgain->val);
	mutex_unlock(&sensor->lock);

	if (*ppos >= len)
		return 0;

	if (count > len - *ppos)
		count = len - *ppos;

	if (copy_to_user(buf, stats + *ppos, count))
		return -EFAULT;

	*ppos += count;
	return count;
}

/*
 * write() — accepts control commands as text:
 *   "exposure=500\n"
 *   "gain=100\n"
 *
 * Blocks if a write is already in progress.
 */
static ssize_t imx708_cdev_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct imx708_dev *sensor = file->private_data;
	char cmd[64];
	unsigned int val;
	int ret;

	if (count >= sizeof(cmd))
		return -EINVAL;

	/* block if another write is in progress */
	ret = wait_event_interruptible(sensor->write_queue,
				       !sensor->write_busy);
	if (ret)
		return -ERESTARTSYS;

	mutex_lock(&sensor->lock);
	sensor->write_busy = true;
	mutex_unlock(&sensor->lock);

	if (copy_from_user(cmd, buf, count)) {
		ret = -EFAULT;
		goto done;
	}
	cmd[count] = '\0';

	/* parse and apply command */
	if (sscanf(cmd, "exposure=%u", &val) == 1) {
		ret = imx708_write_reg(sensor, IMX708_REG_EXPOSURE_HI,
				       (val >> 8) & 0xFF);
		ret |= imx708_write_reg(sensor, IMX708_REG_EXPOSURE_LO,
					val & 0xFF);
		if (!ret) {
			dev_info(&sensor->client->dev,
				 "exposure set to %u\n", val);
			ret = count;
		}
	} else if (sscanf(cmd, "gain=%u", &val) == 1) {
		ret = imx708_write_reg(sensor, IMX708_REG_AGAIN_HI,
				       (val >> 8) & 0xFF);
		ret |= imx708_write_reg(sensor, IMX708_REG_AGAIN_LO,
					val & 0xFF);
		if (!ret) {
			dev_info(&sensor->client->dev,
				 "gain set to %u\n", val);
			ret = count;
		}
	} else {
		dev_warn(&sensor->client->dev,
			 "unknown command: %s\n", cmd);
		ret = -EINVAL;
	}

done:
	mutex_lock(&sensor->lock);
	sensor->write_busy = false;
	mutex_unlock(&sensor->lock);
	wake_up_interruptible(&sensor->write_queue);

	return ret;
}

/*
 * ioctl() — query chip info and streaming state
 */
static long imx708_cdev_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct imx708_dev *sensor = file->private_data;
	unsigned int val;

	switch (cmd) {

	case IMX708_IOC_GET_CHIP_ID:
		val = IMX708_CHIP_ID;
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		return 0;

	case IMX708_IOC_GET_STREAMING:
		mutex_lock(&sensor->lock);
		val = sensor->streaming ? 1 : 0;
		mutex_unlock(&sensor->lock);
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		return 0;

	case IMX708_IOC_SET_EXPOSURE:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
			return -EFAULT;
		imx708_write_reg(sensor, IMX708_REG_EXPOSURE_HI,
				 (val >> 8) & 0xFF);
		imx708_write_reg(sensor, IMX708_REG_EXPOSURE_LO,
				 val & 0xFF);
		return 0;

	case IMX708_IOC_SET_GAIN:
		if (copy_from_user(&val, (void __user *)arg, sizeof(val)))
			return -EFAULT;
		imx708_write_reg(sensor, IMX708_REG_AGAIN_HI,
				 (val >> 8) & 0xFF);
		imx708_write_reg(sensor, IMX708_REG_AGAIN_LO,
				 val & 0xFF);
		return 0;

	case IMX708_IOC_GET_STATS:
		mutex_lock(&sensor->lock);
		val = (unsigned int)sensor->frame_count;
		mutex_unlock(&sensor->lock);
		if (copy_to_user((void __user *)arg, &val, sizeof(val)))
			return -EFAULT;
		return 0;

	default:
		return -ENOTTY;
	}
}

static const struct file_operations imx708_cdev_fops = {
	.owner          = THIS_MODULE,
	.open           = imx708_cdev_open,
	.release        = imx708_cdev_release,
	.read           = imx708_cdev_read,
	.write          = imx708_cdev_write,
	.unlocked_ioctl = imx708_cdev_ioctl,
};

/* ════════════════════════════════════════════════════════════
   V4L2 Controls
   ════════════════════════════════════════════════════════════ */

static int imx708_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx708_dev *sensor =
		container_of(ctrl->handler, struct imx708_dev, ctrl_handler);
	int ret = 0;
	u32 val = ctrl->val;

	if (!sensor->streaming && ctrl->id != V4L2_CID_VBLANK)
		return 0;

	switch (ctrl->id) {

	case V4L2_CID_EXPOSURE:
		ret  = imx708_write_reg(sensor, IMX708_REG_EXPOSURE_HI,
					(val >> 8) & 0xFF);
		ret |= imx708_write_reg(sensor, IMX708_REG_EXPOSURE_LO,
					val & 0xFF);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		ret  = imx708_write_reg(sensor, IMX708_REG_AGAIN_HI,
					(val >> 8) & 0xFF);
		ret |= imx708_write_reg(sensor, IMX708_REG_AGAIN_LO,
					val & 0xFF);
		break;

	case V4L2_CID_DIGITAL_GAIN:
		ret  = imx708_write_reg(sensor, IMX708_REG_DGAIN_HI,
					(val >> 8) & 0xFF);
		ret |= imx708_write_reg(sensor, IMX708_REG_DGAIN_LO,
					val & 0xFF);
		break;

	case V4L2_CID_VBLANK: {
		u32 flen = sensor->cur_mode->height + val;
		ret  = imx708_write_reg(sensor, IMX708_REG_FRAME_LEN_HI,
					(flen >> 8) & 0xFF);
		ret |= imx708_write_reg(sensor, IMX708_REG_FRAME_LEN_LO,
					flen & 0xFF);
		break;
	}

	case V4L2_CID_HFLIP:
		ret = imx708_update_bits(sensor, IMX708_REG_ORIENTATION,
					  BIT(1), val ? BIT(1) : 0);
		break;

	case V4L2_CID_VFLIP:
		ret = imx708_update_bits(sensor, IMX708_REG_ORIENTATION,
					  BIT(0), val ? BIT(0) : 0);
		break;

	case V4L2_CID_TEST_PATTERN:
		ret = imx708_write_reg(sensor, IMX708_REG_TEST_PATTERN,
				       (u8)val);
		break;

	default:
		ret = -EINVAL;
	}

	if (ret)
		dev_err(&sensor->client->dev,
			"ctrl 0x%x write failed: %d\n", ctrl->id, ret);
	return ret;
}

static const struct v4l2_ctrl_ops imx708_ctrl_ops = {
	.s_ctrl = imx708_s_ctrl,
};

static int imx708_init_controls(struct imx708_dev *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	const struct imx708_mode *mode = sensor->cur_mode;
	int ret;

	v4l2_ctrl_handler_init(hdl, 8);

	sensor->exposure = v4l2_ctrl_new_std(hdl, &imx708_ctrl_ops,
		V4L2_CID_EXPOSURE,
		IMX708_EXPOSURE_MIN,
		mode->frame_length - 18,
		IMX708_EXPOSURE_STEP,
		mode->frame_length - 18);

	sensor->again = v4l2_ctrl_new_std(hdl, &imx708_ctrl_ops,
		V4L2_CID_ANALOGUE_GAIN,
		IMX708_AGAIN_MIN, IMX708_AGAIN_MAX,
		IMX708_AGAIN_STEP, IMX708_AGAIN_DEFAULT);

	sensor->dgain = v4l2_ctrl_new_std(hdl, &imx708_ctrl_ops,
		V4L2_CID_DIGITAL_GAIN,
		IMX708_DGAIN_MIN, IMX708_DGAIN_MAX,
		1, IMX708_DGAIN_DEFAULT);

	sensor->vblank = v4l2_ctrl_new_std(hdl, &imx708_ctrl_ops,
		V4L2_CID_VBLANK,
		mode->vblank_min,
		0xFFFF - mode->height,
		1,
		mode->frame_length - mode->height);

	sensor->hflip = v4l2_ctrl_new_std(hdl, &imx708_ctrl_ops,
		V4L2_CID_HFLIP, 0, 1, 1, 0);

	sensor->vflip = v4l2_ctrl_new_std(hdl, &imx708_ctrl_ops,
		V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std_menu_items(hdl, &imx708_ctrl_ops,
		V4L2_CID_TEST_PATTERN,
		3, 0, 0,
		(const char * const[]){
			"Disabled", "Solid Colour",
			"Colour Bars", "Greyscale Bars"
		});

	ret = hdl->error;
	if (ret) {
		v4l2_ctrl_handler_free(hdl);
		return ret;
	}

	sensor->sd.ctrl_handler = hdl;
	return 0;
}

/* ════════════════════════════════════════════════════════════
   Streaming
   ════════════════════════════════════════════════════════════ */

static int imx708_start_streaming(struct imx708_dev *sensor)
{
	int ret;

	ret = imx708_write_regs(sensor,
				 sensor->cur_mode->regs,
				 sensor->cur_mode->num_regs);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
	if (ret)
		return ret;

	ret = imx708_write_reg(sensor,
			       IMX708_REG_MODE_SELECT,
			       IMX708_MODE_STREAMING);
	if (ret)
		return ret;

	sensor->streaming = true;
	dev_info(&sensor->client->dev, "streaming started: %ux%u\n",
		 sensor->cur_mode->width, sensor->cur_mode->height);
	return 0;
}

static int imx708_stop_streaming(struct imx708_dev *sensor)
{
	imx708_write_reg(sensor, IMX708_REG_MODE_SELECT, IMX708_MODE_STANDBY);
	msleep(50);
	sensor->streaming = false;
	dev_info(&sensor->client->dev, "streaming stopped\n");
	return 0;
}

static int imx708_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx708_dev *sensor =
		container_of(sd, struct imx708_dev, sd);
	int ret = 0;

	mutex_lock(&sensor->lock);
	if (enable && !sensor->streaming)
		ret = imx708_start_streaming(sensor);
	else if (!enable && sensor->streaming)
		imx708_stop_streaming(sensor);
	mutex_unlock(&sensor->lock);

	/*
	 * Wake up any readers blocked in imx708_cdev_read() so they can
	 * notice the streaming state changed.
	 */
	wake_up_interruptible(&sensor->wait_queue);

	return ret;
}

/* ════════════════════════════════════════════════════════════
   Format / pad ops
   ════════════════════════════════════════════════════════════ */

static int imx708_enum_mbus_code(struct v4l2_subdev *sd,
	struct v4l2_subdev_state *state,
	struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	return 0;
}

static int imx708_enum_frame_size(struct v4l2_subdev *sd,
	struct v4l2_subdev_state *state,
	struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;
	fse->min_width  = fse->max_width  = supported_modes[fse->index].width;
	fse->min_height = fse->max_height = supported_modes[fse->index].height;
	return 0;
}

static int imx708_get_fmt(struct v4l2_subdev *sd,
	struct v4l2_subdev_state *state,
	struct v4l2_subdev_format *fmt)
{
	struct imx708_dev *sensor =
		container_of(sd, struct imx708_dev, sd);
	mutex_lock(&sensor->lock);
	fmt->format = sensor->fmt;
	mutex_unlock(&sensor->lock);
	return 0;
}

static int imx708_set_fmt(struct v4l2_subdev *sd,
	struct v4l2_subdev_state *state,
	struct v4l2_subdev_format *fmt)
{
	struct imx708_dev *sensor =
		container_of(sd, struct imx708_dev, sd);
	const struct imx708_mode *mode = &supported_modes[0];
	int i;

	for (i = 0; i < ARRAY_SIZE(supported_modes); i++) {
		if (supported_modes[i].width  <= fmt->format.width &&
		    supported_modes[i].height <= fmt->format.height) {
			mode = &supported_modes[i];
			break;
		}
	}

	fmt->format.width  = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code   = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->format.field  = V4L2_FIELD_NONE;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE) {
		mutex_lock(&sensor->lock);
		sensor->fmt      = fmt->format;
		sensor->cur_mode = mode;
		mutex_unlock(&sensor->lock);
	}

	return 0;
}

static const struct v4l2_subdev_video_ops imx708_video_ops = {
	.s_stream = imx708_s_stream,
};

static const struct v4l2_subdev_pad_ops imx708_pad_ops = {
	.enum_mbus_code  = imx708_enum_mbus_code,
	.enum_frame_size = imx708_enum_frame_size,
	.get_fmt         = imx708_get_fmt,
	.set_fmt         = imx708_set_fmt,
};

static const struct v4l2_subdev_ops imx708_subdev_ops = {
	.video = &imx708_video_ops,
	.pad   = &imx708_pad_ops,
};

/* ════════════════════════════════════════════════════════════
   Power helpers
   ════════════════════════════════════════════════════════════ */

static int imx708_power_on(struct imx708_dev *sensor)
{
	struct device *dev = &sensor->client->dev;
	int ret;

	sensor->vana = devm_regulator_get(dev, "vana1");
	sensor->vdig = devm_regulator_get(dev, "vdig");
	sensor->vddl = devm_regulator_get(dev, "vddl");

	if (!IS_ERR(sensor->vddl)) {
		ret = regulator_enable(sensor->vddl);
		if (ret)
			dev_warn(dev, "vddl enable failed: %d\n", ret);
	}
	if (!IS_ERR(sensor->vdig)) {
		ret = regulator_enable(sensor->vdig);
		if (ret)
			dev_warn(dev, "vdig enable failed: %d\n", ret);
	}
	if (!IS_ERR(sensor->vana)) {
		ret = regulator_enable(sensor->vana);
		if (ret) {
			dev_err(dev, "vana1 enable failed: %d\n", ret);
			return ret;
		}
	}

	msleep(120);

	sensor->xclk = devm_clk_get(dev, "inclk");
	if (IS_ERR(sensor->xclk))
		sensor->xclk = devm_clk_get(dev, NULL);
	if (!IS_ERR(sensor->xclk)) {
		clk_set_rate(sensor->xclk, 24000000);
		ret = clk_prepare_enable(sensor->xclk);
		if (ret)
			dev_warn(dev, "clock enable failed: %d\n", ret);
	}

	msleep(20);
	return 0;
}

static void imx708_power_off(struct imx708_dev *sensor)
{
	if (!IS_ERR(sensor->xclk))
		clk_disable_unprepare(sensor->xclk);
	if (!IS_ERR(sensor->vana))
		regulator_disable(sensor->vana);
	if (!IS_ERR(sensor->vdig))
		regulator_disable(sensor->vdig);
	if (!IS_ERR(sensor->vddl))
		regulator_disable(sensor->vddl);
}

/* ════════════════════════════════════════════════════════════
   Probe / Remove
   ════════════════════════════════════════════════════════════ */

static int imx708_probe(struct i2c_client *client)
{
	struct imx708_dev *sensor;
	struct device *dev = &client->dev;
	u8 id_hi, id_lo;
	u16 chip_id;
	int ret;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	sensor->client   = client;
	sensor->cur_mode = &supported_modes[0];
	mutex_init(&sensor->lock);
	init_waitqueue_head(&sensor->wait_queue);
	init_waitqueue_head(&sensor->write_queue);
	i2c_set_clientdata(client, sensor);

	/* ── Power on ── */
	ret = imx708_power_on(sensor);
	if (ret)
		goto err_mutex;

	/* ── Chip ID check ── */
	ret = imx708_read_reg(sensor, IMX708_REG_CHIP_ID, &id_hi);
	if (ret) {
		dev_err(dev, "chip ID hi read failed: %d\n", ret);
		goto err_power;
	}
	ret = imx708_read_reg(sensor, IMX708_REG_CHIP_ID + 1, &id_lo);
	if (ret) {
		dev_err(dev, "chip ID lo read failed: %d\n", ret);
		goto err_power;
	}
	chip_id = ((u16)id_hi << 8) | id_lo;
	if (chip_id != IMX708_CHIP_ID) {
		dev_err(dev, "unexpected chip ID 0x%04x (expected 0x%04x)\n",
			chip_id, IMX708_CHIP_ID);
		ret = -ENODEV;
		goto err_power;
	}

	/* ── Default format ── */
	sensor->fmt.width  = supported_modes[0].width;
	sensor->fmt.height = supported_modes[0].height;
	sensor->fmt.code   = MEDIA_BUS_FMT_SRGGB10_1X10;
	sensor->fmt.field  = V4L2_FIELD_NONE;

	/* ── V4L2 subdev ── */
	v4l2_i2c_subdev_init(&sensor->sd, client, &imx708_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret) {
		dev_err(dev, "media entity init failed: %d\n", ret);
		goto err_power;
	}

	ret = imx708_init_controls(sensor);
	if (ret) {
		dev_err(dev, "controls init failed: %d\n", ret);
		goto err_entity;
	}

	ret = v4l2_async_register_subdev(&sensor->sd);
	if (ret) {
		dev_err(dev, "v4l2 register failed: %d\n", ret);
		goto err_controls;
	}

	/* ── Char device /dev/imx708_ctrl ── */
	ret = alloc_chrdev_region(&sensor->cdev_num, 0, 1, IMX708_CDEV_NAME);
	if (ret) {
		dev_err(dev, "alloc_chrdev_region failed: %d\n", ret);
		goto err_v4l2;
	}

	cdev_init(&sensor->cdev, &imx708_cdev_fops);
	sensor->cdev.owner = THIS_MODULE;
	ret = cdev_add(&sensor->cdev, sensor->cdev_num, 1);
	if (ret) {
		dev_err(dev, "cdev_add failed: %d\n", ret);
		goto err_chrdev;
	}

	sensor->cdev_class = class_create(IMX708_CDEV_NAME);
	if (IS_ERR(sensor->cdev_class)) {
		ret = PTR_ERR(sensor->cdev_class);
		dev_err(dev, "class_create failed: %d\n", ret);
		goto err_cdev;
	}

	if (IS_ERR(device_create(sensor->cdev_class, NULL,
				  sensor->cdev_num, NULL,
				  IMX708_CDEV_NAME))) {
		dev_warn(dev, "device_create failed\n");
	}

	/* ── /proc ── */
	sensor->proc_entry = proc_create_data(IMX708_PROC_NAME, 0444,
					       NULL, &imx708_proc_ops, sensor);
	if (!sensor->proc_entry)
		dev_warn(dev, "failed to create /proc/%s\n", IMX708_PROC_NAME);

	dev_info(dev,
		 "IMX708 custom driver ready: %ux%u chip_id=0x%04x\n"
		 "  /dev/imx708_ctrl and /proc/%s created\n",
		 sensor->cur_mode->width, sensor->cur_mode->height,
		 chip_id, IMX708_PROC_NAME);
	return 0;

err_cdev:
	cdev_del(&sensor->cdev);
err_chrdev:
	unregister_chrdev_region(sensor->cdev_num, 1);
err_v4l2:
	v4l2_async_unregister_subdev(&sensor->sd);
err_controls:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_power:
	imx708_power_off(sensor);
err_mutex:
	mutex_destroy(&sensor->lock);
	return ret;
}

static void imx708_remove(struct i2c_client *client)
{
	struct imx708_dev *sensor = i2c_get_clientdata(client);

	if (sensor->proc_entry)
		remove_proc_entry(IMX708_PROC_NAME, NULL);

	device_destroy(sensor->cdev_class, sensor->cdev_num);
	class_destroy(sensor->cdev_class);
	cdev_del(&sensor->cdev);
	unregister_chrdev_region(sensor->cdev_num, 1);

	v4l2_async_unregister_subdev(&sensor->sd);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	media_entity_cleanup(&sensor->sd.entity);
	imx708_power_off(sensor);
	mutex_destroy(&sensor->lock);

	dev_info(&client->dev, "IMX708 custom driver removed\n");
}

/* ════════════════════════════════════════════════════════════
   Module registration
   ════════════════════════════════════════════════════════════ */

static const struct i2c_device_id imx708_id[] = {
	{ "imx708-custom", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, imx708_id);

static const struct of_device_id imx708_of_match[] = {
	{ .compatible = "sony,imx708" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx708_of_match);

static struct i2c_driver imx708_driver = {
	.driver = {
		.name           = IMX708_DRIVER_NAME,
		.of_match_table = imx708_of_match,
	},
	.probe    = imx708_probe,
	.remove   = imx708_remove,
	.id_table = imx708_id,
};

module_i2c_driver(imx708_driver);

MODULE_SOFTDEP("pre: videodev v4l2-async v4l2-fwnode");
MODULE_AUTHOR("Patrick");
MODULE_DESCRIPTION("IMX708 Custom Camera Sensor Driver");
MODULE_LICENSE("GPL");