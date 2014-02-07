/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <errno.h>
#include <unistd.h>

#include <libudev.h>

#include "usbip_common.h"
#include "usbip_host_driver.h"

#undef  PROGNAME
#define PROGNAME "libusbip"

struct usbip_host_driver *host_driver;
struct udev *udev_context;

static int32_t read_attr_usbip_status(struct usbip_usb_device *udev)
{
	char status_attr_path[SYSFS_PATH_MAX];
	int fd;
	int length;
	char status;
	int value = 0;

	snprintf(status_attr_path, SYSFS_PATH_MAX, "%s/usbip_status",
		 udev->path);

	if ((fd = open(status_attr_path, O_RDONLY)) < 0) {
                dbg("Error opening attribute %s.", status_attr_path);
                return -1; 
        } 

	length = read(fd, &status, 1);
	if (length < 0) {
                dbg("Error reading attribute %s.", status_attr_path);
		close(fd);
		return -1;	
	}

	value = atoi(&status);

	return value;
}

static struct usbip_exported_device *usbip_exported_device_new(char *sdevpath)
{
	struct usbip_exported_device *edev = NULL;
	size_t size;
	int i;

	edev = calloc(1, sizeof(struct usbip_exported_device));

	edev->sudev = udev_device_new_from_syspath(udev_context, sdevpath);
	if (!edev->sudev) {
		dbg("udev_device_new_from_syspath: %s", sdevpath);
		goto err;
	}

	read_usb_device(edev->sudev, &edev->udev);

	edev->status = read_attr_usbip_status(&edev->udev);
	if (edev->status < 0)
		goto err;

	/* reallocate buffer to include usb interface data */
	size = sizeof(struct usbip_exported_device) + edev->udev.bNumInterfaces *
		sizeof(struct usbip_usb_interface);

	edev = realloc(edev, size);

	for (i = 0; i < edev->udev.bNumInterfaces; i++)
		read_usb_interface(&edev->udev, i, &edev->uinf[i]);

	return edev;
err:
	if (edev->sudev)
		udev_device_unref(edev->sudev);
	if (edev)
		free(edev);

	return NULL;
}

static int check_new(struct dlist *dlist, struct sysfs_device *target)
{
	struct sysfs_device *dev;

	dlist_for_each_data(dlist, dev, struct sysfs_device) {
		if (!strncmp(dev->bus_id, target->bus_id, SYSFS_BUS_ID_SIZE))
			/* device found and is not new */
			return 0;
	}
	return 1;
}

static void delete_nothing(void *unused_data)
{
	/*
	 * NOTE: Do not delete anything, but the container will be deleted.
	 */
	(void) unused_data;
}

static int refresh_exported_devices(void)
{
	/* sysfs_device of usb_device */
	struct sysfs_device	*sudev;
	struct dlist		*sudev_list;
	struct dlist		*sudev_unique_list;
	struct usbip_exported_device *edev;

	sudev_unique_list = dlist_new_with_delete(sizeof(struct sysfs_device),
						  delete_nothing);

	sudev_list = sysfs_get_driver_devices(host_driver->sysfs_driver);

	if (!sudev_list) {
		/*
		 * Not an error condition. There are simply no devices bound to
		 * the driver yet.
		 */
		dbg("bind " USBIP_HOST_DRV_NAME ".ko to a usb device to be "
		    "exportable!");
		return 0;
	}

	dlist_for_each_data(sudev_list, sudev, struct sysfs_device)
		if (check_new(sudev_unique_list, sudev))
			dlist_unshift(sudev_unique_list, sudev);

	dlist_for_each_data(sudev_unique_list, sudev, struct sysfs_device) {
		edev = usbip_exported_device_new(sudev->path);

		if (!edev) {
			dbg("usbip_exported_device_new failed");
			continue;
		}

		dlist_unshift(host_driver->edev_list, edev);
		host_driver->ndevs++;
	}

	dlist_destroy(sudev_unique_list);

	return 0;
}

static struct sysfs_driver *open_sysfs_host_driver(void)
{
	char bus_type[] = "usb";
	char sysfs_mntpath[SYSFS_PATH_MAX];
	char host_drv_path[SYSFS_PATH_MAX];
	struct sysfs_driver *host_drv;
	int rc;

	rc = sysfs_get_mnt_path(sysfs_mntpath, SYSFS_PATH_MAX);
	if (rc < 0) {
		dbg("sysfs_get_mnt_path failed");
		return NULL;
	}

	snprintf(host_drv_path, SYSFS_PATH_MAX, "%s/%s/%s/%s/%s",
		 sysfs_mntpath, SYSFS_BUS_NAME, bus_type, SYSFS_DRIVERS_NAME,
		 USBIP_HOST_DRV_NAME);

