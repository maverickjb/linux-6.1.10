#ifndef _LINUX_USBPD_MANAGER_H
#define _LINUX_USBPD_MANAGER_H

#if 1
#define pdbg(fmt, args...)    pr_info("[%s] %s %d: " fmt, KBUILD_MODNAME, __func__, __LINE__, ##args)
#else
#define pdbg(fmt, args...)
#endif

#define PPS_VOL_HYS			1000

#define STEP_MV			20
#define TAPER_VOL_HYS			80
#define BQ_TAPER_HYS_MV			10

#define GENERATE_ENUM(e)	e
#define GENERATE_STRING(s)	#s

#define FOREACH_STATE(S)			\
	S(PD_PM_STATE_ENTRY),			\
	S(PD_PM_STATE_FC2_ENTRY),			\
	S(PD_PM_STATE_FC2_ENTRY_1),			\
	S(PD_PM_STATE_FC2_ENTRY_2),			\
	S(PD_PM_STATE_FC2_ENTRY_3),			\
	S(PD_PM_STATE_FC2_TUNE),			\
	S(PD_PM_STATE_FC2_EXIT)

enum pm_state {
	FOREACH_STATE(GENERATE_ENUM)
};

static const char * const pm_states[] = {
	FOREACH_STATE(GENERATE_STRING)
};

struct pdpm_config {
	int bat_volt_lp_lmt; /*bat volt loop limit*/
	int	bat_curr_lp_lmt;

	int	fc2_taper_current;
};

struct sw_device {
	bool charge_enabled;
};

struct cp_device {
	bool charge_enabled;
	int  vbat_volt;
	int  vbus_volt;
	int  ibat_curr;
	int  ibus_curr;

	bool bat_ovp_fault;
	bool bat_ocp_fault;
	bool bat_therm_fault;
	bool bus_ovp_fault;
	bool bus_ocp_fault;
	bool bus_therm_fault;

	bool vbus_pres;
};

struct usbpd_pm {
	struct device *dev;

	enum pm_state state;

	struct notifier_block nb;

	bool online;
	bool pd_active;

	int	request_voltage;
	int	request_current;
	int	apdo_selected_pdo;

	int	apdo_max_volt;
	int	apdo_max_curr;

	struct delayed_work pm_work;
	struct work_struct usb_psy_change_work;
	spinlock_t psy_change_lock;

	struct power_supply *sw_psy;
	struct power_supply *fg_psy;	
	struct power_supply *cp_psy;
	struct power_supply *cp_batt_psy;
	struct power_supply *tcpm_psy;

	struct pdpm_config *config;

	struct sw_device sw;
	struct cp_device cp;
};

#endif
