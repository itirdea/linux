/*
 * Core driver for the generic pin config portions of the pin control subsystem
 *
 * Copyright (C) 2011 ST-Ericsson SA
 * Written on behalf of Linaro for ST-Ericsson
 *
 * Author: Linus Walleij <linus.walleij@linaro.org>
 *
 * License terms: GNU General Public License (GPL) version 2
 */

#define pr_fmt(fmt) "generic pinconfig core: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include "core.h"
#include "pinconf.h"
#include "pinctrl-utils.h"

#ifdef CONFIG_DEBUG_FS
static const struct pin_config_item conf_items[] = {
	PCONFDUMP(PIN_CONFIG_BIAS_BUS_HOLD, "input bias bus hold", NULL, false),
	PCONFDUMP(PIN_CONFIG_BIAS_DISABLE, "input bias disabled", NULL, false),
	PCONFDUMP(PIN_CONFIG_BIAS_HIGH_IMPEDANCE, "input bias high impedance", NULL, false),
	PCONFDUMP(PIN_CONFIG_BIAS_PULL_DOWN, "input bias pull down", NULL, false),
	PCONFDUMP(PIN_CONFIG_BIAS_PULL_PIN_DEFAULT,
				"input bias pull to pin specific state", NULL, false),
	PCONFDUMP(PIN_CONFIG_BIAS_PULL_UP, "input bias pull up", NULL, false),
	PCONFDUMP(PIN_CONFIG_DRIVE_OPEN_DRAIN, "output drive open drain", NULL, false),
	PCONFDUMP(PIN_CONFIG_DRIVE_OPEN_SOURCE, "output drive open source", NULL, false),
	PCONFDUMP(PIN_CONFIG_DRIVE_PUSH_PULL, "output drive push pull", NULL, false),
	PCONFDUMP(PIN_CONFIG_DRIVE_STRENGTH, "output drive strength", "mA", true),
	PCONFDUMP(PIN_CONFIG_INPUT_DEBOUNCE, "input debounce", "usec", true),
	PCONFDUMP(PIN_CONFIG_INPUT_ENABLE, "input enabled", NULL, false),
	PCONFDUMP(PIN_CONFIG_INPUT_SCHMITT, "input schmitt trigger", NULL, false),
	PCONFDUMP(PIN_CONFIG_INPUT_SCHMITT_ENABLE, "input schmitt enabled", NULL, false),
	PCONFDUMP(PIN_CONFIG_LOW_POWER_MODE, "pin low power", "mode", true),
	PCONFDUMP(PIN_CONFIG_OUTPUT, "pin output", "level", true),
	PCONFDUMP(PIN_CONFIG_POWER_SOURCE, "pin power source", "selector", true),
	PCONFDUMP(PIN_CONFIG_SLEW_RATE, "slew rate", NULL, true),
};

static void pinconf_generic_dump_one(struct pinctrl_dev *pctldev,
				     struct seq_file *s, const char *gname,
				     unsigned pin,
				     const struct pin_config_item *items,
				     int nitems)
{
	int i;

	for (i = 0; i < nitems; i++) {
		unsigned long config;
		int ret;

		/* We want to check out this parameter */
		config = pinconf_to_config_packed(items[i].param, 0);
		if (gname)
			ret = pin_config_group_get(dev_name(pctldev->dev),
						   gname, &config);
		else
			ret = pin_config_get_for_pin(pctldev, pin, &config);
		/* These are legal errors */
		if (ret == -EINVAL || ret == -ENOTSUPP)
			continue;
		if (ret) {
			seq_printf(s, "ERROR READING CONFIG SETTING %d ", i);
			continue;
		}
		/* Space between multiple configs */
		seq_puts(s, " ");
		seq_puts(s, items[i].display);
		/* Print unit if available */
		if (items[i].has_arg) {
			seq_printf(s, " (%u",
				   pinconf_to_config_argument(config));
			if (items[i].format)
				seq_printf(s, " %s)", items[i].format);
			else
				seq_puts(s, ")");
		}
	}
}

