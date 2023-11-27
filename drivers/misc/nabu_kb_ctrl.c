#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/device.h>
#include "nabu_kb_ctrl.h"
//#include <drm/drm_notifier.h>
#include <linux/notifier.h>

static struct xiaomi_keyboard_data *mdata;

static void set_keyboard_status(bool on);

static void xiaomi_keyboard_reset(void)
{
	if (!mdata || !mdata->pdata) {
		MI_KB_ERR("reset failed!Invalid Memory\n");
		return;
	}
	MI_KB_INFO("xiaomi keyboard IC Reset\n");
	gpio_direction_output(mdata->pdata->rst_gpio, 0);
	msleep(2);
	gpio_direction_output(mdata->pdata->rst_gpio, 1);
}

static int xiaomi_keyboard_gpio_config(struct xiaomi_keyboard_platdata *pdata)
{
	int ret = 0;
	if (gpio_is_valid(pdata->rst_gpio)) {
		ret = gpio_request_one(pdata->rst_gpio, GPIOF_OUT_INIT_LOW, "kb_rst");
		if (ret) {
			MI_KB_ERR("Failed to request xiaomi keyboard rst gpio\n");
			goto err_request_rst_gpio;
		}
	}

	if (gpio_is_valid(pdata->in_irq_gpio)) {
		ret = gpio_request_one(pdata->in_irq_gpio, GPIOF_IN, "kb_in_irq");
		if (ret) {
			MI_KB_ERR("Failed to request xiaomi keyboard in-irq gpio\n");
			goto err_request_in_irq_gpio;
		}
	}

	return ret;
err_request_in_irq_gpio:
	gpio_free(pdata->rst_gpio);
err_request_rst_gpio:
	return ret;
}

static void xiaomi_keyboard_gpio_deconfig(struct xiaomi_keyboard_platdata *pdata)
{
	if (gpio_is_valid(pdata->rst_gpio))
		gpio_free(pdata->rst_gpio);

	if (gpio_is_valid(pdata->in_irq_gpio))
		gpio_free(pdata->in_irq_gpio);
}

static int xiaomi_keyboard_setup_gpio(struct xiaomi_keyboard_platdata *pdata)
{
	int ret = 0;
	if (!pdata) {
		MI_KB_ERR("xiaomi keyboard platdata is NULL\n");
		return -EINVAL;
	}
	if (gpio_is_valid(pdata->rst_gpio))
		gpio_direction_output(pdata->rst_gpio, 1);

	return ret;
}

static int xiaomi_keyboard_resetup_gpio(struct xiaomi_keyboard_platdata *pdata)
{
	int ret = 0;

	if (!mdata || !pdata) {
		MI_KB_ERR("mdata or pdata not ready, return!");
		return -EINVAL;
	}

	if (gpio_is_valid(pdata->rst_gpio))
		gpio_direction_output(pdata->rst_gpio, 0);

	return ret;
}

#ifdef CONFIG_OF
static int xiaomi_keyboard_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct xiaomi_keyboard_platdata *pdata;
	int ret = 0;

	pdata = mdata->pdata;

	pdata->rst_gpio = of_get_named_gpio_flags(np, "xiaomi-keyboard,rst-gpio", 0, &pdata->rst_flags);
	MI_KB_INFO("xiaomi-kb,reset-gpio=%d\n", pdata->rst_gpio);

	pdata->in_irq_gpio = of_get_named_gpio_flags(np, "xiaomi-keyboard,in-irq-gpio", 0, &pdata->in_irq_flags);
	MI_KB_INFO("xiaomi-kb,in-irq-gpio=%d\n", pdata->in_irq_gpio);

	pdata->vdd_gpio = of_get_named_gpio(np, "xiaomi-keyboard,vdd-gpio", 0);
	MI_KB_INFO("xiaomi-kb,vdd-gpio=%d\n", pdata->vdd_gpio);

	return ret;
}
#else
static int xiaomi_keyboard_parse_dt(struct device *dev)
{
	MI_KB_ERR("Xiaomi Keyboard dev is not defined\n");
	return -EINVAL;
}
#endif

