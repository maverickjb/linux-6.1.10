#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/power_supply.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/usb/pd.h>

#include "usbpd_manager.h"

#define MIN_ADAPTER_VOLT_REQUIRED		10000		
#define MIN_ADAPTER_CURR_REQUIRED		2000

#define BATT_MAX_CHG_VOLT		4450
#define BATT_FAST_CHG_CURR		6000
#define	BUS_OVP_THRESHOLD		12000
#define	APDO_MAX_VOLT		11000

#define BUS_VOLT_INIT_UP		400

#define BAT_VOLT_LOOP_LMT		BATT_MAX_CHG_VOLT
#define BAT_CURR_LOOP_LMT		BATT_FAST_CHG_CURR
#define BUS_VOLT_LOOP_LMT		BUS_OVP_THRESHOLD
#define BUS_CURR_LOOP_LMT		(BATT_FAST_CHG_CURR >> 1)

#define PM_WORK_RUN_NORMAL_INTERVAL		500
#define PM_WORK_RUN_QUICK_INTERVAL		200
#define PM_WORK_RUN_CRITICAL_INTERVAL		100

#define TAPER_TIMEOUT	(25000 / PM_WORK_RUN_NORMAL_INTERVAL)
#define IBUS_CHANGE_TIMEOUT  (2500 / PM_WORK_RUN_NORMAL_INTERVAL)

/* Power Supply access to expose source power information */
enum tcpm_psy_online_states {
	TCPM_PSY_OFFLINE = 0,
	TCPM_PSY_FIXED_ONLINE,
	TCPM_PSY_PROG_ONLINE,
};

u32 usbpd_source_caps[PDO_MAX_OBJECTS];
unsigned int usbpd_nr_source_caps;

static bool get_psy_enabled(struct power_supply *psy)
{
	union power_supply_propval val;
	int ret;

	if (!psy)
		return false;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_STATUS,
			&val);
	if (ret < 0) {
		return false;
	}

//	pdbg("usbpd: POWER_SUPPLY_PROP_STATUS %d\n", val.intval);

	if (val.intval == POWER_SUPPLY_STATUS_CHARGING)
		return true;

	return false;
}

static int set_psy_enable(struct power_supply *psy, bool enable)
{
	union power_supply_propval val;

	if (!psy)
		return -ENODEV;

	val.intval = enable;
	return power_supply_set_property(psy, POWER_SUPPLY_PROP_STATUS,
			&val);
}

static int get_psy_temperature(struct power_supply *psy, int *temperature)
{
	union power_supply_propval val;
	int ret;

	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_TEMP,
			&val);
	if (ret < 0) {
		return ret;
	}

	*temperature = val.intval;
	return 0;
}

static int get_psy_capacity(struct power_supply *psy, int *capacity)
{
	union power_supply_propval val;
	int ret;

	if (!psy)
		return -ENODEV;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_CAPACITY,
			&val);
	if (ret < 0) {
		return ret;
	}

	*capacity = val.intval;
	return 0;
}

static bool get_psy_pps_status(struct power_supply *psy)
{
	union power_supply_propval val;
	int ret;

	if (!psy)
		return false;

	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_USB_TYPE,
			&val);
	if (ret == 0 && (val.intval == POWER_SUPPLY_USB_TYPE_PD_PPS)) {
		return true;
	}

	return false;
}

static inline bool get_usbpd_cp_enabled(struct usbpd_pm *pdpm)
{
	return get_psy_enabled(pdpm->cp_psy);
}

static inline int set_usbpd_cp_enable(struct usbpd_pm *pdpm, bool enable)
{
	return set_psy_enable(pdpm->cp_psy, enable);
}

static inline int get_usbpd_bat_temperature(struct usbpd_pm *pdpm, int *temperature)
{
	return get_psy_temperature(pdpm->fg_psy, temperature);
}

static inline int get_usbpd_bat_capacity(struct usbpd_pm *pdpm, int *capacity)
{
	return get_psy_capacity(pdpm->fg_psy, capacity);
}

static inline bool get_usbpd_sw_enabled(struct usbpd_pm *pdpm)
{
	return get_psy_enabled(pdpm->sw_psy);
}