/**
 * pinconf_generic_dump_pins - Print information about pin or group of pins
 * @pctldev:	Pincontrol device
 * @s:		File to print to
 * @gname:	Group name specifying pins
 * @pin:	Pin number specyfying pin
 *
 * Print the pinconf configuration for the requested pin(s) to @s. Pins can be
 * specified either by pin using @pin or by group using @gname. Only one needs
 * to be specified the other can be NULL/0.
 */
void pinconf_generic_dump_pins(struct pinctrl_dev *pctldev, struct seq_file *s,
			       const char *gname, unsigned pin)
{
	const struct pinconf_ops *ops = pctldev->desc->confops;

	if (!ops->is_generic)
		return;

	/* generic parameters */
	pinconf_generic_dump_one(pctldev, s, gname, pin, conf_items,
				 ARRAY_SIZE(conf_items));
	/* driver-specific parameters */
	if (pctldev->desc->num_custom_params &&
	    pctldev->desc->custom_conf_items)
		pinconf_generic_dump_one(pctldev, s, gname, pin,
					 pctldev->desc->custom_conf_items,
					 pctldev->desc->num_custom_params);
}

void pinconf_generic_dump_config(struct pinctrl_dev *pctldev,
				 struct seq_file *s, unsigned long config)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(conf_items); i++) {
		if (pinconf_to_config_param(config) != conf_items[i].param)
			continue;
		seq_printf(s, "%s: 0x%x", conf_items[i].display,
			   pinconf_to_config_argument(config));
	}

	if (!pctldev->desc->num_custom_params ||
	    !pctldev->desc->custom_conf_items)
		return;

	for (i = 0; i < pctldev->desc->num_custom_params; i++) {
		if (pinconf_to_config_param(config) !=
		    pctldev->desc->custom_conf_items[i].param)
			continue;
		seq_printf(s, "%s: 0x%x",
				pctldev->desc->custom_conf_items[i].display,
				pinconf_to_config_argument(config));
	}
}
EXPORT_SYMBOL_GPL(pinconf_generic_dump_config);
#endif

#if defined(CONFIG_OF) || defined(CONFIG_ACPI)
static const struct pinconf_generic_params fw_params[] = {
	{ "bias-bus-hold", PIN_CONFIG_BIAS_BUS_HOLD, 0 },
	{ "bias-disable", PIN_CONFIG_BIAS_DISABLE, 0 },
	{ "bias-high-impedance", PIN_CONFIG_BIAS_HIGH_IMPEDANCE, 0 },
	{ "bias-pull-up", PIN_CONFIG_BIAS_PULL_UP, 1 },
	{ "bias-pull-pin-default", PIN_CONFIG_BIAS_PULL_PIN_DEFAULT, 1 },
	{ "bias-pull-down", PIN_CONFIG_BIAS_PULL_DOWN, 1 },
	{ "drive-open-drain", PIN_CONFIG_DRIVE_OPEN_DRAIN, 0 },
	{ "drive-open-source", PIN_CONFIG_DRIVE_OPEN_SOURCE, 0 },
	{ "drive-push-pull", PIN_CONFIG_DRIVE_PUSH_PULL, 0 },
	{ "drive-strength", PIN_CONFIG_DRIVE_STRENGTH, 0 },
	{ "input-debounce", PIN_CONFIG_INPUT_DEBOUNCE, 0 },
	{ "input-disable", PIN_CONFIG_INPUT_ENABLE, 0 },
	{ "input-enable", PIN_CONFIG_INPUT_ENABLE, 1 },
	{ "input-schmitt", PIN_CONFIG_INPUT_SCHMITT, 0 },
	{ "input-schmitt-disable", PIN_CONFIG_INPUT_SCHMITT_ENABLE, 0 },
	{ "input-schmitt-enable", PIN_CONFIG_INPUT_SCHMITT_ENABLE, 1 },
	{ "low-power-disable", PIN_CONFIG_LOW_POWER_MODE, 0 },
	{ "low-power-enable", PIN_CONFIG_LOW_POWER_MODE, 1 },
	{ "output-high", PIN_CONFIG_OUTPUT, 1, },
	{ "output-low", PIN_CONFIG_OUTPUT, 0, },
	{ "power-source", PIN_CONFIG_POWER_SOURCE, 0 },
	{ "slew-rate", PIN_CONFIG_SLEW_RATE, 0 },
};

