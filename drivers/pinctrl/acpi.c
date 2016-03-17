/*
 * ACPI integration for the pin control subsystem
 *
 * Copyright (c) 2016, Intel Corporation.
 *
 * Derived from:
 *  devicetree.c - Copyright (C) 2012 NVIDIA CORPORATION
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifdef CONFIG_GENERIC_PINCONF

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf-generic.h>

#include "acpi.h"
#include "core.h"
#include "pinconf.h"
#include "pinctrl-utils.h"

/**
 * struct pinctrl_acpi_map - mapping table chunk parsed from ACPI
 * @node: list node for struct pinctrl's ACPI data field
 * @pctldev: the pin controller that allocated this struct, and will free it
 * @maps: the mapping table entries
 * @num_maps: number of mapping table entries
 */
struct pinctrl_acpi_map {
	struct list_head node;
	struct pinctrl_dev *pctldev;
	struct pinctrl_map *map;
	unsigned num_maps;
};

struct acpi_gpio_lookup {
	unsigned int index;
	bool found;
	unsigned int n;
	struct pinctrl *p;
	char *statename;
};

/* For now we only handle acpi pin config values */
#define ACPI_MAX_CFGS 1

static void acpi_maps_list_dh(acpi_handle handle, void *data)
{
	/* The address of this function is used as a key. */
}

static struct list_head *acpi_get_maps(struct device *dev)
{
	acpi_handle handle = ACPI_HANDLE(dev);
	struct list_head *maps;
	acpi_status status;

	status = acpi_get_data(handle, acpi_maps_list_dh, (void **)&maps);
	if (ACPI_FAILURE(status))
		return NULL;

	return maps;
}

static void acpi_free_maps(struct device *dev, struct list_head *maps)
{
	acpi_handle handle = ACPI_HANDLE(dev);

	acpi_detach_data(handle, acpi_maps_list_dh);
	kfree(maps);
}

static int acpi_init_maps(struct device *dev)
{
	acpi_handle handle = ACPI_HANDLE(dev);
	struct list_head *maps;
	acpi_status status;
	int ret;

	maps = kzalloc(sizeof(*maps), GFP_KERNEL);
	if (!maps)
		return -ENOMEM;

	INIT_LIST_HEAD(maps);

	status = acpi_attach_data(handle, acpi_maps_list_dh, maps);
	if (ACPI_FAILURE(status)) {
		ret = -EINVAL;
		goto err_free_maps;
	}

	return 0;

err_free_maps:
	kfree(maps);
	return ret;
}

void pinctrl_acpi_free_maps(struct pinctrl *p)
{
	struct pinctrl_acpi_map *map, *_map;
	struct list_head *maps;

	maps = acpi_get_maps(p->dev);
	if (!maps)
		goto out;

	list_for_each_entry_safe(map, _map, maps, node) {
		pinctrl_unregister_map(map->map);
		list_del(&map->node);
		pinctrl_utils_free_map(map->pctldev, map->map, map->num_maps);
		kfree(map);
	}

	acpi_free_maps(p->dev, maps);
out:
	acpi_bus_put_acpi_device(ACPI_COMPANION(p->dev));
}

static int acpi_remember_or_free_map(struct pinctrl *p, const char *statename,
				     struct pinctrl_dev *pctldev,
				     struct pinctrl_map *map, unsigned num_maps)
{
	struct pinctrl_acpi_map *acpi_map;
	struct list_head *acpi_maps;
	unsigned i;

	acpi_maps = acpi_get_maps(p->dev);
	if (!acpi_maps) {
		pinctrl_utils_free_map(pctldev, map, num_maps);
		return -EINVAL;
	}

	/* Initialize common mapping table entry fields */
	for (i = 0; i < num_maps; i++) {
		map[i].dev_name = dev_name(p->dev);
		map[i].name = statename;
		if (pctldev)
			map[i].ctrl_dev_name = dev_name(pctldev->dev);
	}

