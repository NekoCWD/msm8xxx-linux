// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2025 Vasiliy Doylov <nekocwd@mainlining.org>

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-event.h>

#define SA3103_NAME "sa3103"
/* Actuator has 11 bit resolution */
#define SA3103_MAX_FOCUS_POS (2048 - 1)
#define SA3103_MIN_FOCUS_POS 0
#define SA3103_FOCUS_STEPS 1

#define SA3103_MSB_ADDR 130
#define SA3103_ENABLE_ADDR 129


static const char *const sa3103_supply_names[] = {
	"vcc",
};

struct sa3103 {
	struct regulator_bulk_data supplies[ARRAY_SIZE(sa3103_supply_names)];
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct v4l2_subdev sd;
};

static inline struct sa3103 *sd_to_sa3103(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct sa3103, sd);
}

static int sa3103_set_dac(struct sa3103 *sa3103, u16 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&sa3103->sd);
	i2c_smbus_write_byte_data(client, SA3103_ENABLE_ADDR, 1);

	return i2c_smbus_write_word_swapped(client, SA3103_MSB_ADDR, val);
}

static int __maybe_unused sa3103_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sa3103 *sa3103 = sd_to_sa3103(sd);

	regulator_bulk_disable(ARRAY_SIZE(sa3103_supply_names),
			       sa3103->supplies);

	return 0;
}

static int __maybe_unused sa3103_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct sa3103 *sa3103 = sd_to_sa3103(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(sa3103_supply_names),
				    sa3103->supplies);

	if (ret < 0) {
		dev_err(dev, "failed to enable regulators\n");
		return ret;
	}

	usleep_range(8000, 10000);

	return ret;
}

static int sa3103_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sa3103 *sa3103 =
		container_of(ctrl->handler, struct sa3103, ctrls);

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
		return sa3103_set_dac(sa3103, ctrl->val);

	return 0;
}

static const struct v4l2_ctrl_ops sa3103_ctrl_ops = {
	.s_ctrl = sa3103_set_ctrl,
};

static int sa3103_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int sa3103_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_mark_last_busy(sd->dev);
	pm_runtime_put_autosuspend(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops sa3103_int_ops = {
	.open = sa3103_open,
	.close = sa3103_close,
};

static const struct v4l2_subdev_core_ops sa3103_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops sa3103_ops = {
	.core = &sa3103_core_ops,
};

static int sa3103_init_controls(struct sa3103 *sa3103)
{
	struct v4l2_ctrl_handler *hdl = &sa3103->ctrls;
	const struct v4l2_ctrl_ops *ops = &sa3103_ctrl_ops;

	v4l2_ctrl_handler_init(hdl, 1);

	sa3103->focus = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE,
					      SA3103_MIN_FOCUS_POS,
					      SA3103_MAX_FOCUS_POS,
					      SA3103_FOCUS_STEPS, 0);

	if (hdl->error)
		return hdl->error;

	sa3103->sd.ctrl_handler = hdl;

	return 0;
}

static int sa3103_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sa3103 *sa3103;
	unsigned int i;
	int ret;

	sa3103 = devm_kzalloc(dev, sizeof(*sa3103), GFP_KERNEL);
	if (!sa3103)
		return -ENOMEM;

	/* Initialize subdev */
	v4l2_i2c_subdev_init(&sa3103->sd, client, &sa3103_ops);

	for (i = 0; i < ARRAY_SIZE(sa3103_supply_names); i++)
		sa3103->supplies[i].supply = sa3103_supply_names[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(sa3103_supply_names),
				      sa3103->supplies);

	if (ret) {
		dev_err(dev, "failed to get regulators\n");
		return ret;
	}

	/* Initialize controls */
	ret = sa3103_init_controls(sa3103);
	if (ret)
		goto err_free_handler;

	/* Initialize subdev */
	sa3103->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				V4L2_SUBDEV_FL_HAS_EVENTS;
	sa3103->sd.internal_ops = &sa3103_int_ops;

	ret = media_entity_pads_init(&sa3103->sd.entity, 0, NULL);
	if (ret < 0)
		goto err_free_handler;

	sa3103->sd.entity.function = MEDIA_ENT_F_LENS;

	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev(&sa3103->sd);

	if (ret < 0) {
		dev_err(dev, "failed to register V4L2 subdev: %d", ret);
		goto err_power_off;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

err_power_off:
	pm_runtime_disable(dev);
	media_entity_cleanup(&sa3103->sd.entity);
err_free_handler:
	v4l2_ctrl_handler_free(&sa3103->ctrls);

	return ret;
}

static void sa3103_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sa3103 *sa3103 = sd_to_sa3103(sd);
	struct device *dev = &client->dev;

	v4l2_async_unregister_subdev(&sa3103->sd);
	v4l2_ctrl_handler_free(&sa3103->ctrls);
	media_entity_cleanup(&sa3103->sd.entity);
	pm_runtime_disable(dev);
}

static const struct of_device_id sa3103_of_table[] = {
	{ .compatible = "sa,sa3103" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sa3103_of_table);

static const struct dev_pm_ops sa3103_pm_ops = {
	SET_RUNTIME_PM_OPS(sa3103_runtime_suspend,
			   sa3103_runtime_resume, NULL)
};

static struct i2c_driver sa3103_i2c_driver = {
	.driver = {
		.name = SA3103_NAME,
		.pm = &sa3103_pm_ops,
		.of_match_table = sa3103_of_table,
	},
	.probe = sa3103_probe,
	.remove = sa3103_remove,
};
module_i2c_driver(sa3103_i2c_driver);

MODULE_AUTHOR("Vasiliy Doylov <nekocwd@mainlining.org>");
MODULE_DESCRIPTION("SA3103 VCM driver");
MODULE_LICENSE("GPL");