/**
 * parse_fwnode_cfg() - Parse FW pinconf parameters
 * @fwnode:	FW node
 * @params:	Array of describing generic parameters
 * @count:	Number of entries in @params
 * @cfg:	Array of parsed config options
 * @ncfg:	Number of entries in @cfg
 *
 * Parse the config options described in @params from @fwnode and puts the result
 * in @cfg. @cfg does not need to be empty, entries are added beggining at
 * @ncfg. @ncfg is updated to reflect the number of entries after parsing. @cfg
 * needs to have enough memory allocated to hold all possible entries.
 */
static void parse_fwnode_cfg(struct fwnode_handle *fwnode,
			     const struct pinconf_generic_params *params,
			     unsigned int count, unsigned long *cfg,
			     unsigned int *ncfg)
{
	int i;

	for (i = 0; i < count; i++) {
		u32 val;
		int ret;
		const struct pinconf_generic_params *par = &params[i];

		ret = fwnode_property_read_u32(fwnode, par->property, &val);

		/* property not found */
		if (ret == -EINVAL)
			continue;

		/* use default value, when no value is specified */
		if (ret)
			val = par->default_value;

		pr_debug("found %s with value %u\n", par->property, val);
		cfg[*ncfg] = pinconf_to_config_packed(par->param, val);
		(*ncfg)++;
	}
}

/**
 * pinconf_generic_parse_fwnode_config()
 * parse the config properties into generic pinconfig values.
 * @fwnode: node containing the pinconfig properties
 * @configs: array with nconfigs entries containing the generic pinconf values
 *           must be freed when no longer necessary.
 * @nconfigs: umber of configurations
 */