	/* Remember the converted mapping table entries */
	acpi_map = kzalloc(sizeof(*acpi_map), GFP_KERNEL);
	if (!acpi_map) {
		pinctrl_utils_free_map(pctldev, map, num_maps);
		return -ENOMEM;
	}

	acpi_map->pctldev = pctldev;
	acpi_map->map = map;
	acpi_map->num_maps = num_maps;
	list_add_tail(&acpi_map->node, acpi_maps);

	return pinctrl_register_map(map, num_maps, false);
}

static int acpi_parse_gpio_config(const struct acpi_resource_gpio *agpio,
				  unsigned long **configs,
				  unsigned int *nconfigs)
{
	enum pin_config_param param;
	int ret;

	/* Parse configs from GpioInt/GpioIo ACPI resource */
	*nconfigs = 0;
	*configs = kcalloc(ACPI_MAX_CFGS, sizeof(**configs), GFP_KERNEL);
	if (!*configs)
		return -ENOMEM;

	/* For now, only parse pin_config */
	switch (agpio->pin_config) {
	case ACPI_PIN_CONFIG_DEFAULT:
		param = PIN_CONFIG_BIAS_PULL_PIN_DEFAULT;
		break;
	case ACPI_PIN_CONFIG_PULLUP:
		param = PIN_CONFIG_BIAS_PULL_UP;
		break;
	case ACPI_PIN_CONFIG_PULLDOWN:
		param = PIN_CONFIG_BIAS_PULL_DOWN;
		break;
	case ACPI_PIN_CONFIG_NOPULL:
		param = PIN_CONFIG_BIAS_DISABLE;
		break;
	default:
		ret = -EINVAL;
		goto exit_free;
	}
	*configs[*nconfigs] = pinconf_to_config_packed(param,
				      param == PIN_CONFIG_BIAS_DISABLE ? 0 : 1);
	(*nconfigs)++;

	return 0;

exit_free:
	kfree(*configs);
	return ret;
}

static int acpi_gpio_to_map(struct acpi_resource *ares, void *data)
{
	unsigned num_maps = 0, reserved_maps = 0;
	struct acpi_gpio_lookup *lookup = data;
	const struct acpi_resource_gpio *agpio;
	acpi_handle pctrl_handle = NULL;
	struct pinctrl_map *map = NULL;
	struct pinctrl_dev *pctldev;
	unsigned int nconfigs, i;
	unsigned long *configs;
	acpi_status status;
	const char *pin;
	int ret;

	if (ares->type != ACPI_RESOURCE_TYPE_GPIO)
		return 1;
	if (lookup->n++ != lookup->index || lookup->found)
		return 1;

	agpio = &ares->data.gpio;

	/* Get configs from ACPI GPIO resource */
	ret = acpi_parse_gpio_config(agpio, &configs, &nconfigs);
	if (ret)
		return ret;

	/* Get pinctrl reference from GPIO resource */
	status = acpi_get_handle(NULL, agpio->resource_source.string_ptr,
				 &pctrl_handle);
	if (ACPI_FAILURE(status) || !pctrl_handle) {
		ret = -EINVAL;
		goto exit_free_configs;
	}

	/* Find the pin controller */
	pctldev = get_pinctrl_dev_from_acpi(pctrl_handle);
	if (!pctldev) {
		ret = -EINVAL;
		goto exit_free_configs;
	}

	/* Allocate space for maps and pinctrl_dev references */
	ret = pinctrl_utils_reserve_map(pctldev, &map, &reserved_maps,
					&num_maps, agpio->pin_table_length);
	if (ret < 0)
		goto exit_free_configs;

	/* For each GPIO pin */
	for (i = 0; i < agpio->pin_table_length; i++) {
		pin = pin_get_name(pctldev, agpio->pin_table[i]);
		if (!pin) {
			ret = -EINVAL;
			goto exit_free_map;
		}
		ret = pinctrl_utils_add_map_configs(pctldev, &map,
						    &reserved_maps,
						    &num_maps, pin,
						    configs, nconfigs,
						    PIN_MAP_TYPE_CONFIGS_PIN);
		if (ret < 0)
			goto exit_free_map;
	}

	ret = acpi_remember_or_free_map(lookup->p, lookup->statename, pctldev,
					map, num_maps);
	if (ret < 0)
		goto exit_free_maps;

	lookup->found = true;
	kfree(configs);
	return 1;

exit_free_maps:
	pinctrl_acpi_free_maps(lookup->p);
exit_free_map:
	pinctrl_utils_free_map(NULL, map, num_maps);
exit_free_configs:
	kfree(configs);
	return ret;
}