static int set_usbpd_sw_enable(struct usbpd_pm *pdpm, bool enable)
{
	struct power_supply *psy = pdpm->sw_psy;
	union power_supply_propval val;

	if (!psy)
		return -ENODEV;

	if (enable)
		val.intval = 2000000;
	else
		val.intval = 25000;
	return power_supply_set_property(psy, POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
			&val);
}

static inline bool get_usbpd_pps_enabled(struct usbpd_pm *pdpm)
{
	return get_psy_pps_status(pdpm->tcpm_psy);
}

static inline int set_usbpd_pps_enable(struct usbpd_pm *pdpm, bool enable)
{
	union power_supply_propval val;

	if (!pdpm->tcpm_psy)
		return -ENODEV;

	val.intval = enable ? TCPM_PSY_PROG_ONLINE : TCPM_PSY_FIXED_ONLINE;
	return power_supply_set_property(pdpm->tcpm_psy, POWER_SUPPLY_PROP_ONLINE,
			&val);
}

static int set_usbpd_pps_voltage(struct usbpd_pm *pdpm, int vbus)
{
	union power_supply_propval val;

	if (!pdpm->tcpm_psy)
		return -ENODEV;

	val.intval = vbus;
	return power_supply_set_property(pdpm->tcpm_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW,
			&val);
}

static int set_usbpd_pps_current(struct usbpd_pm *pdpm, int ibus)
{
	union power_supply_propval val;

	if (!pdpm->tcpm_psy)
		return -ENODEV;

	val.intval = ibus;
	return power_supply_set_property(pdpm->tcpm_psy, POWER_SUPPLY_PROP_CURRENT_NOW,
			&val);
}

static void usbpd_pm_update_sw_status(struct usbpd_pm *pdpm)
{
	struct sw_device *sw = &pdpm->sw;
	sw->charge_enabled = get_usbpd_sw_enabled(pdpm);
}

static void usbpd_pm_update_cp_status(struct usbpd_pm *pdpm)
{
	int ret;
	union power_supply_propval val = {0,};
	struct cp_device *cp = &pdpm->cp;

	if (!pdpm->cp_psy)
		return;

	memset(cp, 0, sizeof(*cp));

	ret = power_supply_get_property(pdpm->cp_batt_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (!ret)
		cp->vbat_volt = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
					POWER_SUPPLY_PROP_VOLTAGE_NOW, &val);
	if (!ret)
		cp->vbus_volt = val.intval;

	ret = power_supply_get_property(pdpm->cp_psy,
					POWER_SUPPLY_PROP_CURRENT_NOW, &val);
	if (!ret)
		cp->ibus_curr = val.intval;
#if 0
	ret = power_supply_get_property(pdpm->cp_psy,
					POWER_SUPPLY_PROP_ONLINE, &val);
	if (!ret)
		cp->vbus_pres = val.intval;

	if (pdpm->fg_psy && cp->vbus_pres) {
		ret = power_supply_get_property(pdpm->fg_psy,
						POWER_SUPPLY_PROP_CURRENT_NOW, &val);
		if (!ret)
			cp->ibat_curr = -(val.intval / 1000);
	}
#else
	if (pdpm->fg_psy) {
		ret = power_supply_get_property(pdpm->fg_psy,
						POWER_SUPPLY_PROP_CURRENT_NOW, &val);
		if (!ret)
			cp->ibat_curr = -(val.intval / 1000);
	}
#endif
	ret = power_supply_get_property(pdpm->cp_psy,
					POWER_SUPPLY_PROP_STATUS, &val);
	if (!ret)
		cp->charge_enabled = (val.intval == POWER_SUPPLY_STATUS_CHARGING);

	ret = power_supply_get_property(pdpm->cp_psy,
			POWER_SUPPLY_PROP_HEALTH, &val);
	if (!ret) {
		if (val.intval == POWER_SUPPLY_HEALTH_OVERHEAT)
			cp->bus_therm_fault = true;
		if (val.intval == POWER_SUPPLY_HEALTH_OVERVOLTAGE)
			cp->bus_ovp_fault = true;
		if (val.intval == POWER_SUPPLY_HEALTH_OVERCURRENT)
			cp->bus_ocp_fault = true;
	}

	ret = power_supply_get_property(pdpm->cp_batt_psy,
			POWER_SUPPLY_PROP_HEALTH, &val);
	if (!ret) {
		if (val.intval == POWER_SUPPLY_HEALTH_OVERHEAT)
			cp->bat_therm_fault = true;
		if (val.intval == POWER_SUPPLY_HEALTH_OVERVOLTAGE)
			cp->bat_ovp_fault = true;
		if (val.intval == POWER_SUPPLY_HEALTH_OVERCURRENT)
			cp->bat_ocp_fault = true;
	}

}

