/*
 * bq27510 battery driver
 *
 * Copyright (C) 2008 Rodolfo Giometti <giometti@linux.it>
 * Copyright (C) 2008 Eurotech S.p.A. <info@eurotech.it>
 * Copyright (C) 2010 Konstantin Motov <kmotov@mm-sol.com>
 * Copyright (C) 2010 Dimitar Dimitrov <dddimitrov@mm-sol.com>
 *
 * Based on a previous work by Copyright (C) 2008 Texas Instruments, Inc.
 *
 * This package is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 */
#include <linux/module.h>
#include <linux/param.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/idr.h>
#include <linux/i2c.h>
#include <asm/unaligned.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#define DRIVER_VERSION			"1.0.1"

/*
 * Polling interval, in milliseconds.
 *
 * It is expected that each power supply will call power_supply_changed(),
 * thus alleviating user-space from the need to poll. But BQ27510 cannot
 * raise an interrupt so we're forced to issue change events regularly.
 */
#define T_POLL_MS			2000

#define BQ27510_REG_TEMP		0x06
#define BQ27510_REG_VOLT		0x08
#define BQ27510_REG_RSOC		0x2C /* Relative State-of-Charge */
#define BQ27510_REG_AI			0x14
#define BQ27510_REG_FLAGS		0x0A
#define BQ27510_REG_TTE			0x16
#define BQ27510_REG_TTF			0x18
#define BQ27510_REG_FCC			0x12
#define OFFSET_KELVIN_CELSIUS_DECI	2731
#define CURRENT_OVF_THRESHOLD		((1 << 15) - 1)

#define FLAG_BIT_DSG			0
#define FLAG_BIT_SOCF			1
#define FLAG_BIT_SOC1			2
#define FLAG_BIT_BAT_DET		3
#define FLAG_BIT_WAIT_ID		4
#define FLAG_BIT_OCV_GD			5
#define FLAG_BIT_CHG			8
#define FLAG_BIT_FC			9
#define FLAG_BIT_XCHG			10
#define FLAG_BIT_CHG_INH		11
#define FLAG_BIT_OTD			14
#define FLAG_BIT_OTC			15

#define to_bq27510_device_info(x) container_of((x), \
				struct bq27510_device_info, bat);

/* If the system has several batteries we need a different name for each
 * of them...
 */
static DEFINE_IDR(battery_id);
static DEFINE_MUTEX(battery_mutex);

struct bq27510_device_info;
struct bq27510_access_methods {
	int (*read)(u8 reg, int *rt_value, int b_single,
		struct bq27510_device_info *di);
};

struct bq27510_device_info {
	struct device 			*dev;
	int				id;
	int				voltage_uV;
	int				current_uA;
	int				temp_C;
	int				charge_rsoc;
	int                     	time_to_empty;
	int                     	time_to_full;
	struct bq27510_access_methods	*bus;
	struct power_supply		bat;
	struct timer_list		polling_timer;

	struct i2c_client		*client;
};

static enum power_supply_property bq27510_battery_props[] = {
	/* Battery status - see POWER_SUPPLY_STATUS_* */
	POWER_SUPPLY_PROP_STATUS,
	/* Battery health - see POWER_SUPPLY_HEALTH_* */
	POWER_SUPPLY_PROP_HEALTH,
	/* Battery technology - see POWER_SUPPLY_TECHNOLOGY_* */
	POWER_SUPPLY_PROP_TECHNOLOGY,
	/* Boolean 1 -> battery detected, 0 battery not inserted. */
	POWER_SUPPLY_PROP_PRESENT,
	/* Measured Voltage cell pack in uV. */
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	/*
	 * Battery current in uA.
	 */
	POWER_SUPPLY_PROP_CURRENT_NOW,
	/*
	 * Predicted remaining battery capacity expressed
	 * as a percentage 0 - 100%.
	 */
	POWER_SUPPLY_PROP_CAPACITY,
	/* Battery temperature converted in 0.1 °C. */
	POWER_SUPPLY_PROP_TEMP,
	/*
	 * Time to discharge the battery in minutes based on the
	 * average current. 65535 indicates charging cycle.
	 */
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
	/*
	 * Time to recharge the battery in minutes based on the average
	 * current. 65535 indicates discharging cycle.
	 */
	POWER_SUPPLY_PROP_TIME_TO_FULL_NOW,
	/* Maximum battery charge, µAh */
	POWER_SUPPLY_PROP_CHARGE_FULL
};

