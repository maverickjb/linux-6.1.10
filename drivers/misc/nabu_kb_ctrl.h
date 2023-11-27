#ifndef __XIAOMI_KEYBOARD_H
#define __XIAOMI_KEYBOARD_H

#include <linux/regulator/consumer.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>

#define XIAOMI_KB_TAG "xiaomi-keyboard"
#define MI_KB_INFO(fmt, args...)    pr_info("[%s] %s %d: " fmt, XIAOMI_KB_TAG, __func__, __LINE__, ##args)
#define MI_KB_ERR(fmt, args...)    pr_err("[%s] %s %d: " fmt, XIAOMI_KB_TAG, __func__, __LINE__, ##args)

struct xiaomi_keyboard_platdata {
	u32 rst_gpio;
	u32 rst_flags;
	u32 in_irq_gpio;
	u32 in_irq_flags;
	u32 vdd_gpio;
};

struct xiaomi_keyboard_data {
	struct xiaomi_keyboard_platdata *pdata;
	struct platform_device *pdev;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_active;
	struct pinctrl_state *pins_suspend;
	struct mutex rw_mutex;

	bool keyboard_is_enable;
};
#endif