static void usbpd_pm_move_state(struct usbpd_pm *pdpm, enum pm_state state)
{
#if 1
	pdbg("state change:%s -> %s\n",
		pm_states[pdpm->state], pm_states[state]);
#endif
	pdpm->state = state;
}

static int usbpd_fetch_pdo(struct usbpd_pdo *pdos)
{
	int ret = 0;
	int pdo;
	int i;

	if (!pdos)
		return -EINVAL;

	for (i = 0; i < usbpd_nr_source_caps; i++) {
		pdo = usbpd_source_caps[i];
		if (pdo == 0)
			break;

		pdos[i].pos = i + 1;
		pdos[i].pps = (pdo_apdo_type(pdo) == APDO_TYPE_PPS);
		pdos[i].type = pdo_type(pdo);

		if (pdos[i].type == PDO_TYPE_FIXED) {
			pdos[i].curr_ma = pdo_max_current(pdo);
			pdos[i].max_volt_mv = pdo_fixed_voltage(pdo);
			pdos[i].min_volt_mv = pdo_fixed_voltage(pdo);
			pdbg("pdo:%d, Fixed supply, volt:%d(mv), max curr:%d\n",
					i+1, pdos[i].max_volt_mv,
					pdos[i].curr_ma);
		} else if (pdos[i].type == PDO_TYPE_APDO) {
			pdos[i].max_volt_mv = pdo_pps_apdo_max_voltage(pdo);
			pdos[i].min_volt_mv = pdo_pps_apdo_min_voltage(pdo);
			pdos[i].curr_ma     = pdo_pps_apdo_max_current(pdo);
			pdbg("pdo:%d, PPS, volt: %d(mv), max curr:%d\n",
					i+1, pdos[i].max_volt_mv,
					pdos[i].curr_ma);
		} else {
			pdbg("only fixed and pps pdo supported\n");
		}
	}

	return ret;
}

static int usbpd_pm_evaluate_src_caps(struct usbpd_pm *pdpm)
{
	int ret;
	int i;

	pdpm->apdo_max_volt = MIN_ADAPTER_VOLT_REQUIRED;
	pdpm->apdo_max_curr = MIN_ADAPTER_CURR_REQUIRED;

	if (usbpd_nr_source_caps == 0) {
		return -EINVAL;
	}

	ret = usbpd_fetch_pdo(pdpm->pdo);
	if (ret) {
		pr_err("Failed to fetch pdo info\n");
		return -EINVAL;
	}

	for (i = 0; i < PDO_MAX_OBJECTS; i++) {
		if (pdpm->pdo[i].type == PDO_TYPE_APDO
			&& pdpm->pdo[i].pps && pdpm->pdo[i].pos) {
			if (pdpm->pdo[i].max_volt_mv >= pdpm->apdo_max_volt
					&& pdpm->pdo[i].curr_ma >= pdpm->apdo_max_curr
					&& pdpm->pdo[i].max_volt_mv <= APDO_MAX_VOLT) {
				pdpm->apdo_max_volt = pdpm->pdo[i].max_volt_mv;
				pdpm->apdo_max_curr = pdpm->pdo[i].curr_ma;
				pdpm->apdo_selected_pdo = pdpm->pdo[i].pos;
			}
		}
	}

	pdbg("PPS supported, preferred APDO pos:%d, max volt:%d, current:%d\n",
				pdpm->apdo_selected_pdo,
				pdpm->apdo_max_volt,
				pdpm->apdo_max_curr);

	if (pdpm->apdo_max_curr <= LOW_POWER_PPS_CURR_THR)
		pdpm->apdo_max_curr = XIAOMI_LOW_POWER_PPS_CURR_MAX;

	return 0;
}

