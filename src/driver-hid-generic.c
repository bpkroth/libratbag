/*
 * Copyright © 2024 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <errno.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>

#include "libratbag-private.h"
#include "libratbag-data.h"
#include "libratbag-hidraw.h"

/**
 * Generic HID driver for simple devices that don't require special protocols.
 *
 * This driver is designed for devices where:
 * - Button remapping is handled by the kernel HID driver
 * - No special DPI or LED configuration is needed initially
 * - The device uses standard HID reports
 * - Button count can be read from HID report descriptor or overridden in .device file
 *
 * Typical use cases:
 * - Elecom mice/trackballs (with kernel hid-elecom driver)
 * - Simple gaming mice that don't need advanced features
 * - Devices where we only want basic button visibility/remapping
 */

#define HID_GENERIC_REPORT_RATE_DEFAULT 1000

struct hidgeneric_data {
	unsigned int num_profiles;
	unsigned int num_buttons;
};

static void
hidgeneric_read_button(struct ratbag_button *button)
{
	/* Enable basic button action types.
	 * Actual button mapping is handled by the kernel, we just
	 * provide visibility and allow remapping through evdev. */
	ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_BUTTON);
	ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_KEY);
	ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_SPECIAL);

	/* Set default action: button N -> button N */
	button->action.type = RATBAG_BUTTON_ACTION_TYPE_BUTTON;
	button->action.action.button = button->index + 1;
}

static void
hidgeneric_read_profile(struct ratbag_profile *profile)
{
	struct ratbag_button *button;
	static const unsigned int report_rate = HID_GENERIC_REPORT_RATE_DEFAULT;

	/* This is the only active profile */
	profile->is_active = true;
	profile->is_enabled = true;

	/* Set a fixed report rate (read-only, not written back to device) */
	ratbag_profile_set_report_rate_list(profile, &report_rate, 1);
	profile->hz = report_rate;

	/* Read all buttons */
	ratbag_profile_for_each_button(profile, button)
		hidgeneric_read_button(button);
}

static unsigned int
hidgeneric_count_buttons_from_evdev(struct ratbag_device *device)
{
	struct udev *udev = device->ratbag->udev;
	struct udev_device *udev_device = device->udev_device;
	struct udev_device *parent;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *dev_list_entry;
	struct libevdev *evdev = NULL;
	unsigned int num_buttons = 0;
	const char *syspath;
	int fd = -1;
	int rc;

	/* Find the parent input device */
	parent = udev_device_get_parent_with_subsystem_devtype(udev_device,
							       "hid",
							       NULL);
	if (!parent) {
		log_debug(device->ratbag, "Failed to find input parent\n");
		return 0;
	}

	syspath = udev_device_get_syspath(parent);
	if (!syspath) {
		log_debug(device->ratbag, "Failed to get parent syspath\n");
		return 0;
	}

	/* Enumerate event devices under this input device */
	enumerate = udev_enumerate_new(udev);
	if (!enumerate)
		return 0;

	udev_enumerate_add_match_parent(enumerate, parent);
	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);
	udev_list_entry_foreach(dev_list_entry, devices) {
		struct udev_device *dev;
		const char *devnode;
		const char *devpath = udev_list_entry_get_name(dev_list_entry);

		dev = udev_device_new_from_syspath(udev, devpath);
		if (!dev)
			continue;

		devnode = udev_device_get_devnode(dev);
		if (!devnode || strstr(devnode, "/event") == NULL) {
			udev_device_unref(dev);
			continue;
		}

		/* Try to open the event device and count buttons */
		fd = open(devnode, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			udev_device_unref(dev);
			continue;
		}

		rc = libevdev_new_from_fd(fd, &evdev);
		if (rc < 0) {
			close(fd);
			udev_device_unref(dev);
			continue;
		}

		/* Count mouse buttons (BTN_MOUSE to BTN_JOYSTICK) */
		for (unsigned int i = BTN_MOUSE; i < BTN_JOYSTICK; i++) {
			if (libevdev_has_event_code(evdev, EV_KEY, i))
				num_buttons++;
		}

		libevdev_free(evdev);
		close(fd);
		udev_device_unref(dev);

		/* We found an event device, use it */
		if (num_buttons > 0)
			break;
	}

	udev_enumerate_unref(enumerate);

	if (num_buttons > 0) {
		log_debug(device->ratbag,
			  "Detected %d buttons from evdev\n",
			  num_buttons);
	}

	return num_buttons;
}

static int
hidgeneric_test_hidraw(struct ratbag_device *device)
{
	/* Accept any device that has a hidraw interface.
	 * We're a generic fallback driver, so we're not picky. */
	return true;
}

static int
hidgeneric_probe(struct ratbag_device *device)
{
	struct hidgeneric_data *drv_data;
	struct ratbag_profile *profile;
	int num_buttons;
	int rc;

	/* Try to open hidraw device */
	rc = ratbag_find_hidraw(device, hidgeneric_test_hidraw);
	if (rc) {
		log_error(device->ratbag,
			  "hid-generic: failed to open hidraw '%s': %s\n",
			  udev_device_get_syspath(device->udev_device),
			  strerror(-rc));
		return rc;
	}

	drv_data = zalloc(sizeof(*drv_data));

	/* Get number of profiles from device data (default: 1) */
    // TODO: Make this configurable in .device file
	drv_data->num_profiles = 1;

	/* Try to get button count from device data first */
	num_buttons = ratbag_device_data_hidgeneric_get_button_count(device->data);
	if (num_buttons > 0) {
		log_debug(device->ratbag,
			  "hid-generic: using button count from device file: %d\n",
			  num_buttons);
		drv_data->num_buttons = num_buttons;
	} else {
		/* Fall back to detecting from evdev */
		num_buttons = hidgeneric_count_buttons_from_evdev(device);
		if (num_buttons == 0) {
			/* Fallback to a reasonable default */
            // TODO: Move this to a constant.
            // TODO: Split this based on device type (e.g., mice vs keyboards).
			num_buttons = 5;
			log_debug(device->ratbag,
				  "hid-generic: using default button count: %d\n",
				  num_buttons);
		}
		drv_data->num_buttons = num_buttons;
	}

	ratbag_set_drv_data(device, drv_data);

	/* Initialize device with single profile, no resolutions, and detected buttons */
	ratbag_device_init_profiles(device,
				    drv_data->num_profiles,
				    0,  /* num_resolutions */
				    drv_data->num_buttons,
				    0); /* num_leds */

	/* Read the profile data */
	ratbag_device_for_each_profile(device, profile)
		hidgeneric_read_profile(profile);

	log_info(device->ratbag,
		 "hid-generic: initialized device '%s' with %d buttons\n",
		 ratbag_device_get_name(device),
		 drv_data->num_buttons);

	return 0;
}

static void
hidgeneric_remove(struct ratbag_device *device)
{
	ratbag_close_hidraw(device);
	free(ratbag_get_drv_data(device));
}

struct ratbag_driver hidgeneric_driver = {
	.name = "Generic HID",
	.id = "hid-generic",
	.probe = hidgeneric_probe,
	.remove = hidgeneric_remove,
	.commit = NULL, /* No commit support - read-only for now */
};