static int acpi_parse_gpio_resources(struct pinctrl *p, char *statename)
{
	struct acpi_gpio_lookup lookup;
	struct list_head res_list;
	struct acpi_device *adev;
	unsigned int index;
	int ret;

	adev = ACPI_COMPANION(p->dev);

	memset(&lookup, 0, sizeof(lookup));
	lookup.p = p;
	lookup.statename = statename;

	/* Parse all GpioInt/GpioIo resources in _CRS and extract pin conf */
	for (index = 0; ; index++) {
		lookup.index = index;
		lookup.n = 0;
		lookup.found = false;

		INIT_LIST_HEAD(&res_list);
		ret = acpi_dev_get_resources(adev, &res_list, acpi_gpio_to_map,
					     &lookup);
		if (ret < 0)
			goto exit_free_maps;
		acpi_dev_free_resource_list(&res_list);
		if (!lookup.found)
			break;
	}

	return 0;

exit_free_maps:
	pinctrl_acpi_free_maps(p);
	return ret;
}

static int acpi_remember_dummy_state(struct pinctrl *p, const char *statename)
{
	struct pinctrl_map *map;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	/* There is no pctldev for PIN_MAP_TYPE_DUMMY_STATE */
	map->type = PIN_MAP_TYPE_DUMMY_STATE;

	return acpi_remember_or_free_map(p, statename, NULL, map, 1);
}

static struct pinctrl_dev *acpi_find_pctldev(struct fwnode_handle *fw_config)
{
	struct acpi_buffer path = {ACPI_ALLOCATE_BUFFER, NULL};
	acpi_handle pctrl_handle, cfg_handle;
	struct acpi_data_node *dn;
	acpi_status status;
	int ret;

	/*
	 * In ACPI, the pinctrl device is the parent of the configuration
	 * node. In the kernel internal representation, the device node is
	 * the parent of the configuration node. We need to extract the
	 * original path for the configuration node and search for its parent
	 * in the ACPI hierarchy.
	 */
	dn = to_acpi_data_node(fw_config);
	if (!dn)
		return ERR_PTR(-EINVAL);

	ret = acpi_get_name(dn->handle, ACPI_FULL_PATHNAME, &path);
	if (ret)
		return ERR_PTR(ret);

	status = acpi_get_handle(NULL, (char *)path.pointer, &cfg_handle);
	kfree(path.pointer);
	if (ACPI_FAILURE(status))
		return ERR_PTR(-EINVAL);

	status = acpi_get_parent(cfg_handle, &pctrl_handle);
	if (ACPI_FAILURE(status))
		return ERR_PTR(-EINVAL);

	return get_pinctrl_dev_from_acpi(pctrl_handle);
}

static int acpi_to_map_one_config(struct pinctrl *p, const char *statename,
				  struct fwnode_handle *fw_config)
{
	struct pinctrl_map *map;
	struct pinctrl_dev *pctldev;
	unsigned num_maps;
	int ret;

	/* Find the pin controller containing fw_config */
	pctldev = acpi_find_pctldev(fw_config);
	if (!pctldev)
		return -ENODEV;
	if (IS_ERR(pctldev))
		return PTR_ERR(pctldev);