static void usbpd_pm_disconnect(struct usbpd_pm *pdpm)
{
	cancel_delayed_work_sync(&pdpm->pm_work);

	pdpm->apdo_selected_pdo = 0;
	/* select default 5V*/
	set_usbpd_pps_enable(pdpm, false);

	if (!pdpm->sw.charge_enabled) {
		set_usbpd_sw_enable(pdpm, true);
	}

	if (pdpm->cp.charge_enabled) {
		set_usbpd_cp_enable(pdpm, false);
	}

	memset(usbpd_source_caps, 0, sizeof(usbpd_source_caps));
	usbpd_nr_source_caps = 0;

	usbpd_pm_move_state(pdpm, PD_PM_STATE_ENTRY);
}

static inline bool usbpd_temp_check(struct usbpd_pm *pdpm)
{
	int rc;
	int temperature = 0;

	rc = get_usbpd_bat_temperature(pdpm, &temperature);
	if (rc < 0) {
		dev_err(pdpm->dev, "failed get battery temperature\n");
		return false;
	}
	
	pdbg("usbpd: battery temperature is %d\n", temperature);
	
	if ((temperature < 100) || (temperature > 450)) {
		pr_info("usbpd: fc_enable = 0, battery temperature is %d\n", temperature);
		return false;
	}

	return true;
}

static void usb_psy_change_work(struct work_struct *work)
{
	struct usbpd_pm *pdpm = container_of(work, struct usbpd_pm,
					usb_psy_change_work);
	static bool old_pps_status = false;

	pdpm->pd_active = get_usbpd_pps_enabled(pdpm);
	if (pdpm->pd_active != old_pps_status) {
		pr_info("\n\n\npd_active = %d\n\n\n", pdpm->pd_active);
		old_pps_status = pdpm->pd_active;
		if (pdpm->pd_active) {
			if (usbpd_pm_evaluate_src_caps(pdpm) == 0)
				schedule_delayed_work(&pdpm->pm_work, 0);
		} else {
			usbpd_pm_disconnect(pdpm);
		}
	}
}

static int usbpd_psy_notifier_cb(struct notifier_block *nb,
			unsigned long event, void *data)
{
	struct usbpd_pm *pdpm = container_of(nb, struct usbpd_pm, nb);
	struct power_supply *psy = data;
	unsigned long flags;

//	pdbg("usbpd: %s enter, psy: %s\n", __func__, psy->desc->name);

	if (event != PSY_EVENT_PROP_CHANGED)
		return NOTIFY_OK;

	if (psy == pdpm->tcpm_psy) {
		spin_lock_irqsave(&pdpm->psy_change_lock, flags);
		schedule_work(&pdpm->usb_psy_change_work);
		spin_unlock_irqrestore(&pdpm->psy_change_lock, flags);
	}

	return NOTIFY_OK;
}