static int xiaomi_keyboard_pinctrl_init(struct device *dev)
{
	int ret = 0;

	mdata->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR_OR_NULL(mdata->pinctrl)) {
		MI_KB_ERR("Failed to get pinctrl, please check dts\n");
		ret = PTR_ERR(mdata->pinctrl);
		goto err_pinctrl_get;
	}
	mdata->pins_active = pinctrl_lookup_state(mdata->pinctrl, "pm_kb_active");
	if (IS_ERR_OR_NULL(mdata->pins_active)) {
		MI_KB_ERR("Pin state [active] not found\n");
		ret = PTR_ERR(mdata->pins_active);
		goto err_pinctrl_lookup;
	}

	mdata->pins_suspend = pinctrl_lookup_state(mdata->pinctrl, "pm_kb_suspend");
	if (IS_ERR_OR_NULL(mdata->pins_suspend)) {
		MI_KB_ERR("Pin state [suspend] not found\n");
		ret = PTR_ERR(mdata->pins_suspend);
		goto err_pinctrl_lookup;
	}

	return 0;
err_pinctrl_lookup:
	if (mdata->pinctrl) {
		devm_pinctrl_put(mdata->pinctrl);
	}
err_pinctrl_get:
	return ret;
}

static int xiaomi_keyboard_power_on(void)
{
	int ret = 0;
	struct xiaomi_keyboard_platdata *pdata;
	pdata = mdata->pdata;
	MI_KB_INFO("Power On\n");
	if (gpio_is_valid(pdata->vdd_gpio)) {
		ret = gpio_request_one(pdata->vdd_gpio, GPIOF_OUT_INIT_HIGH, "kb_vdd_gpio");
		if (ret) {
			MI_KB_ERR("Failed to request xiaomi-keyboard-out-irq gpio\n");
			goto err_request_vdd_gpio;
		}
	}

	gpio_direction_output(pdata->vdd_gpio, 1);
err_request_vdd_gpio:
	return ret;
}

static void xiaomi_keyboard_power_off(void)
{
	struct xiaomi_keyboard_platdata *pdata;
	pdata = mdata->pdata;
	MI_KB_INFO("Power Off\n");
	if (gpio_is_valid(pdata->vdd_gpio)) {
		gpio_direction_output(pdata->vdd_gpio, 0);
		gpio_free(pdata->vdd_gpio);
	}
	return;
}

static int xiaomi_keyboard_pm_suspend(struct device *dev)
{
	int ret = 0;

	set_keyboard_status(0);

	return ret;
}

static int xiaomi_keyboard_pm_resume(struct device *dev)
{
	int ret = 0;

	xiaomi_keyboard_reset();
	set_keyboard_status(1);

	return ret;
}

static const struct dev_pm_ops xiaomi_keyboard_pm_ops = {
	.suspend = xiaomi_keyboard_pm_suspend,
	.resume = xiaomi_keyboard_pm_resume,
};

static void set_keyboard_status(bool on) {
	int ret = 0;

	if (!mdata || !(mdata->pdata)) {
		MI_KB_ERR("mdata or pdata not ready, return!");
		return;
	}

	if (on && !(mdata->keyboard_is_enable)) {
		ret = xiaomi_keyboard_power_on();
		if (ret) {
			MI_KB_ERR("Init 3.3V power failed\n");
			return;
		}
		msleep(1);
		ret = xiaomi_keyboard_setup_gpio(mdata->pdata);
		if (ret) {
			MI_KB_ERR("setup gpio failed\n");
			return;
		}
		msleep(2);

		ret = pinctrl_select_state(mdata->pinctrl, mdata->pins_active);
		if (ret < 0) {
			MI_KB_ERR("Set active pin state error:%d\n", ret);
		}
		mdata->keyboard_is_enable = true;

	} else if (!on && mdata->keyboard_is_enable) {
		ret = pinctrl_select_state(mdata->pinctrl, mdata->pins_suspend);
		if (ret < 0) {
			MI_KB_ERR("Set suspend pin state error:%d\n", ret);
		}

		ret = xiaomi_keyboard_resetup_gpio(mdata->pdata);
		if (ret < 0) {
			MI_KB_ERR("resetup gpio failed\n");
			return;
		}
		xiaomi_keyboard_power_off();
		mdata->keyboard_is_enable = false;
	} else {
		MI_KB_INFO("keyboard status do not need change!");
	}
}

