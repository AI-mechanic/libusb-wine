/*
 * Linux USB support
 *
 * Copyright (c) 2000-2003 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is covered by the LGPL, read LICENSE for details.
 */

#include <stdlib.h>	/* getenv, etc */
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <dirent.h>

#include "linux.h"
#include "usbi.h"

/*
 * libusb-win32's timeout error (from its error.h), the value Windows
 * programs compare against. Linux's own ETIMEDOUT is 110.
 */
#define ETRANSFER_TIMEDOUT 116

static char usb_path[PATH_MAX + 1] = "";

/*
 * libusb-win32 reports every failure as one of four errors
 * (usb_win_error_to_errno() in its error.c): EINVAL, ETRANSFER_TIMEDOUT,
 * ENOMEM and EIO. Windows programs were written against that set, so
 * Linux's errors are folded into it.
 */
static int win_error(int err)
{
  switch (err) {
  case EINVAL:
    return -EINVAL;
  case ENOMEM:
    return -ENOMEM;
  case ETIMEDOUT:
    return -ETRANSFER_TIMEDOUT;
  default:
    return -EIO;
  }
}

/* Timeouts run on CLOCK_MONOTONIC, so changing the clock cannot stretch them. */
static void deadline_in(struct timespec *ts, int ms)
{
  clock_gettime(CLOCK_MONOTONIC, ts);
  ts->tv_sec += ms / 1000;
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

/* Milliseconds left until ts, rounded up so that a wait never ends early;
   0 once ts has passed. */
static int ms_left(const struct timespec *ts)
{
  struct timespec now;
  long long ns;

  clock_gettime(CLOCK_MONOTONIC, &now);
  ns = (long long)(ts->tv_sec - now.tv_sec) * 1000000000LL
     + (ts->tv_nsec - now.tv_nsec);
  if (ns <= 0)
    return 0;
  ns = (ns + 999999) / 1000000;
  return ns > INT_MAX ? INT_MAX : (int)ns;
}

static int device_open(struct usb_device *dev)
{
  char filename[PATH_MAX + 1];
  int fd;

  snprintf(filename, sizeof(filename) - 1, "%s/%s/%s",
    usb_path, dev->bus->dirname, dev->filename);

  fd = open(filename, O_RDWR);
  if (fd < 0) {
    fd = open(filename, O_RDONLY);
    if (fd < 0)
      USB_ERROR_STR(-errno, "failed to open %s: %s",
	filename, strerror(errno));
  }

  return fd;
}

int usb_os_open(usb_dev_handle *dev)
{
  dev->fd = device_open(dev->device);

  return 0;
}

int usb_os_close(usb_dev_handle *dev)
{
  if (dev->fd < 0)
    return 0;

  if (close(dev->fd) == -1)
    /* Failing trying to close a file really isn't an error, so return 0 */
    USB_ERROR_STR(0, "tried to close device fd %d: %s", dev->fd,
	strerror(errno));

  return 0;
}

LIBUSB_API int usb_set_configuration(usb_dev_handle *dev, int configuration)
{
  int ret;

  /* detach kernel driver for windows program ( Stanson <me@stanson.ch > ) */
  usb_detach_kernel_driver_np(dev, 0);

  ret = ioctl(dev->fd, IOCTL_USB_SETCONFIG, &configuration);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "could not set config %d: %s", configuration,
	strerror(errno));

  dev->config = configuration;

  return 0;
}

LIBUSB_API int usb_claim_interface(usb_dev_handle *dev, int interface)
{
  int ret;

  ret = ioctl(dev->fd, IOCTL_USB_CLAIMINTF, &interface);
  if (ret < 0) {
    int err = errno;

    if (err == EBUSY && usb_debug > 0)
      fprintf(stderr, "Check that you have permissions to write to %s/%s and, if you don't, that you set up hotplug (http://linux-hotplug.sourceforge.net/) correctly.\n", dev->bus->dirname, dev->device->filename);

    USB_ERROR_STR(win_error(err), "could not claim interface %d: %s", interface,
	strerror(err));
  }

  dev->interface = interface;

  return 0;
}

LIBUSB_API int usb_release_interface(usb_dev_handle *dev, int interface)
{
  int ret;

  ret = ioctl(dev->fd, IOCTL_USB_RELEASEINTF, &interface);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "could not release intf %d: %s", interface,
    	strerror(errno));

  dev->interface = -1;

  return 0;
}