static int usbpd_pm_sm(struct usbpd_pm *pdpm)
{
	struct pdpm_config *config = pdpm->config;
	struct sw_device *sw = &pdpm->sw;
	struct cp_device *cp = &pdpm->cp;
	int capacity = 0, batt_temp = 0;
	static int tune_vbus_retry;
	static int cool_overcharge_timer;
	static int fc2_taper_timer;
	static int ibus_lmt_change_timer;
	static bool stop_sw;
	static bool recover;
	static int curr_fcc_limit;
	static int curr_ibus_limit;
	static int ibus_limit;
	int step_vbat = 0;
	int step_ibus = 0;
	int step_ibat = 0;
	int steps;
	int rc = 0;
	int	new_voltage;
	int	new_current;

	switch (pdpm->state) {
	case PD_PM_STATE_ENTRY:
		stop_sw = false;
		recover = false;

		rc = get_usbpd_bat_capacity(pdpm, &capacity);
		if ((rc < 0) && (capacity >= 95)) {
			pr_info("usbpd: fc_enable = 0, battery capacity is %d\n", capacity);
			break;
		}

		if ((cp->vbat_volt < 3500) || (cp->vbat_volt > 4400)) {
			pr_info("usbpd: fc_enable = 0, battery voltage is %d\n", cp->vbat_volt);
			break;
		}

		rc = get_usbpd_bat_temperature(pdpm, &batt_temp);		
		if ((rc < 0) && ((batt_temp <= 100) || (batt_temp >= 450))) {
			pr_info("usbpd: fc_enable = 0, battery temperature is %d\n", batt_temp);
			break;
		}

		pdbg("usbpd: battery capacity is %d\n", capacity);
		pdbg("usbpd: battery temperature is %d\n", batt_temp);
		pdbg("usbpd: battery voltage is %d\n", cp->vbat_volt);

		usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY);
		break;
	case PD_PM_STATE_FC2_ENTRY:
		usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_1);
		break;
	case PD_PM_STATE_FC2_ENTRY_1:
		pdpm->request_voltage = cp->vbat_volt * 2 + BUS_VOLT_INIT_UP;
		pdpm->request_current = min(pdpm->apdo_max_curr, pdpm->apdo_max_curr);
		pdpm->request_current -= 50;

		set_usbpd_pps_enable(pdpm, true);
		set_usbpd_pps_voltage(pdpm, pdpm->request_voltage * 1000);
		set_usbpd_pps_current(pdpm, pdpm->request_current * 1000);
		pdbg("request_voltage:%d, request_current:%d\n",
					pdpm->request_voltage, pdpm->request_current);

		usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_2);
		tune_vbus_retry = 0;
		break;
	case PD_PM_STATE_FC2_ENTRY_2:
		pdbg("tune adapter volt %d , vbatt %d\n",
					cp->vbus_volt, cp->vbat_volt);

		if (cp->vbus_volt < (cp->vbat_volt * 2 + BUS_VOLT_INIT_UP - 50)) {
			tune_vbus_retry++;
			pdpm->request_voltage += STEP_MV;
			set_usbpd_pps_voltage(pdpm, pdpm->request_voltage * 1000);
		} else if (cp->vbus_volt > (cp->vbat_volt * 2 + BUS_VOLT_INIT_UP + 200)) {
			tune_vbus_retry++;
			pdpm->request_voltage -= STEP_MV;
			set_usbpd_pps_voltage(pdpm, pdpm->request_voltage * 1000);
		} else {
			pdbg("adapter volt tune ok, retry %d times\n", tune_vbus_retry);
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_ENTRY_3);

			break;
		}

		if (tune_vbus_retry > 60) {
			pr_info("Failed to tune adapter volt into valid range, charge with switching charger\n");
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		}

		break;
	case PD_PM_STATE_FC2_ENTRY_3:
		if (!cp->charge_enabled) {
			pdbg("try to trun on cp ...\n");
			set_usbpd_cp_enable(pdpm, true);
			break;
		}

		pdbg("cp enable = %d\n", cp->charge_enabled);
		pdbg("sw enable = %d\n", sw->charge_enabled);
		if (cp->charge_enabled) {
			if (sw->charge_enabled) {
				pdbg("try to trun off sw ...\n");
				set_usbpd_sw_enable(pdpm, false);
			}
			ibus_lmt_change_timer = 0;
			fc2_taper_timer = 0;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_TUNE);
		} else {
			pr_info("can't enable cp, stop fast charge\n");
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		}
		break;
	case PD_PM_STATE_FC2_TUNE:
		/* battery overheat, stop charge*/
		if (cp->bat_therm_fault) {
			pr_info("bat_therm_fault:%d\n", pdpm->cp.bat_therm_fault);
			pr_info("Move to stop charging\n");
			stop_sw = true;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			break;
		}

		rc = get_usbpd_bat_temperature(pdpm, &batt_temp);
		if ((rc < 0) && ((batt_temp < 100) || (batt_temp > 450))) {
			pr_info("thermal level too high or batt temp is out of fc2 range\n");
			pr_info("Move to switch charging, will try to recover flash charging\n");
			recover = true;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			break;
		}

		if (cp->bat_ovp_fault || cp->bat_ocp_fault ||
						cp->bus_ovp_fault || cp->bus_ocp_fault) {
			pr_info("bat_ocp_fault:%d, bus_ocp_fault:%d, bat_ovp_fault:%d, bus_ovp_fault:%d\n",
					pdpm->cp.bat_ocp_fault, pdpm->cp.bus_ocp_fault,
					pdpm->cp.bat_ovp_fault, pdpm->cp.bus_ovp_fault);
			pr_info("Move to switch charging\n");
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			break;
		}

		if (!cp->charge_enabled) {
			pr_info("cp->charge_enabled:%d\n", cp->charge_enabled);
			pr_info("Move to switch charging, will try to recover flash charging\n");
			recover = true;
			usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
		}

		/*check overcharge when it is cool*/
		if ((cp->vbat_volt > config->bat_volt_lp_lmt) &&
					(batt_temp < 150)) {
			if (cool_overcharge_timer++ > TAPER_TIMEOUT) {
				pr_info("cool overcharge\n");
				cool_overcharge_timer = 0;
				pr_info("Move to switch charging\n");
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			}
			break;
		}
		cool_overcharge_timer = 0;

		/* charge pump taper charge */
		if ((cp->vbat_volt > config->bat_volt_lp_lmt - TAPER_VOL_HYS)
				&& (cp->ibat_curr < config->fc2_taper_current)) {
			if (fc2_taper_timer++ > TAPER_TIMEOUT) {
				pr_info("charge pump taper charging done\n");
				fc2_taper_timer = 0;
				pr_info("Move to switch charging\n");
				usbpd_pm_move_state(pdpm, PD_PM_STATE_FC2_EXIT);
			}
			break;
		}
		fc2_taper_timer = 0;

		curr_fcc_limit = config->bat_curr_lp_lmt;
		curr_ibus_limit = curr_fcc_limit >> 1;
		pdbg("curr_ibus_limit:%d\n", curr_ibus_limit);
		/* curr_ibus_limit should compare with apdo_max_curr here */
		curr_ibus_limit = min(curr_ibus_limit, pdpm->apdo_max_curr);

		ibus_limit = curr_ibus_limit;
		/* reduce bus current in cv loop */
		if (cp->vbat_volt > config->bat_volt_lp_lmt - BQ_TAPER_HYS_MV) {
			if (ibus_lmt_change_timer++ > IBUS_CHANGE_TIMEOUT) {
				ibus_lmt_change_timer = 0;
				ibus_limit = curr_ibus_limit - 100;
			}
		} else if (cp->vbat_volt < config->bat_volt_lp_lmt - 250) {
			ibus_limit = curr_ibus_limit + 100;
			ibus_lmt_change_timer = 0;
		} else {
			ibus_lmt_change_timer = 0;
		}
		pdbg("ibus_limit:%d\n", ibus_limit);

		/* battery voltage loop*/
		if (cp->vbat_volt > config->bat_volt_lp_lmt)
			step_vbat = -1;
		else if (cp->vbat_volt < config->bat_volt_lp_lmt - 10)
			step_vbat = 1;
		
		/* battery charge current loop*/
		if (cp->ibat_curr < curr_fcc_limit)
			step_ibat = 1;
		else if (cp->ibat_curr > curr_fcc_limit + 50)
			step_ibat = -1;

		pdbg("vbus_mv: %d\n", cp->vbus_volt);
		pdbg("vbat_mv: %d\n", cp->vbat_volt);
		pdbg("ibus_ma: %d\n", cp->ibus_curr);
		pdbg("ibat_ma: %d\n", cp->ibat_curr);

		if (cp->ibus_curr < ibus_limit - 50)
			step_ibus = 1;
		else if (cp->ibus_curr > ibus_limit)
			step_ibus = -1;
		pdbg("step_ibus:%d\n", step_ibus);

		steps = min(min(step_vbat, step_ibus), step_ibat);

		new_voltage = pdpm->request_voltage + steps * STEP_MV;		
		new_current = min(pdpm->apdo_max_curr, curr_ibus_limit);

		pdbg("steps: %d, pdpm->request_voltage: %d\n", steps, new_voltage);

		if (new_voltage > pdpm->apdo_max_volt - PPS_VOL_HYS)
			new_voltage = pdpm->apdo_max_volt - PPS_VOL_HYS;

		pdbg("request_voltage:%d, request_current:%d\n",
				new_voltage, new_current);

		pdpm->request_voltage = new_voltage;
		set_usbpd_pps_voltage(pdpm, pdpm->request_voltage * 1000);

		if (new_current != pdpm->request_current) {
			pdpm->request_current = new_current;
			set_usbpd_pps_current(pdpm, pdpm->request_current * 1000);
		}

		break;
	case PD_PM_STATE_FC2_EXIT:
		/* select default 5V*/
		set_usbpd_pps_enable(pdpm, false);

		pdbg("stop_sw:%d, sw->charge_enabled:%d\n",
					stop_sw, sw->charge_enabled);
		if (!stop_sw && (!sw->charge_enabled)) {
			set_usbpd_sw_enable(pdpm, true);
		}

		if (stop_sw && (sw->charge_enabled))
			set_usbpd_sw_enable(pdpm, false);

		if (cp->charge_enabled) {
			set_usbpd_cp_enable(pdpm, false);
		}

		if (recover)
			usbpd_pm_move_state(pdpm, PD_PM_STATE_ENTRY);
		else
			rc = 1;

		break;
	default:
		usbpd_pm_move_state(pdpm, PD_PM_STATE_ENTRY);
		break;
	}

	return rc;
}

