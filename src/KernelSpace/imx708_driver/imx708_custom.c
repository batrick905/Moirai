// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include "imx708_custom.h"

/* ════════════════════════════════════════
      Register sequences
   ════════════════════════════════════════ */

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

/* ════════════════════════════════════════
      regmap
   ════════════════════════════════════════ */

static const struct regmap_config imx708_regmap_config = {
      .reg_bits   = 16,
      .val_bits   = 8,
      .cache_type = REGCACHE_RBTREE,
};

/* ════════════════════════════════════════
      /proc stats
   ════════════════════════════════════════ */

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

/* ════════════════════════════════════════
      Controls
   ════════════════════════════════════════ */

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
            ret  = regmap_write(sensor->regmap, IMX708_REG_EXPOSURE_HI,
                              (val >> 8) & 0xFF);
            ret |= regmap_write(sensor->regmap, IMX708_REG_EXPOSURE_LO,
                              val & 0xFF);
            break;

      case V4L2_CID_ANALOGUE_GAIN:
            ret  = regmap_write(sensor->regmap, IMX708_REG_AGAIN_HI,
                              (val >> 8) & 0xFF);
            ret |= regmap_write(sensor->regmap, IMX708_REG_AGAIN_LO,
                              val & 0xFF);
            break;

      case V4L2_CID_DIGITAL_GAIN:
            ret  = regmap_write(sensor->regmap, IMX708_REG_DGAIN_HI,
                              (val >> 8) & 0xFF);
            ret |= regmap_write(sensor->regmap, IMX708_REG_DGAIN_LO,
                              val & 0xFF);
            break;

      case V4L2_CID_VBLANK: {
            u32 flen = sensor->cur_mode->height + val;
            ret  = regmap_write(sensor->regmap, IMX708_REG_FRAME_LEN_HI,
                              (flen >> 8) & 0xFF);
            ret |= regmap_write(sensor->regmap, IMX708_REG_FRAME_LEN_LO,
                              flen & 0xFF);
            break;
      }

      case V4L2_CID_HFLIP:
            ret = regmap_update_bits(sensor->regmap, IMX708_REG_ORIENTATION,
                                    BIT(1), val ? BIT(1) : 0);
            break;

      case V4L2_CID_VFLIP:
            ret = regmap_update_bits(sensor->regmap, IMX708_REG_ORIENTATION,
                                    BIT(0), val ? BIT(0) : 0);
            break;

      case V4L2_CID_TEST_PATTERN:
            ret = regmap_write(sensor->regmap, IMX708_REG_TEST_PATTERN,
                              val);
            break;

      default:
            ret = -EINVAL;
      }

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

/* ════════════════════════════════════════
      Streaming
   ════════════════════════════════════════ */

static int imx708_write_regs(struct imx708_dev *sensor,
                              const struct reg_sequence *regs, u32 n)
{
      int ret = regmap_multi_reg_write(sensor->regmap, regs, n);
      if (ret)
            dev_err(&sensor->client->dev, "reg write failed: %d\n", ret);
      return ret;
}

static int imx708_start_streaming(struct imx708_dev *sensor)
{
      int ret;

      ret = imx708_write_regs(sensor,
                              sensor->cur_mode->regs,
                              sensor->cur_mode->num_regs);
      if (ret) return ret;

      ret = __v4l2_ctrl_handler_setup(&sensor->ctrl_handler);
      if (ret) return ret;

      ret = regmap_write(sensor->regmap,
                        IMX708_REG_MODE_SELECT, IMX708_MODE_STREAMING);
      if (ret) return ret;

      sensor->streaming = true;
      return 0;
}

static int imx708_stop_streaming(struct imx708_dev *sensor)
{
      regmap_write(sensor->regmap,
                  IMX708_REG_MODE_SELECT, IMX708_MODE_STANDBY);
      msleep(50);
      sensor->streaming = false;
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

      return ret;
}

/* ════════════════════════════════════════
      Format / pad ops
   ════════════════════════════════════════ */

static int imx708_enum_mbus_code(struct v4l2_subdev *sd,
    struct v4l2_subdev_state *state,
    struct v4l2_subdev_mbus_code_enum *code)
{
      if (code->index > 0) return -EINVAL;
      code->code = MEDIA_BUS_FMT_SRGGB10_1X10;
      return 0;
}