LIBUSB_API int usb_set_altinterface(usb_dev_handle *dev, int alternate)
{
  int ret;
  struct usb_setinterface setintf;

  if (dev->interface < 0)
    USB_ERROR(-EINVAL);

  setintf.interface = dev->interface;
  setintf.altsetting = alternate;

  ret = ioctl(dev->fd, IOCTL_USB_SETINTF, &setintf);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "could not set alt intf %d/%d: %s",
	dev->interface, alternate, strerror(errno));

  dev->altsetting = alternate;

  return 0;
}

/*
 * Older kernels refuse a synchronous transfer of more than 16 KB, so
 * bigger ones go in pieces.
 */
#define MAX_READ_WRITE	(16 * 1024)

LIBUSB_API int usb_control_msg(usb_dev_handle *dev, int requesttype, int request,
	int value, int index, char *bytes, int size, int timeout)
{
  struct usb_ctrltransfer ctrl;
  int ret;

  ctrl.bRequestType = requesttype;
  ctrl.bRequest = request;
  ctrl.wValue = value;
  ctrl.wIndex = index;
  ctrl.wLength = size;

  ctrl.data = bytes;
  ctrl.timeout = timeout;

  ret = ioctl(dev->fd, IOCTL_USB_CONTROL, &ctrl);
  if (ret < 0)
    USB_ERROR_STR(win_error(errno), "error sending control message: %s",
	strerror(errno));

  return ret;
}

/*
 * The four synchronous transfers. libusb-win32 does them all alike,
 * whatever the type of the endpoint, and so does the kernel's blocking
 * USBDEVFS_BULK, which picks bulk or interrupt from the endpoint. The old
 * code here submitted interrupt URBs for usb_interrupt_read/write(), which
 * the kernel refuses on bulk endpoints.
 *
 * USBDEVFS_BULK also stays out of the URB completion queue, so it can
 * neither take nor lose a completion that belongs to an asynchronous
 * request another thread has pending. The old code reaped from that
 * shared queue, which broke programs that keep a read pending while they
 * write.
 *
 * As in libusb-win32, a timeout of 0 (or less) means no timeout.
 */
static int usb_sync_transfer(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
  struct usb_bulktransfer bulk;
  struct timespec deadline;
  int done = 0, requested, ret;

  if (timeout > 0)
    deadline_in(&deadline, timeout);

  do {
    requested = size - done;
    if (requested > MAX_READ_WRITE)
      requested = MAX_READ_WRITE;

    bulk.ep = ep;
    bulk.len = requested;
    bulk.data = bytes + done;
    bulk.timeout = 0;			/* the kernel's "no timeout" */
    if (timeout > 0) {
      bulk.timeout = ms_left(&deadline);
      if (!bulk.timeout)
        USB_ERROR_STR(-ETRANSFER_TIMEDOUT, "transfer on ep 0x%02x timed out", ep);
    }

    ret = ioctl(dev->fd, IOCTL_USB_BULK, &bulk);
    if (ret < 0)
      USB_ERROR_STR(win_error(errno), "transfer on ep 0x%02x failed: %s", ep,
	strerror(errno));

    done += ret;
  } while (ret == requested && done < size);

  return done;
}