static void usbpd_pm_workfunc(struct work_struct *work)
{
	struct usbpd_pm *pdpm = container_of(work, struct usbpd_pm,
					pm_work.work);
	int internal = PM_WORK_RUN_NORMAL_INTERVAL;

	usbpd_pm_update_sw_status(pdpm);
	usbpd_pm_update_cp_status(pdpm);

	if (!usbpd_pm_sm(pdpm) && pdpm->pd_active) {
		if (pdpm->state == PD_PM_STATE_FC2_ENTRY_2)
			internal = PM_WORK_RUN_QUICK_INTERVAL;
		else
			internal = PM_WORK_RUN_NORMAL_INTERVAL;

		schedule_delayed_work(&pdpm->pm_work,
				msecs_to_jiffies(internal));
	}
}

static int usbpd_parse_dt(struct usbpd_pm *pdpm)
{
	struct device *dev = pdpm->dev;
	struct device_node *node = dev->of_node;
	struct pdpm_config *config;
	unsigned int val;
	int rc = 0;

	config = devm_kzalloc(dev, sizeof(struct pdpm_config),
					GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	pdpm->config = config;

	rc = of_property_read_u32(node, "mi,pd-bat-volt-max", &val);
	if (rc < 0) {
		pr_err("pd-bat-volt-max property missing, use default val\n");
		config->bat_volt_lp_lmt = BAT_VOLT_LOOP_LMT;
	} else {
		config->bat_volt_lp_lmt = val;
	}
	pdbg("bat_volt_lp_lmt : %d\n", config->bat_volt_lp_lmt);

	rc = of_property_read_u32(node, "mi,pd-bat-curr-max", &val);
	if (rc < 0) {
		pr_err("pd-bat-curr-max property missing, use default val\n");
		config->bat_curr_lp_lmt = BAT_CURR_LOOP_LMT;
	} else {
		config->bat_curr_lp_lmt = val;
	}
	pdbg("pm_config.bat_curr_lp_lmt:%d\n", config->bat_curr_lp_lmt);

	rc = of_property_read_u32(node, "mi,pd-bus-curr-max", &val);
	if (rc < 0) {
		pr_err("pd-bus-curr-max property missing, use default val\n");
		config->bus_curr_lp_lmt = BUS_CURR_LOOP_LMT;
	} else {
		config->bus_curr_lp_lmt = val;
	}
	pr_info("pm_config.bus_curr_lp_lmt:%d\n", config->bus_curr_lp_lmt);

	return rc;
}

static int usbpd_pm_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct device *dev = &pdev->dev;
	struct usbpd_pm *pdpm;

	pdpm = devm_kzalloc(dev, sizeof(struct usbpd_pm), GFP_KERNEL);
	if (!pdpm)
		return -ENOMEM;

	pdpm->dev = dev;

	ret = usbpd_parse_dt(pdpm);
	if (ret < 0) {
		pr_err("Couldn't parse device tree rc=%d\n", ret);
		return ret;
	}

	pdpm->sw_psy = power_supply_get_by_name("pm8150b-charger");
	if (!pdpm->sw_psy) {
		dev_err(&pdev->dev, "Cannot find power supply \"pm8150b-charger\"\n");
		return -ENODEV;
	}

	pdpm->fg_psy = power_supply_get_by_name("bms");
	if (!pdpm->fg_psy) {
		dev_err(&pdev->dev, "Cannot find power supply \"bms\"\n");
		return -ENODEV;
	}

	pdpm->cp_psy = power_supply_get_by_name("ln8000-charger");
	if (!pdpm->cp_psy) {
		dev_err(&pdev->dev, "Cannot find power supply \"ln8000-charger\"\n");
		return -ENODEV;
	}

	pdpm->cp_batt_psy = power_supply_get_by_name("ln8000-battery");
	if (!pdpm->cp_batt_psy) {
		dev_err(&pdev->dev, "Cannot find power supply \"ln8000-battery\"\n");
		return -ENODEV;
	}

	pdpm->tcpm_psy = power_supply_get_by_name(
			"tcpm-source-psy-c440000.spmi:pmic@2:typec@1500");
	if (!pdpm->tcpm_psy) {
		dev_err(&pdev->dev, "Cannot find power supply \"tcpm-source-psy\"\n");
		return -ENODEV;
	}

	platform_set_drvdata(pdev, pdpm);

	pdpm->config->fc2_taper_current = 2300;

	INIT_WORK(&pdpm->usb_psy_change_work, usb_psy_change_work);
	INIT_DELAYED_WORK(&pdpm->pm_work, usbpd_pm_workfunc);

	pdpm->nb.notifier_call = usbpd_psy_notifier_cb;
	power_supply_reg_notifier(&pdpm->nb);
#if 0
	ret = set_usbpd_pps_enable(pdpm, true);
	if (ret < 0) {
		pr_err("Couldn't active pps, rc=%d\n", ret);
		return ret;
	}
#endif
	dev_info(&pdev->dev, "usbpd manager registered!\n");

	return ret;
}