static void polling_timer_func(unsigned long di_)
{
	struct bq27510_device_info *di = (void *)di_;

	power_supply_changed(&di->bat);

	mod_timer(&di->polling_timer, jiffies + msecs_to_jiffies(T_POLL_MS));
}


/*
 * Common code for bq27x10 devices
 */
static int bq27x10_read(u8 reg, int *rt_value, int b_single,
			struct bq27510_device_info *di)
{
	int ret;
	ret = di->bus->read(reg, rt_value, b_single, di);
	return ret;
}

/*
 * Return the battery temperature in 0.1 Kelvin degrees
 * Or < 0 if something fails.
 */
static int bq27510_battery_temperature(struct bq27510_device_info *di)
{
	int ret;
	int temp = 0;

	ret = bq27x10_read(BQ27510_REG_TEMP, &temp, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading temperature\n");
		return ret;
	}

	dev_dbg(di->dev, "temperature: %d [0.1K]\n", temp);
	return temp;
}

/*
 * Return the battery Voltage in milivolts
 * Or < 0 if something fails.
 */
static int bq27510_battery_voltage(struct bq27510_device_info *di)
{
	int ret;
	int volt = 0;
	ret = bq27x10_read(BQ27510_REG_VOLT, &volt, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}

	return volt;
}

/*
 * Return the battery average current.
 * Negative means discharging, positive means charging.
 * Or 0 if something fails.
 */
static int bq27510_battery_current(struct bq27510_device_info *di)
{
	int ret;
	int curr = 0;
	/*int flags = 0;*/

	ret = bq27x10_read(BQ27510_REG_AI, &curr, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading current\n");
		return 0;
	}

	/*
	 * In the BQ27510 convention, charging current is positive while
	 * discharging current is negative. So sign-extend the 16-bit
	 * signed integer to the native int type.
	 */
	curr = ((curr > CURRENT_OVF_THRESHOLD) ? (curr - (1 << 16)) : curr);

	dev_dbg(di->dev, "AverageCurrent=%d mA\n", curr);

	return curr;
}

/*
 * Return the battery Relative State-of-Charge
 * Or < 0 if something fails.
 */
static int bq27510_battery_rsoc(struct bq27510_device_info *di)
{
	int ret;
	int rsoc = 0;

	ret = bq27x10_read(BQ27510_REG_RSOC, &rsoc, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading relative State-of-Charge\n");
		return ret;
	}

	return rsoc;
}

/*
 * Battery detected.
 * Rerturn true when batery is present
 */