LIBUSB_API int usb_bulk_write(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
  /* Ensure the endpoint address is correct */
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

LIBUSB_API int usb_bulk_read(usb_dev_handle *dev, int ep, char *bytes, int size,
	int timeout)
{
  /* Ensure the endpoint address is correct */
  ep |= USB_ENDPOINT_IN;
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

LIBUSB_API int usb_interrupt_write(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
  /* Ensure the endpoint address is correct */
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

LIBUSB_API int usb_interrupt_read(usb_dev_handle *dev, int ep, char *bytes,
	int size, int timeout)
{
  /* Ensure the endpoint address is correct */
  ep |= USB_ENDPOINT_IN;
  return usb_sync_transfer(dev, ep, bytes, size, timeout);
}

int usb_os_find_busses(struct usb_bus **busses)
{
  struct usb_bus *fbus = NULL;
  DIR *dir;
  struct dirent *entry;

  dir = opendir(usb_path);
  if (!dir)
    USB_ERROR_STR(-errno, "couldn't opendir(%s): %s", usb_path,
	strerror(errno));

  while ((entry = readdir(dir)) != NULL) {
    struct usb_bus *bus;

    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    if (!strchr("0123456789", entry->d_name[strlen(entry->d_name) - 1])) {
      if (usb_debug >= 2)
        fprintf(stderr, "usb_os_find_busses: Skipping non bus directory %s\n",
		entry->d_name);
      continue;
    }

    bus = malloc(sizeof(*bus));
    if (!bus)
      USB_ERROR(-ENOMEM);

    memset((void *)bus, 0, sizeof(*bus));

    strncpy(bus->dirname, entry->d_name, sizeof(bus->dirname) - 1);
    bus->dirname[sizeof(bus->dirname) - 1] = 0;

    LIST_ADD(fbus, bus);

    if (usb_debug >= 2)
       fprintf(stderr, "usb_os_find_busses: Found %s\n", bus->dirname);
  }

  closedir(dir);

  *busses = fbus;

  return 0;
}

int usb_os_find_devices(struct usb_bus *bus, struct usb_device **devices)
{
  struct usb_device *fdev = NULL;
  DIR *dir;
  struct dirent *entry;
  char dirpath[PATH_MAX + 1];

  snprintf(dirpath, PATH_MAX, "%s/%s", usb_path, bus->dirname);

  dir = opendir(dirpath);
  if (!dir)
    USB_ERROR_STR(-errno, "couldn't opendir(%s): %s", dirpath,
	strerror(errno));

  while ((entry = readdir(dir)) != NULL) {
    unsigned char device_desc[DEVICE_DESC_LENGTH];
    char filename[PATH_MAX + 1];
    struct usb_device *dev;
    struct usb_connectinfo connectinfo;
    int i, fd, ret;

    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    dev = malloc(sizeof(*dev));
    if (!dev)
      USB_ERROR(-ENOMEM);

    memset((void *)dev, 0, sizeof(*dev));

    dev->bus = bus;

    strncpy(dev->filename, entry->d_name, sizeof(dev->filename) - 1);
    dev->filename[sizeof(dev->filename) - 1] = 0;

    snprintf(filename, sizeof(filename) - 1, "%s/%s", dirpath, entry->d_name);
    fd = open(filename, O_RDWR);
    if (fd < 0) {
      fd = open(filename, O_RDONLY);
      if (fd < 0) {
        if (usb_debug >= 2)
          fprintf(stderr, "usb_os_find_devices: Couldn't open %s\n",
                  filename);

        free(dev);
        continue;
      }
    }

    /* Get the device number */
    ret = ioctl(fd, IOCTL_USB_CONNECTINFO, &connectinfo);
    if (ret < 0) {
      if (usb_debug)
        fprintf(stderr, "usb_os_find_devices: couldn't get connect info\n");
    } else
      dev->devnum = connectinfo.devnum;

    ret = read(fd, (void *)device_desc, DEVICE_DESC_LENGTH);
    if (ret < 0) {
      if (usb_debug)
        fprintf(stderr, "usb_os_find_devices: Couldn't read descriptor\n");

      free(dev);

      goto err;
    }

    /*
     * Linux kernel converts the words in this descriptor to CPU endian, so
     * we use the undocumented W character for usb_parse_descriptor() that
     * doesn't convert endianess when parsing the descriptor
     */
    usb_parse_descriptor(device_desc, "bbWbbbbWWWbbbb", &dev->descriptor);

    LIST_ADD(fdev, dev);

    if (usb_debug >= 2)
      fprintf(stderr, "usb_os_find_devices: Found %s on %s\n",
		dev->filename, bus->dirname);

    /* Now try to fetch the rest of the descriptors */
    if (dev->descriptor.bNumConfigurations > USB_MAXCONFIG)
      /* Silent since we'll try again later */
      goto err;

    if (dev->descriptor.bNumConfigurations < 1)
      /* Silent since we'll try again later */
      goto err;

    dev->config = (struct usb_config_descriptor *)malloc(dev->descriptor.bNumConfigurations * sizeof(struct usb_config_descriptor));
    if (!dev->config)
      /* Silent since we'll try again later */
      goto err;

    memset(dev->config, 0, dev->descriptor.bNumConfigurations *
          sizeof(struct usb_config_descriptor));

    for (i = 0; i < dev->descriptor.bNumConfigurations; i++) {
      unsigned char buffer[8], *bigbuffer;
      struct usb_config_descriptor config;

      /* Get the first 8 bytes so we can figure out what the total length is */
      ret = read(fd, (void *)buffer, 8);
      if (ret < 8) {
        if (usb_debug >= 1) {
          if (ret < 0)
            fprintf(stderr, "Unable to get descriptor (%d)\n", ret);
          else
            fprintf(stderr, "Config descriptor too short (expected %d, got %d)\n", 8, ret);
        }

        goto err;
      }

      usb_parse_descriptor(buffer, "bbw", &config);

      bigbuffer = malloc(config.wTotalLength);
      if (!bigbuffer) {
        if (usb_debug >= 1)
          fprintf(stderr, "Unable to allocate memory for descriptors\n");
        goto err;
      }

      /* Read the rest of the config descriptor */
      memcpy(bigbuffer, buffer, 8);

      ret = read(fd, (void *)(bigbuffer + 8), config.wTotalLength - 8);
      if (ret < config.wTotalLength - 8) {
        if (usb_debug >= 1) {
          if (ret < 0)
            fprintf(stderr, "Unable to get descriptor (%d)\n", ret);
          else
            fprintf(stderr, "Config descriptor too short (expected %d, got %d)\n", config.wTotalLength, ret);
        }

        free(bigbuffer);
        goto err;
      }

      ret = usb_parse_configuration(&dev->config[i], bigbuffer);
      if (usb_debug >= 2) {
        if (ret > 0)
          fprintf(stderr, "Descriptor data still left\n");
        else if (ret < 0)
          fprintf(stderr, "Unable to parse descriptors\n");
      }

      free(bigbuffer);
    }

err:
    close(fd);
  }

  closedir(dir);

  *devices = fdev;

  return 0;
}

int usb_os_determine_children(struct usb_bus *bus)
{
  struct usb_device *dev, *devices[256];
  struct usb_ioctl command;
  int ret, i, i1;

  /* Create a list of devices first */
  memset(devices, 0, sizeof(devices));
  for (dev = bus->devices; dev; dev = dev->next)
    if (dev->devnum)
      devices[dev->devnum] = dev;

  /* Now fetch the children for each device */
  for (dev = bus->devices; dev; dev = dev->next) {
    struct usb_hub_portinfo portinfo;
    int fd;

    fd = device_open(dev);
    if (fd < 0)
      continue;

    /* Query the hub driver for the children of this device */
    if (dev->config && dev->config->interface && dev->config->interface->altsetting)
      command.ifno = dev->config->interface->altsetting->bInterfaceNumber;
    else
      command.ifno = 0;
    command.ioctl_code = IOCTL_USB_HUB_PORTINFO;
    command.data = &portinfo;
    ret = ioctl(fd, IOCTL_USB_IOCTL, &command);
    if (ret < 0) {
      /* errno == ENOSYS means the device probably wasn't a hub */
      if (errno != ENOSYS && usb_debug > 1)
        fprintf(stderr, "error obtaining child information: %s\n",
		strerror(errno));

      close(fd);
      continue;
    }

    dev->num_children = 0;
    for (i = 0; i < portinfo.numports; i++)
      if (portinfo.port[i])
        dev->num_children++;

    /* Free any old children first */
    free(dev->children);

    dev->children = malloc(sizeof(struct usb_device *) * dev->num_children);
    if (!dev->children) {
      if (usb_debug > 1)
        fprintf(stderr, "error allocating %zu bytes memory for dev->children\n",
                sizeof(struct usb_device *) * dev->num_children);

      dev->num_children = 0;
      close(fd);
      continue;
    }

    for (i = 0, i1 = 0; i < portinfo.numports; i++) {
      if (!portinfo.port[i])
        continue;

      dev->children[i1++] = devices[portinfo.port[i]];

      devices[portinfo.port[i]] = NULL;
    }

    close(fd);
  }

  /*
   * There should be one device left in the devices list and that should be
   * the root device
   */
  for (i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
    if (devices[i])
      bus->root_dev = devices[i];
  }

  return 0;
}

static int check_usb_vfs(const char *dirname)
{
  DIR *dir;
  struct dirent *entry;
  int found = 0;

  dir = opendir(dirname);
  if (!dir)
    return 0;

  while ((entry = readdir(dir)) != NULL) {
    /* Skip anything starting with a . */
    if (entry->d_name[0] == '.')
      continue;

    /* We assume if we find any files that it must be the right place */
    found = 1;
    break;
  }

  closedir(dir);

  return found;
}

void usb_os_init(void)
{
  /* Find the path to the virtual filesystem */
  if (getenv("USB_DEVFS_PATH")) {
    if (check_usb_vfs(getenv("USB_DEVFS_PATH"))) {
      strncpy(usb_path, getenv("USB_DEVFS_PATH"), sizeof(usb_path) - 1);
      usb_path[sizeof(usb_path) - 1] = 0;
    } else if (usb_debug)
      fprintf(stderr, "usb_os_init: couldn't find USB VFS in USB_DEVFS_PATH\n");
  }

  if (!usb_path[0]) {
    if (check_usb_vfs("/dev/bus/usb")) {
      strncpy(usb_path, "/dev/bus/usb", sizeof(usb_path) - 1);
      usb_path[sizeof(usb_path) - 1] = 0;
    } else if (check_usb_vfs("/proc/bus/usb")) {
      strncpy(usb_path, "/proc/bus/usb", sizeof(usb_path) - 1);
      usb_path[sizeof(usb_path) - 1] = 0;
    } else
      usb_path[0] = 0;	/* No path, no USB support */
  }

  if (usb_debug) {
    if (usb_path[0])
      fprintf(stderr, "usb_os_init: Found USB VFS at %s\n", usb_path);
    else
      fprintf(stderr, "usb_os_init: No USB VFS found, is it mounted?\n");
  }
}

LIBUSB_API int usb_resetep(usb_dev_handle *dev, unsigned int ep)
{
  int ret;

  ret = ioctl(dev->fd, IOCTL_USB_RESETEP, &ep);
  if (ret)
    USB_ERROR_STR(win_error(errno), "could not reset ep %d: %s", ep,
    	strerror(errno));

  return 0;
}

LIBUSB_API int usb_clear_halt(usb_dev_handle *dev, unsigned int ep)
{
  int ret;

  ret = ioctl(dev->fd, IOCTL_USB_CLEAR_HALT, &ep);
  if (ret)
    USB_ERROR_STR(win_error(errno), "could not clear/halt ep %d: %s", ep,
    	strerror(errno));

  return 0;
}

LIBUSB_API int usb_reset(usb_dev_handle *dev)
{
  int ret;

  ret = ioctl(dev->fd, IOCTL_USB_RESET, NULL);
  if (ret)
     USB_ERROR_STR(win_error(errno), "could not reset: %s", strerror(errno));

  return 0;
}

LIBUSB_API int usb_get_driver_np(usb_dev_handle *dev, int interface, char *name,
	unsigned int namelen)
{
  struct usb_getdriver getdrv;
  int ret;

  getdrv.interface = interface;
  ret = ioctl(dev->fd, IOCTL_USB_GETDRIVER, &getdrv);
  if (ret)
    USB_ERROR_STR(-errno, "could not get bound driver: %s", strerror(errno));

  strncpy(name, getdrv.driver, namelen - 1);
  name[namelen - 1] = 0;

  return 0;
}

LIBUSB_API int usb_detach_kernel_driver_np(usb_dev_handle *dev, int interface)
{
  struct usb_ioctl command;
  int ret;

  command.ifno = interface;
  command.ioctl_code = IOCTL_USB_DISCONNECT;
  command.data = NULL;

  ret = ioctl(dev->fd, IOCTL_USB_IOCTL, &command);
  if (ret)
    USB_ERROR_STR(-errno, "could not detach kernel driver from interface %d: %s",
        interface, strerror(errno));

  return 0;
}


// -------------------------------------------------------------------------------
// this async functions added by some person who'd like to remain anonymous
// It was necessary to make Aerodrums application run under wine.

typedef struct {
	usb_dev_handle *dev;
	char *bytes;
	int size;
	struct usb_urb urb;
	int reaped;
} usb_context_t;

static int _usb_setup_async(usb_dev_handle *dev, void **context,
                            int urbtype,
                            unsigned char ep, int pktsize)
{
	usb_context_t **c = (usb_context_t **)context;

	/* Any error checks required here? */

	*c = malloc(sizeof(usb_context_t));

	if (!*c) {
		USB_ERROR_STR(-errno, "memory allocation error: %s\n", strerror(errno));
		return -ENOMEM;
	}

	memset(*c, 0, sizeof(usb_context_t));

	(*c)->dev = dev;
	(*c)->urb.type = urbtype;
	(*c)->urb.endpoint = ep;
	(*c)->urb.flags = 0;
	(*c)->urb.signr = 0;
	(*c)->urb.actual_length = 0;
	(*c)->urb.number_of_packets = 0;	/* don't do isochronous yet */
	(*c)->urb.usercontext = NULL;

	return 0;
}

LIBUSB_API int usb_bulk_setup_async(usb_dev_handle *dev, void **context,
	unsigned char ep)
{
	return _usb_setup_async(dev, context, USB_URB_TYPE_BULK, ep, 0);
}

/*
 * libusb-win32 handles interrupt endpoints exactly like bulk ones. So can
 * we: the kernel turns a bulk URB on an interrupt endpoint into an
 * interrupt transfer, while it refuses an interrupt URB on a bulk one.
 */
LIBUSB_API int usb_interrupt_setup_async(usb_dev_handle *dev, void **context,
	unsigned char ep)
{
	return _usb_setup_async(dev, context, USB_URB_TYPE_BULK, ep, 0);
}

/* Reading and writing are the same except for the endpoint */
LIBUSB_API int usb_submit_async(void *context, char *bytes, int size)
{
	int ret;

	usb_context_t *c = (usb_context_t *)context;

	c->urb.buffer = bytes;
	c->urb.buffer_length = size;
	c->urb.usercontext = c;
	c->reaped = 0;

	ret = ioctl(c->dev->fd, IOCTL_USB_SUBMITURB, &c->urb);
	if (ret < 0) {
		USB_ERROR_STR(-errno, "error submitting URB: %s", strerror(errno));
		return ret;
	}

	return 0;
}

static int _usb_reap_async(void *context, int timeout, int cancel)
{
	int rc;
	usb_context_t *c = (usb_context_t *)context;
	struct usb_urb *urb;

	if (c->reaped)
		return c->urb.actual_length;

again:
	rc = ioctl(c->dev->fd, IOCTL_USB_REAPURB, &urb);
	if (rc < 0) {
		fprintf(stderr, "error reaping URB: %s", strerror(errno));
		return rc;
	}

	usb_context_t *b = (usb_context_t *)urb->usercontext;
	if (b != c) {
		b->reaped = 1;
		goto again;
	}

#if 0
	/* Not ready for usb_cancel_async */
	if (cancel) {
		usb_cancel_async(context);
	}
#else
	(void)cancel;
#endif
	return urb->actual_length;
}

LIBUSB_API int usb_reap_async(void *context, int timeout)
{
	return _usb_reap_async(context, timeout, 1);
}

LIBUSB_API int usb_reap_async_nocancel(void *context, int timeout)
{
	return _usb_reap_async(context, timeout, 0);
}


LIBUSB_API int usb_cancel_async(void *context)
{
	printf("%s()\n", __func__);
	USB_ERROR_STR(-errno, "usb_cancel_async is not yet implemented: %s\n", strerror(errno));
#if 0
    /* NOTE that this function will cancel all pending URBs */
    /* on the same endpoint as this particular context, or even */
    /* all pending URBs for this particular device. */

    usb_context_t *c = (usb_context_t *)context;

    if (!c)
    {
        USBERR0("invalid context\n");
        return -EINVAL;
    }

    if (c->dev->impl_info == INVALID_HANDLE_VALUE)
    {
        USBERR0("device not open\n");
        return -EINVAL;
    }

    _usb_cancel_io(c);

    return 0;
#endif
	int rc;
	usb_context_t *c = (usb_context_t *)context;

	rc = ioctl(c->dev->fd, IOCTL_USB_DISCARDURB, &c->urb);
	if (rc < 0)
		fprintf(stderr, "error discarding URB: %s", strerror(errno));

	/*
	* When the URB is unlinked, it gets moved to the completed list and
	* then we need to reap it or else the next time we call this function,
	* we'll get the previous completion and exit early
	*/
	ioctl(c->dev->fd, IOCTL_USB_REAPURB, &context);

	return 0;
}

LIBUSB_API int usb_free_async(void **context)
{
	usb_context_t **c = (usb_context_t **)context;

	if (!*c) {
		USB_ERROR_STR(-errno, "invalid context: %s\n", strerror(errno));
		return -EINVAL;
	}

	free(*c);
	*c = NULL;

	return 0;
}