static int usbpd_pm_remove(struct platform_device *pdev)
{
	struct usbpd_pm *pdpm = platform_get_drvdata(pdev);

	power_supply_unreg_notifier(&pdpm->nb);
	cancel_delayed_work(&pdpm->pm_work);
	cancel_work(&pdpm->usb_psy_change_work);

	return 0;
}

static const struct of_device_id usbpd_pm_of_match[] = {
	{ .compatible = "xiaomi,usbpd-pm", },
	{},
};
MODULE_DEVICE_TABLE(of, usbpd_pm_of_match);

static struct platform_driver usbpd_pm_driver = {
	.driver = {
		.name = "usbpd-pm",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(usbpd_pm_of_match),
	},
	.probe = usbpd_pm_probe,
	.remove = usbpd_pm_remove,
};

static int __init usbpd_pm_init(void)
{
	return platform_driver_register(&usbpd_pm_driver);
}

late_initcall(usbpd_pm_init);

static void __exit usbpd_pm_exit(void)
{
	return platform_driver_unregister(&usbpd_pm_driver);
}
module_exit(usbpd_pm_exit);

MODULE_AUTHOR("maverick jia<maverick_jia@sina.com>");
MODULE_AUTHOR("Fei Jiang<jiangfei1@xiaomi.com>");
MODULE_DESCRIPTION("Xiaomi usbpd statemachine for bq");
MODULE_LICENSE("GPL");