	host_drv = sysfs_open_driver_path(host_drv_path);
	if (!host_drv) {
		dbg("sysfs_open_driver_path failed");
		return NULL;
	}

	return host_drv;
}

static void usbip_exported_device_delete(void *dev)
{
	//struct usbip_exported_device *edev = dev;
	//sysfs_close_device(edev->sudev);
	free(dev);
}

int usbip_host_driver_open(void)
{
	int rc;

	udev_context = udev_new();
	if (!udev_context) {
		dbg("udev_new failed");
		return -1;
	}

	host_driver = calloc(1, sizeof(*host_driver));
	if (!host_driver) {
		dbg("calloc failed");
		return -1;
	}

	host_driver->ndevs = 0;
	host_driver->edev_list =
		dlist_new_with_delete(sizeof(struct usbip_exported_device),
				      usbip_exported_device_delete);
	if (!host_driver->edev_list) {
		dbg("dlist_new_with_delete failed");
		goto err_free_host_driver;
	}

	host_driver->sysfs_driver = open_sysfs_host_driver();
	if (!host_driver->sysfs_driver)
		goto err_destroy_edev_list;

	rc = refresh_exported_devices();
	if (rc < 0)
		goto err_close_sysfs_driver;

	return 0;

err_close_sysfs_driver:
	sysfs_close_driver(host_driver->sysfs_driver);
err_destroy_edev_list:
	dlist_destroy(host_driver->edev_list);
err_free_host_driver:
	free(host_driver);
	host_driver = NULL;

	udev_unref(udev_context);

	return -1;
}

void usbip_host_driver_close(void)
{
	if (!host_driver)
		return;

	if (host_driver->edev_list)
		dlist_destroy(host_driver->edev_list);
	if (host_driver->sysfs_driver)
		sysfs_close_driver(host_driver->sysfs_driver);

	free(host_driver);
	host_driver = NULL;

	udev_unref(udev_context);
}

int usbip_host_refresh_device_list(void)
{
	int rc;

	if (!udev_context) {
		dbg("udev_new failed");
		return -1;
	}

	if (host_driver->edev_list)
		dlist_destroy(host_driver->edev_list);

	host_driver->ndevs = 0;
	host_driver->edev_list =
		dlist_new_with_delete(sizeof(struct usbip_exported_device),
				      usbip_exported_device_delete);
	if (!host_driver->edev_list) {
		dbg("dlist_new_with_delete failed");
		return -1;
	}

	rc = refresh_exported_devices();
	if (rc < 0)
		return -1;

	return 0;
}

int usbip_host_export_device(struct usbip_exported_device *edev, int sockfd)
{
	char attr_name[] = "usbip_sockfd";
	char attr_path[SYSFS_PATH_MAX];
	struct sysfs_attribute *attr;
	char sockfd_buff[30];
	int ret;

	if (edev->status != SDEV_ST_AVAILABLE) {
		dbg("device not available: %s", edev->udev.busid);
		switch (edev->status) {
		case SDEV_ST_ERROR:
			dbg("status SDEV_ST_ERROR");
			break;
		case SDEV_ST_USED:
			dbg("status SDEV_ST_USED");
			break;
		default:
			dbg("status unknown: 0x%x", edev->status);
		}
		return -1;
	}

	/* only the first interface is true */
	snprintf(attr_path, sizeof(attr_path), "%s/%s",
		 edev->udev.path, attr_name);

	attr = sysfs_open_attribute(attr_path);
	if (!attr) {
		dbg("sysfs_open_attribute failed: %s", attr_path);
		return -1;
	}

	snprintf(sockfd_buff, sizeof(sockfd_buff), "%d\n", sockfd);
	dbg("write: %s", sockfd_buff);

	ret = sysfs_write_attribute(attr, sockfd_buff, strlen(sockfd_buff));
	if (ret < 0) {
		dbg("sysfs_write_attribute failed: sockfd %s to %s",
		    sockfd_buff, attr_path);
		goto err_write_sockfd;
	}

	dbg("connect: %s", edev->udev.busid);

err_write_sockfd:
	sysfs_close_attribute(attr);

	return ret;
}

struct usbip_exported_device *usbip_host_get_device(int num)
{
	struct usbip_exported_device *edev;
	struct dlist *dlist = host_driver->edev_list;
	int cnt = 0;

	dlist_for_each_data(dlist, edev, struct usbip_exported_device) {
		if (num == cnt)
			return edev;
		else
			cnt++;
	}

	return NULL;
}