/*******************************************************
Description:
	xiami pad keyboard driver probe function.

return:
	Executive outcomes. 0---succeed. negative---failed
*******************************************************/
static int xiaomi_keyboard_probe(struct platform_device *pdev)
{
	struct xiaomi_keyboard_platdata *pdata;
	int ret = 0;
	MI_KB_INFO("enter\n");
	mdata = kzalloc(sizeof(struct xiaomi_keyboard_data), GFP_KERNEL);
	if (!mdata) {
		MI_KB_ERR("Alloc Memory for xiaomi_keyboard_data failed\n");
		return -ENOMEM;
	}

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct xiaomi_keyboard_platdata), GFP_KERNEL);
	if (!pdata) {
		MI_KB_ERR("Alloc Memory for xiaomi_keyboard_platdata failed\n");
		return -ENOMEM;
	}

	mdata->pdev = pdev;
	mdata->pdata = pdata;
	mutex_init(&mdata->rw_mutex);

	ret = xiaomi_keyboard_parse_dt(&pdev->dev);
	if (ret) {
		MI_KB_ERR("parse device tree failed\n");
		goto out;
	}

	ret = xiaomi_keyboard_pinctrl_init(&pdev->dev);
	if (ret) {
		MI_KB_ERR("Pinctrl init failed\n");
		goto out;
	}

	pdata = mdata->pdata;
	ret = xiaomi_keyboard_gpio_config(pdata);
	if (ret) {
		MI_KB_ERR("set gpio config failed\n");
		goto err_pinctrl_select;
	}

	mdata->keyboard_is_enable = false;

	xiaomi_keyboard_reset();
	MI_KB_INFO("enable keyboard\n");
	set_keyboard_status(1);

	MI_KB_INFO("Success\n");
	return ret;

err_pinctrl_select:
	if (mdata->pinctrl) {
		devm_pinctrl_put(mdata->pinctrl);
	}
	xiaomi_keyboard_gpio_deconfig(pdata);
out:
	mutex_destroy(&mdata->rw_mutex);

	if (mdata) {
		kfree(mdata);
		mdata = NULL;
	}
	MI_KB_ERR("Failed\n");
	return ret;
}

static int xiaomi_keyboard_remove(struct platform_device *pdev)
{
	MI_KB_INFO("enter\n");
	xiaomi_keyboard_gpio_deconfig(mdata->pdata);
	xiaomi_keyboard_power_off();
	devm_pinctrl_put(mdata->pinctrl);
	xiaomi_keyboard_gpio_deconfig(mdata->pdata);
	mutex_destroy(&mdata->rw_mutex);

	if (mdata) {
		kfree(mdata);
		mdata = NULL;
	}
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id xiaomi_keyboard_dt_match[] = {
	{ .compatible = "xiaomi,keyboard" },
	{},
};
MODULE_DEVICE_TABLE(of, xiaomi_keyboard_dt_match);
#endif

static const struct platform_device_id xiaomi_keyboard_driver_ids[] = {
	{
		.name = "xiaomi-keyboard",
		.driver_data = 0,
	},
};
MODULE_DEVICE_TABLE(platform, xiaomi_keyboard_driver_ids);


static struct platform_driver xiaomi_keyboard_driver = {
	.probe        = xiaomi_keyboard_probe,
	.remove       = xiaomi_keyboard_remove,
	.driver       = {
		.name = "xiaomi-keyboard",
		.of_match_table = of_match_ptr(xiaomi_keyboard_dt_match),
		.pm = &xiaomi_keyboard_pm_ops,
	},
	.id_table     = xiaomi_keyboard_driver_ids,
};

module_platform_driver(xiaomi_keyboard_driver);

MODULE_DESCRIPTION("Xiaomi Keyboard Control-driver");
MODULE_AUTHOR("Tonghui Wang<wangtonghui@xiaomi.com>");