static int imx708_enum_frame_size(struct v4l2_subdev *sd,
    struct v4l2_subdev_state *state,
    struct v4l2_subdev_frame_size_enum *fse)
{
      if (fse->index >= ARRAY_SIZE(supported_modes)) return -EINVAL;
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

      /* pick closest supported mode */
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

/* ════════════════════════════════════════
      Probe / Remove
   ════════════════════════════════════════ */

static int imx708_probe(struct i2c_client *client)
{
      struct imx708_dev *sensor;
      unsigned int chip_id;
      int ret;

      sensor = devm_kzalloc(&client->dev, sizeof(*sensor), GFP_KERNEL);
      if (!sensor) return -ENOMEM;

      sensor->client   = client;
      sensor->cur_mode = &supported_modes[0];
      mutex_init(&sensor->lock);
      i2c_set_clientdata(client, sensor);

      /* regmap */
      sensor->regmap = devm_regmap_init_i2c(client, &imx708_regmap_config);
      if (IS_ERR(sensor->regmap))
            return PTR_ERR(sensor->regmap);

      /* chip ID check */
      ret = regmap_read(sensor->regmap, IMX708_REG_CHIP_ID, &chip_id);
      if (ret) {
            dev_err(&client->dev, "chip ID read failed: %d\n", ret);
            return ret;
      }
      if (chip_id != IMX708_CHIP_ID) {
            dev_err(&client->dev, "wrong chip ID 0x%04x\n", chip_id);
            return -ENODEV;
      }

      /* default format */
      sensor->fmt.width  = supported_modes[0].width;
      sensor->fmt.height = supported_modes[0].height;
      sensor->fmt.code   = MEDIA_BUS_FMT_SRGGB10_1X10;
      sensor->fmt.field  = V4L2_FIELD_NONE;

      /* V4L2 subdev */
      v4l2_i2c_subdev_init(&sensor->sd, client, &imx708_subdev_ops);
      sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
      sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

      /* media pad */
      sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
      ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
      if (ret) goto err_mutex;

      /* controls */
      ret = imx708_init_controls(sensor);
      if (ret) goto err_entity;

      /* register subdev */
      ret = v4l2_async_register_subdev(&sensor->sd);
      if (ret) goto err_controls;

      /* /proc entry */
      sensor->proc_entry = proc_create_data("imx708_custom_stats", 0444,
                                                NULL, &imx708_proc_ops,
                                                sensor);

      dev_info(&client->dev,
                  "IMX708 ready: %ux%u, chip_id=0x%04x\n",
                  sensor->cur_mode->width, sensor->cur_mode->height, chip_id);
      return 0;

err_controls:
      v4l2_ctrl_handler_free(&sensor->ctrl_handler);
err_entity:
      media_entity_cleanup(&sensor->sd.entity);
err_mutex:
      mutex_destroy(&sensor->lock);
      return ret;
}

static void imx708_remove(struct i2c_client *client)
{
    struct imx708_dev *sensor = i2c_get_clientdata(client);

      if (sensor->proc_entry)
            remove_proc_entry("imx708_custom_stats", NULL);

      v4l2_async_unregister_subdev(&sensor->sd);
      v4l2_ctrl_handler_free(&sensor->ctrl_handler);
      media_entity_cleanup(&sensor->sd.entity);
      mutex_destroy(&sensor->lock);
}

/* ════════════════════════════════════════
      Module boilerplate
   ════════════════════════════════════════ */

static const struct i2c_device_id imx708_id[] = {
      { "imx708", 0 },
      { }
};
MODULE_DEVICE_TABLE(i2c, imx708_id);

static const struct of_device_id imx708_of_match[] = {
      { .compatible = "sony,imx708-custom" },   /* must match dts */
      { }
};
MODULE_DEVICE_TABLE(of, imx708_of_match);

static struct i2c_driver imx708_driver = {
      .driver = {
            .name           = "imx708-custom",
            .of_match_table = imx708_of_match,
      },
      .probe    = imx708_probe,
      .remove   = imx708_remove,
      .id_table = imx708_id,
};

module_i2c_driver(imx708_driver);

MODULE_SOFTDEP("pre : regmap-i2c v4l2-common videobuf2-v4l2 v4l2-async");
MODULE_AUTHOR("Patrick");
MODULE_DESCRIPTION("IMX708 Custom Camera Sensor Driver");
MODULE_LICENSE("GPL");