static int pinconf_generic_parse_fwnode_config(struct fwnode_handle *fwnode,
					       struct pinctrl_dev *pctldev,
					       unsigned long **configs,
					       unsigned int *nconfigs)
{
	unsigned long *cfg;
	unsigned int max_cfg, ncfg = 0;
	int ret;

	if (!fwnode)
		return -EINVAL;

	/* allocate a temporary array big enough to hold one of each option */
	max_cfg = ARRAY_SIZE(fw_params);
	if (pctldev)
		max_cfg += pctldev->desc->num_custom_params;
	cfg = kcalloc(max_cfg, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	parse_fwnode_cfg(fwnode, fw_params, ARRAY_SIZE(fw_params), cfg, &ncfg);
	if (pctldev && pctldev->desc->num_custom_params &&
		pctldev->desc->custom_params)
		parse_fwnode_cfg(fwnode, pctldev->desc->custom_params,
				 pctldev->desc->num_custom_params, cfg, &ncfg);

	ret = 0;

	/* no configs found at all */
	if (ncfg == 0) {
		*configs = NULL;
		*nconfigs = 0;
		goto out;
	}

	/*
	 * Now limit the number of configs to the real number of
	 * found properties.
	 */
	*configs = kmemdup(cfg, ncfg * sizeof(unsigned long), GFP_KERNEL);
	if (!*configs) {
		ret = -ENOMEM;
		goto out;
	}

	*nconfigs = ncfg;

out:
	kfree(cfg);
	return ret;
}

static int pinconf_generic_fwnode_subnode_to_map(struct pinctrl_dev *pctldev,
		struct fwnode_handle *fwnode, struct pinctrl_map **map,
		unsigned *reserved_maps, unsigned *num_maps,
		enum pinctrl_map_type type)
{
	const char *function;
	struct device *dev = pctldev->dev;
	unsigned long *configs = NULL;
	unsigned num_configs = 0;
	unsigned reserve, strings_count;
	const char **groups;
	const char *subnode_target_type = "pins";
	int ret, i;

	ret = fwnode_property_read_string_array(fwnode, "pins", NULL, 0);
	if (ret < 0) {
		ret = fwnode_property_read_string_array(fwnode, "groups", NULL, 0);
		if (ret < 0)
			/* skip this node; may contain config child nodes */
			return 0;
		if (type == PIN_MAP_TYPE_INVALID)
			type = PIN_MAP_TYPE_CONFIGS_GROUP;
		subnode_target_type = "groups";
	} else {
		if (type == PIN_MAP_TYPE_INVALID)
			type = PIN_MAP_TYPE_CONFIGS_PIN;
	}
	strings_count = ret;

	ret = fwnode_property_read_string(fwnode, "function", &function);
	if (ret < 0) {
		/* EINVAL=missing, which is fine since it's optional */
		if (ret != -EINVAL)
			dev_err(dev, "%s: could not parse property function\n",
				fwnode_get_name(fwnode));
		function = NULL;
	}

	ret = pinconf_generic_parse_fwnode_config(fwnode, pctldev, &configs,
						  &num_configs);
	if (ret < 0) {
		dev_err(dev, "%s: could not parse node property\n",
			fwnode_get_name(fwnode));
		return ret;
	}

	reserve = 0;
	if (function != NULL)
		reserve++;
	if (num_configs)
		reserve++;

	reserve *= strings_count;

	ret = pinctrl_utils_reserve_map(pctldev, map, reserved_maps,
			num_maps, reserve);
	if (ret < 0)
		goto exit_free_configs;

	groups = kcalloc(strings_count, sizeof(*groups), GFP_KERNEL);
	if (!groups) {
		ret = -ENOMEM;
		goto exit_free_configs;
	}

	ret = fwnode_property_read_string_array(fwnode, subnode_target_type,
						groups, strings_count);
	if (ret < 0)
		goto exit_free_groups;

	for (i = 0; i < strings_count; i++) {
		if (function) {
			ret = pinctrl_utils_add_map_mux(pctldev, map,
					reserved_maps, num_maps, groups[i],
					function);
			if (ret < 0)
				goto exit_free_groups;
		}

		if (num_configs) {
			ret = pinctrl_utils_add_map_configs(pctldev, map,
					reserved_maps, num_maps, groups[i],
					configs, num_configs, type);
			if (ret < 0)
				goto exit_free_groups;
		}
	}
	ret = 0;

exit_free_groups:
	kfree(groups);
exit_free_configs:
	kfree(configs);
	return ret;
}

int pinconf_generic_fwnode_to_map(struct pinctrl_dev *pctldev,
		struct fwnode_handle *fwnode, struct pinctrl_map **map,
		unsigned *num_maps, enum pinctrl_map_type type)
{
	unsigned reserved_maps;
	struct fwnode_handle *fw;
	int ret;

	reserved_maps = 0;
	*map = NULL;
	*num_maps = 0;

	ret = pinconf_generic_fwnode_subnode_to_map(pctldev, fwnode, map,
						&reserved_maps, num_maps, type);
	if (ret < 0)
		goto exit;

	fwnode_for_each_child_node(fwnode, fw) {
		ret = pinconf_generic_fwnode_subnode_to_map(pctldev, fw, map,
					&reserved_maps, num_maps, type);
		if (ret < 0)
			goto exit;
	}
	return 0;

exit:
	pinctrl_utils_free_map(pctldev, *map, *num_maps);
	return ret;
}
EXPORT_SYMBOL_GPL(pinconf_generic_fwnode_to_map);

#endif

#ifdef CONFIG_OF
int pinconf_generic_parse_dt_config(struct device_node *np,
				    struct pinctrl_dev *pctldev,
				    unsigned long **configs,
				    unsigned int *nconfigs)
{
	return pinconf_generic_parse_fwnode_config(&np->fwnode, pctldev,
						   configs, nconfigs);
}
#endif