static int bq27510_battery_supply_prop_present(struct bq27510_device_info *di)
{
	int ret;
	int bat_det_flag = 0;

	ret = bq27x10_read(BQ27510_REG_FLAGS, &bat_det_flag, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return (bat_det_flag & (1 << FLAG_BIT_BAT_DET)) ? 1 : 0;
}

/*
 * Return predicted remaining battery life at the present rate of discharge,
 * in minutes.
 */
static int bq27510_battery_time_to_empty_now(struct bq27510_device_info *di)
{
	int ret;
	int tte = 0;

	ret = bq27x10_read(BQ27510_REG_TTE, &tte, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return tte;
}

/*
 * Return predicted remaining time until the battery reaches full charge,
 * in minutes
 */
static int bq27510_battery_time_to_full_now(struct bq27510_device_info *di)
{
	int ret;
	int ttf = 0;

	ret = bq27x10_read(BQ27510_REG_TTF, &ttf, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return ttf;
}

/*
 * Returns the compensated capacity of the battery when fully charged.
 * Units are mAh
  */
static int bq27510_battery_max_level(struct bq27510_device_info *di)
{
	int ret;
	int bml = 0;

	ret = bq27x10_read(BQ27510_REG_FCC, &bml, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading voltage\n");
		return ret;
	}
	return bml;
}

static int bq27510_battery_status(struct bq27510_device_info *di)
{
	int ret, curr;
	int flags = 0;

	ret = bq27x10_read(BQ27510_REG_FLAGS, &flags, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading status flags (%d)\n", ret);
		return ret;
	}

	curr = bq27510_battery_current(di);

	dev_dbg(di->dev, "Flags=%04x\n", flags);

	if (flags & (1u << FLAG_BIT_FC))
		ret = POWER_SUPPLY_STATUS_FULL;
	else if ((flags & (1u << FLAG_BIT_DSG)) && (curr < 0))
		ret = POWER_SUPPLY_STATUS_DISCHARGING;
	else if ((flags & (1u << FLAG_BIT_CHG)) && (curr > 0))
		ret = POWER_SUPPLY_STATUS_CHARGING;
	else
		ret = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return ret;
}

static int bq27510_battery_health(struct bq27510_device_info *di)
{
	int ret;
	int flags = 0;

	ret = bq27x10_read(BQ27510_REG_FLAGS, &flags, 0, di);
	if (ret) {
		dev_err(di->dev, "error reading health flags (%d)\n", ret);
		return ret;
	}

	if (flags & (1u << FLAG_BIT_OTC))
		ret = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (flags & (1u << FLAG_BIT_OTD))
		ret = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (flags & ((1u << FLAG_BIT_XCHG) | (1u << FLAG_BIT_CHG_INH))) {
		int t = bq27510_battery_temperature(di);
		if (t < OFFSET_KELVIN_CELSIUS_DECI)
			ret = POWER_SUPPLY_HEALTH_COLD;
		else
			ret = POWER_SUPPLY_HEALTH_OVERHEAT;
	} else
		ret = POWER_SUPPLY_HEALTH_GOOD;

	return ret;
}

/*
 * Return reuired battery property or error.
 */
static int bq27510_battery_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	int ret = -EINVAL;

	struct bq27510_device_info *di = to_bq27510_device_info(psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = bq27510_battery_status(di);
		ret = 0;
		if (val->intval < 0)
			val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = bq27510_battery_health(di);
		ret = 0;
		if (val->intval < 0)
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		ret = val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		/* return voltage in uV */
		ret = val->intval = bq27510_battery_voltage(di) * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		/* return positive current in uA */
		ret = val->intval = abs(bq27510_battery_current(di)) * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = val->intval = bq27510_battery_rsoc(di);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = val->intval = bq27510_battery_temperature(di);
		/* convert from 0.1K to 0.1C */
		val->intval -= OFFSET_KELVIN_CELSIUS_DECI;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bq27510_battery_supply_prop_present(di);
		/* Report an absent battery if we can't reach the BQ chip. */
		if (val->intval < 0)
			val->intval = 0;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
		ret = val->intval = bq27510_battery_time_to_empty_now(di);
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_NOW:
		ret = val->intval = bq27510_battery_time_to_full_now(di);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		/* present capacity in uAh */
		ret = val->intval = bq27510_battery_max_level(di) * 1000;
		break;
	default:
		return -EINVAL;
	}

	ret = (ret < 0) ? ret : 0;

	return ret;
}

/*
 * init batery descriptor.
  */
static void bq27510_powersupply_init(struct bq27510_device_info *di)
{
	di->bat.type = POWER_SUPPLY_TYPE_BATTERY;
	di->bat.properties = bq27510_battery_props;
	di->bat.num_properties = ARRAY_SIZE(bq27510_battery_props);
	di->bat.get_property = bq27510_battery_get_property;
	di->bat.external_power_changed = NULL;
}

/*
 * Read by I2C.
 */
static int bq27510_read(u8 reg, int *rt_value, int b_single,
			struct bq27510_device_info *di)
{
	struct i2c_client *client = di->client;
	struct i2c_msg msg[1];
	unsigned char data[2];
	int err;
	if (!client->adapter)
		return -ENODEV;

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 1;
	msg->buf = data;

	data[0] = reg;
	err = i2c_transfer(client->adapter, msg, 1);
	if (err >= 0) {
		if (!b_single)
			msg->len = 2;
		else
			msg->len = 1;

		msg->flags = I2C_M_RD;
		err = i2c_transfer(client->adapter, msg, 1);
		if (err >= 0) {
			if (!b_single)
				*rt_value = data[0]+(data[1]<<8);
			else
				*rt_value = data[0];
			return 0;
		}
	}
	return err;
}

/*
 *
 */
static int bq27510_battery_probe(struct i2c_client *client,
				 const struct i2c_device_id *id)
{
	char *name;
	struct bq27510_device_info *di;
	struct bq27510_access_methods *bus;
	int num;
	int retval = 0;

	/* Get new ID for the new battery device */
	retval = idr_pre_get(&battery_id, GFP_KERNEL);
	if (retval == 0)
		return -ENOMEM;
	mutex_lock(&battery_mutex);
	retval = idr_get_new(&battery_id, client, &num);
	mutex_unlock(&battery_mutex);
	if (retval < 0)
		return retval;
	name = kasprintf(GFP_KERNEL, "bq27510-%d", num);
	if (!name) {
		dev_err(&client->dev, "failed to allocate device name\n");
		retval = -ENOMEM;
		goto batt_failed_1;
	}

	di = kzalloc(sizeof(*di), GFP_KERNEL);
	if (!di) {
		dev_err(&client->dev, "failed to allocate device info data\n");
		retval = -ENOMEM;
		goto batt_failed_2;
	}
	di->id = num;

	bus = kzalloc(sizeof(*bus), GFP_KERNEL);
	if (!bus) {
		dev_err(&client->dev, "failed to allocate access method "
					"data\n");
		retval = -ENOMEM;
		goto batt_failed_3;
	}

	i2c_set_clientdata(client, di);
	di->dev = &client->dev;
	di->bat.name = name;
	bus->read = bq27510_read;
	di->bus = bus;
	di->client = client;

	bq27510_powersupply_init(di);

	retval = power_supply_register(&client->dev, &di->bat);
	if (retval) {
		dev_err(&client->dev, "failed to register battery\n");
		goto batt_failed_4;
	}

	setup_timer(&di->polling_timer, polling_timer_func, (unsigned long)di);
	mod_timer(&di->polling_timer, jiffies + msecs_to_jiffies(T_POLL_MS));

	dev_info(&client->dev, "support ver. %s enabled\n", DRIVER_VERSION);
	return 0;

batt_failed_4:
	kfree(bus);
batt_failed_3:
	kfree(di);
batt_failed_2:
	kfree(name);
batt_failed_1:
	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, num);
	mutex_unlock(&battery_mutex);

	return retval;
}

/*
 *
 */
static int bq27510_battery_remove(struct i2c_client *client)
{
	struct bq27510_device_info *di = i2c_get_clientdata(client);

	del_timer_sync(&di->polling_timer);

	power_supply_unregister(&di->bat);

	kfree(di->bat.name);

	mutex_lock(&battery_mutex);
	idr_remove(&battery_id, di->id);
	mutex_unlock(&battery_mutex);

	kfree(di);

	return 0;
}

/*
 * Module stuff
 */

static const struct i2c_device_id bq27510_id[] = {
	{ "bq27510", 0 },
	{},
};

static struct i2c_driver bq27510_battery_driver = {
	.driver = {
		.name = "bq27510-battery",
	},
	.probe = bq27510_battery_probe,
	.remove = bq27510_battery_remove,
	.id_table = bq27510_id,
};

static int __init bq27510_battery_init(void)
{
	int ret;

	ret = i2c_add_driver(&bq27510_battery_driver);
	if (ret)
		printk(KERN_ERR "Unable to register BQ27510 driver\n");

	return ret;
}
module_init(bq27510_battery_init);

static void __exit bq27510_battery_exit(void)
{
	i2c_del_driver(&bq27510_battery_driver);
}
module_exit(bq27510_battery_exit);

MODULE_AUTHOR("Texas Instruments Inc.");
MODULE_DESCRIPTION("BQ27510 battery monitor driver");
MODULE_LICENSE("GPL");