	/* Parse ACPI node and generate mapping table entries */
	ret = pinconf_generic_fwnode_to_map(pctldev, fw_config, &map, &num_maps,
					    PIN_MAP_TYPE_INVALID);
	if (ret < 0)
		return ret;

	/* Stash the mapping table chunk away for later use */
	return acpi_remember_or_free_map(p, statename, pctldev, map, num_maps);
}

static struct fwnode_handle *acpi_find_config_prop(struct device *dev,
						   char *propname)
{
	struct fwnode_handle *child;
	struct acpi_data_node *dn;

	/*
	 * Pinctrl configuration properties are described with ACPI data
	 * nodes using _DSD Hierarchical Properties Extension.
	 */
	device_for_each_child_node(dev, child) {
		dn = to_acpi_data_node(child);
		if (!dn)
			continue;
		if (!strcmp(dn->name, propname))
			break;
	}

	return child;
}

int pinctrl_acpi_to_map(struct pinctrl *p)
{
	const union acpi_object *prop, *statenames, *configs;
	unsigned int state, nstates, nconfigs, config;
	char *statename, *propname, *configname;
	struct fwnode_handle *fw_prop;
	struct acpi_device *adev;
	int ret;

	/* We may store pointers to property names within the node */
	adev = acpi_bus_get_acpi_device(ACPI_HANDLE(p->dev));
	if (!adev)
		return -ENODEV;

	/* Only allow named states (device must have prop 'pinctrl-names') */
	ret = acpi_dev_get_property(adev, "pinctrl-names", ACPI_TYPE_PACKAGE,
				    &prop);
	if (ret) {
		acpi_bus_put_acpi_device(adev);
		/* No pinctrl properties */
		return 0;
	}
	statenames = prop->package.elements;
	nstates = prop->package.count;

	ret = acpi_init_maps(p->dev);
	if (ret)
		return ret;

	/* For each defined state ID */
	for (state = 0; state < nstates; state++) {
		/* Get state name */
		if (statenames[state].type != ACPI_TYPE_STRING) {
			ret = -EINVAL;
			goto err_free_maps;
		}
		statename = statenames[state].string.pointer;

		/*
		 * Parse any GpioInt/GpioIo resources and
		 * associate them with the 'default' state.
		 */
		if (!strcmp(statename, PINCTRL_STATE_DEFAULT)) {
			ret = acpi_parse_gpio_resources(p, statename);
			if (ret) {
				dev_err(p->dev,
					"Could not parse GPIO resources\n");
				goto err_free_maps;
			}
		}

		/* Retrieve the pinctrl-* property */
		propname = kasprintf(GFP_KERNEL, "pinctrl-%d", state);
		ret = acpi_dev_get_property(adev, propname, ACPI_TYPE_PACKAGE,
					    &prop);
		kfree(propname);
		if (ret)
			break;
		configs = prop->package.elements;
		nconfigs = prop->package.count;

		/* For every referenced pin configuration node in it */
		for (config = 0; config < nconfigs; config++) {
			if (configs[config].type != ACPI_TYPE_STRING) {
				ret = -EINVAL;
				goto err_free_maps;
			}
			configname = configs[config].string.pointer;

			/*
			 * Look up the pin configuration node as
			 * an ACPI data node in the device node.
			 */
			fw_prop = acpi_find_config_prop(p->dev, configname);
			if (!fw_prop) {
				ret = -EINVAL;
				goto err_free_maps;
			}

			/* Parse the configuration node */
			ret = acpi_to_map_one_config(p, statename, fw_prop);
			if (ret < 0)
				goto err_free_maps;
		}
		/* No entries in ACPI? Generate a dummy state table entry */
		if (!nconfigs) {
			ret = acpi_remember_dummy_state(p, statename);
			if (ret < 0)
				goto err_free_maps;
		}
	}

	return 0;

err_free_maps:
	pinctrl_acpi_free_maps(p);
	return ret;
}
#endif
