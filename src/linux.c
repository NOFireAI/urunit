// Copyright (c) 2023-2026, Nubificus LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <fcntl.h>
#include <stdint.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/route.h>
#include <netinet/in.h>

#include "common.h"

#define ETH0_IF "eth0"

// read_block_dev_serial: Read the serial ID of a block device from the respective sysfs
// entry.
//
// Arguments:
// 1. device_name:	The device name
// 2. serial:		The buffer to hold the serial ID that was found
// 2. size:		The max size of the buffer
//
// Return value:
// If the device exists, then 0 is returned.
// If the device does not exist, 1 is returned.
// In all other cases or errors -1 is returned.
int read_block_dev_serial(const char *device_name, char *serial, const size_t size) {
	char path[PATH_MAX];
	FILE *fp;
	int ret = 0;

	ret = snprintf(path, sizeof(path), "/sys/block/%s/serial", device_name);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		fprintf(stderr, "Could not create sysfs path for %s\n", device_name);
		return -1;
	}

	fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT) {
			return 1;
		}
		perror("fopen");
		return -1;
	}

	if (fgets(serial, size, fp) == NULL) {
		fclose(fp);
		return -1;
	}

	// Remove trailing whitespace
	serial[strcspn(serial, "\n\r \t")] = '\0';
	fclose(fp);
	return 0;
}

// vblock_device_name: Writes the path of the nth (zero-based) virtio block
// device (vd*) to device_path, which must hold at least VBLOCK_NAME_SZ bytes.
// n == 0 maps to /dev/vda, n == 1 to /dev/vdb, and so on.
//
// Arguments:
// 1. n:		Zero-based index of the virtio block device.
// 2. device_path:	The buffer that will store the path to the block device.
void vblock_device_name(const uint32_t n, char *device_path) {
	// TODO: Add support for more than 26 devices.
	char device_name[] = "/dev/vda";

	device_name[7] = 'a' + (n % 26);
	// It is safe here to do a quick memcpy, since device_name has
	// a specific size and only its last character (of the string,
	// not NULL termination byte) can change
	memcpy(device_path, device_name, sizeof(device_name));
}

// is_order_block_id: Checks whether a block ID from the configuration names
// the device by its order rather than by its serial ID. urunc prefixes the
// IDs with "FC" for Firecracker, which does not expose serial IDs.
//
// Arguments:
// 1. id:	The block ID from the configuration.
//
// Return value:
// 1 if the device is found by its order, otherwise 0.
int is_order_block_id(const char *id) {
	return strlen(id) > 2 && id[0] == 'F' && id[1] == 'C';
}

// find_vblock_device_by_order: Returns the nth (zero-based) virtio block device
// (vd*) if it exists. The order is based on the conventional naming of virtio
// block devices in Linux where usually the first attached is vda, second vdb,
// etc. In this function, n == 0 maps to /dev/vda, n == 1 to /dev/vdb, and so on.
//
// Arguments:
// 1. n:		Zero-based index of the virtio block device to return.
// 2. device_path:	The buffer that will store the path to the block device.
//
// Return value:
// On success 0 is returned and device_path parameter will hold the path
// the the device with the specific ID.
// Otherwise -1 is returned and device_path stays intact.
int find_vblock_device_by_order(const uint32_t n, char *device_path) {
	char device_name[VBLOCK_NAME_SZ];

	vblock_device_name(n, device_name);
	if (access(device_name, F_OK))
		return -1;

	memcpy(device_path, device_name, sizeof(device_name));
	return 0;
}

// find_vblock_device_by_serial: Search all virtio block devices (vd[a-z]) to find the
// one with a specific serial ID.
//
// Arguments:
// 1. target_serial:	The serial ID to search for in the devices
// 2. device_path:	The buffer that will store the path to the block device
//
// Return value:
// On success 0 is returned and device_path parameter will hold the path
// the the device with the specific ID.
// Otherwise -1 is returned.
int find_vblock_device_by_serial(const char *target_serial, char *device_path) {
	char suffix = 0;
	char serial[SERIAL_MAX_SZ];
	char device_name[] = "vda";

	for (suffix = 'a'; suffix <= 'z'; suffix++) {
		int ret = 0;

		device_name[2] = suffix;
		ret = read_block_dev_serial(device_name, serial, sizeof(serial));
		if (ret < 0) {
			fprintf(stderr, "Error getting serial id of %s\n", device_name);
			continue;
		} else if (ret > 0) {
			// The device does not exist. Move to the next one.
			continue;
		}
		if (strcmp(serial, target_serial) == 0) {
			int ret = snprintf(device_path, PATH_MAX, "/dev/%s", device_name);
			if (ret < 0 || (size_t)ret >= PATH_MAX) {
				fprintf(stderr, "Could not copy the found device: %s", device_name);
				return -1;
			}
			return 0;
		}
	}

	return -1;
}

// already_mounted: Tells whether a failed mount of procfs or sysfs does not
// matter, because the filesystem is there already. A process that may not
// mount (a user without CAP_SYS_ADMIN, as when the tests run the supervisor)
// still finds what it needs under <probe>.
static int already_mounted(const char *probe) {
	return errno == EPERM && access(probe, F_OK) == 0;
}

// tmp_is_mounted: Tells whether /tmp is a mount point, i.e. lives on another
// device than its parent.
static int tmp_is_mounted(void) {
	struct stat tmp, root;

	if (stat("/tmp", &tmp) < 0 || stat("/", &root) < 0) {
		return 0;
	}
	return tmp.st_dev != root.st_dev;
}

// mount_special_fs: Mounts the special filesystems procfs and sysfs in /proc and
// /sys respectively.
//
// In a generic container boot the early init has already mounted both in the
// new root before it hands over to the supervisor. Mounting the same procfs or
// sysfs again on top of itself fails with EBUSY, which therefore means that the
// filesystem is already there. So does EPERM, when the filesystem is usable.
//
// It also mounts a tmpfs on /tmp, unless /tmp is a mount point already. The
// early init of a container boot mounts one when it stages urunit.conf, and
// mounting another on top would hide the staged files. Any other boot has no
// early init, so this is where applications get a writable /tmp.
//
// Arguments:
// No arguments.
//
// Return value:
// It returns 0 in success. Otherwise it returns 1.
int mount_special_fs(void) {
	int ret = 0;

	ret = ensure_dir("/proc");
	if (ret < 0) {
		return 1;
	}
	ret = mount("proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL);
	if (ret < 0 && errno != EBUSY && !already_mounted("/proc/self")) {
		perror("mount /proc");
		return 1;
	}

	ret = ensure_dir("/sys");
	if (ret < 0) {
		return 1;
	}
	ret = mount("sysfs", "/sys", "sysfs", 0, NULL);
	if (ret < 0 && errno != EBUSY && !already_mounted("/sys/kernel")) {
		perror("mount /sys");
		return 1;
	}

	// A writable /tmp is expected by most applications. Unlike /proc and
	// /sys, urunit does not itself depend on it, so a failure is not fatal.
	if (!tmp_is_mounted() && ensure_dir("/tmp") == 0) {
		ret = mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID|MS_NODEV, "mode=1777");
		if (ret < 0) {
			perror("mount /tmp");
		}
	}

	return 0;
}

// mount_block_vols:	Mounts all block devices using their info from the
// block_config parameter.
//
// Arguments:
// 1. vols:	An array of struct block_config with information to mount
//		block volumes
//
// Return value:
// On success 0 is returned.
// Otherwise 1 is returned.
int mount_block_vols(struct block_config **vols) {
	struct block_config **iter_bc = NULL;
	char first_new_dir[PATH_MAX] = { 0 };
	uint32_t blk_count = 0;

	if (vols == NULL) {
		DEBUG_PRINT("No block volumes to mount, nothing to do\n");
		return 0;
	}

	for (iter_bc = vols; *iter_bc != NULL; iter_bc++) {
		struct block_config *tmp_bc = *iter_bc;
		char block_dev[PATH_MAX] = { 0 };
		int ret = 0;

		blk_count++;
		first_new_dir[0] = '\0';
		DEBUG_PRINTF("Searching block device with serial ID %s\n", tmp_bc->id);
		if (is_order_block_id(tmp_bc->id)) {
			ret = find_vblock_device_by_order(blk_count, block_dev);
		} else {
			ret = find_vblock_device_by_serial(tmp_bc->id, block_dev);
		}
		if (ret) {
			fprintf(stderr, "Could not find any virtio block device with serial ID %s\n", tmp_bc->id);
			continue;
		}
		DEBUG_PRINTF("Found device %s\n", block_dev);
		DEBUG_PRINTF("Setup the mountpoint %s\n", tmp_bc->mountpoint);
		ret = mkdir_all(tmp_bc->mountpoint, 0755, first_new_dir);
		if (ret != 0 ) {
			fprintf(stderr, "Failed to create %s\n",tmp_bc->mountpoint);
			continue;
		}
		DEBUG_PRINT("Mount device as ext4\n");
		// TODO: Support more filesystem types
		ret = mount(block_dev, tmp_bc->mountpoint, "ext4", 0, "");
		if (ret != 0) {
			perror("mount");
			// Remove previously created directories.
			// NOTE: In case of an error we just print a warning
			// We might want to revisit this in the future.
			if (first_new_dir[0] != '\0') {
				ret = rm_empty_dirs(tmp_bc->mountpoint, first_new_dir);
				if (ret < 0) {
					fprintf(stderr, "WARNING: Could not remove %s and its subdirs\n", tmp_bc->mountpoint);
				}
			}
		}
	}

	return 0;
}

// set_default_route: Sets the default network route to eth0.
//
// Arguments:
// No arguments.
//
// Return value:
// On success 0 is returned. Otherwise a non-zero value is returned.
int set_default_route(void) {
	int sockfd;
	struct rtentry rt;
	struct sockaddr_in addr;
	int ret = 0;

	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd == -1) {
		perror("socket creation failed");
		return 1;
	}

	memset(&rt, 0, sizeof(rt));

	// Set default route for any IP address
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	memcpy(&rt.rt_dst, &addr, sizeof(addr));

	memcpy(&rt.rt_genmask, &addr, sizeof(addr));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = 0;
	memcpy(&rt.rt_gateway, &addr, sizeof(addr));

	DEBUG_PRINT("Setting default route to eth0\n");
	rt.rt_flags = RTF_UP;
	// TODO: We might want to discover or somehow
	// get the interface as a parameter.
	rt.rt_dev = ETH0_IF;

	ret = ioctl(sockfd, SIOCADDRT, &rt);
	if(ret < 0) {
		perror("ioctl SIOCADDRT");
	}

	close(sockfd);
	return ret;
}

// unmount_external: Unmounts all the external filesystem mounts found in
// /proc/self/mountinfo. External means all the known block, network and cloud storage
// based filesystems.
//
// Arguments:
//
// Return value:
void unmount_external(void) {
	FILE *mount_info_f = NULL;
	char line[1024] = { 0 };

	mount_info_f = fopen("/proc/self/mountinfo", "r");
	if (!mount_info_f) {
		perror("Error opening /proc/self/mountinfo");
		return;
	}

	while (fgets(line, sizeof(line), mount_info_f)) {
		char *tmp = NULL;
		char *mount_point = NULL;
		char *mount_type = NULL;

		mount_point = skip_n_words(line, 4);
		if (mount_point == line) {
			fprintf(stderr, "Malformed line in mountinfo. Could not reach mountpoint: %s\n", line);
			continue;
		}
		tmp = strchr(mount_point, ' ');
		if (!tmp) {
			fprintf(stderr, "Malformed line in mountinfo. Could not get mountpoint: %s\n", line);
			continue;
		}
		*tmp = '\0';
		tmp++;
		DEBUG_PRINTF("Found mountpoint %s\n", mount_point);
		// Skip rootfs because we can not unmount it easily.
		// Also, the rootfs will be based on the container's image
		// and hence even if something goes wrong, a new instance of it
		// will get created for another container. Therefore, it will not
		// get reused.
		if (strcmp(mount_point, "/") == 0)
			continue;
		mount_type = strstr(tmp, " - ");
		if (!mount_type) {
			fprintf(stderr, "Malformed line in mountinfo. Could not reach mount type: %s%s\n", line, tmp);
			continue;
		}
		mount_type += 3;
		tmp = strchr(mount_type, ' ');
		if (!tmp) {
			fprintf(stderr, "Malformed line in mountinfo. Could not get mount type: %s%s\n", line, mount_type - 3);
			continue;
		}
		*tmp = '\0';
		DEBUG_PRINTF("Found mount type %s\n", mount_type);
		if (is_block_fs(mount_type) ||
		    is_network_fs(mount_type) ||
		    is_cloud_storage_fs(mount_type)) {
			int ret = 0;
			DEBUG_PRINTF("Trying to unmount %s\n", mount_point);
			ret = umount2(mount_point, MNT_FORCE);
			if (ret) {
				perror("umount");
			} else {
				DEBUG_PRINTF("Successful unmount of %s\n", mount_point);
			}
		}
	}

	fclose(mount_info_f);
}

int set_subreaper(void) {
	return prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
}

// The ioctl to get the device number of the terminal behind an alias
// device such as /dev/console. Define it for libcs that do not expose it.
#ifndef TIOCGDEV
#define TIOCGDEV 0x80045432
#endif

// setup_controlling_tty: Makes the calling process a session leader with the
// system console as its controlling terminal.
//
// The kernel hands init its stdio on /dev/console, which can never become a
// controlling terminal. As a result the application gets no controlling
// terminal at all: job control does not work and interactive shells print
// "can't access tty; job control turned off". To fix this, we find the real
// terminal behind /dev/console (TIOCGDEV on stdin), reopen that device,
// start a new session and adopt the device as the controlling terminal
// (TIOCSCTTY), pointing stdio to the new file descriptor.
//
// Return value:
// On success 0 is returned: the process is a session leader, the console is
// its controlling terminal and stdio point to it.
// Otherwise -1 is returned and nothing has changed for the calling process,
// hence the caller can fall back to plain process group handling.
int setup_controlling_tty(void) {
	// The console devices urunc guests boot with (virtio-console,
	// x86 UART, arm64 PL011) and the VGA console as a last resort.
	static const char *candidates[] = {
		"/dev/hvc0", "/dev/ttyS0", "/dev/ttyAMA0", "/dev/tty1", NULL
	};
	unsigned int con_dev = 0;
	int fd = -1;
	int i = 0;

	if (!isatty(STDIN_FILENO)) {
		return -1;
	}

	// Find the device number of the real terminal behind /dev/console.
	if (ioctl(STDIN_FILENO, TIOCGDEV, &con_dev) != 0) {
		return -1;
	}

	for (i = 0; candidates[i]; i++) {
		struct stat st = { 0 };

		fd = open(candidates[i], O_RDWR | O_NOCTTY);
		if (fd < 0) {
			continue;
		}
		// The comparison is exact, since for the small major/minor
		// numbers of console devices the kernel's encoding in TIOCGDEV
		// matches the libc's dev_t encoding.
		if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
		    st.st_rdev == (dev_t)con_dev) {
			DEBUG_PRINTF("Console device is %s\n", candidates[i]);
			break;
		}
		close(fd);
		fd = -1;
	}
	if (fd < 0) {
		DEBUG_PRINT("Could not match the console device\n");
		return -1;
	}

	if (setsid() < 0) {
		perror("setsid");
		close(fd);
		return -1;
	}
	// From this point on there is no way back to the old session, so we
	// keep going even if any of the following calls fail. In the worst
	// case the app keeps running on the console without job control,
	// which was the previous behavior anyway.
	if (ioctl(fd, TIOCSCTTY, 0) != 0) {
		perror("TIOCSCTTY");
	}
	if (dup2(fd, STDIN_FILENO) < 0 ||
	    dup2(fd, STDOUT_FILENO) < 0 ||
	    dup2(fd, STDERR_FILENO) < 0) {
		perror("dup2 console");
	}
	if (fd > STDERR_FILENO) {
		close(fd);
	}

	return 0;
}

// read_raw_device: Reads the whole contents of a block device. The caller is
// responsible to free the returned buffer.
char *read_raw_device(int fd, size_t *size) {
	uint64_t dev_size = 0;
	char *buffer = NULL;
	size_t off = 0;
	int ret = 0;
	// BLKGETSIZE64 does not fit in an int. glibc's ioctl takes an unsigned
	// long request, but musl's takes an int, so passing the constant
	// directly overflows under -Werror there. Hold it in an unsigned long
	// so the (musl) narrowing is a runtime conversion, not a constant one.
	unsigned long request = BLKGETSIZE64;

	ret = ioctl(fd, request, &dev_size);
	if (ret < 0) {
		perror("BLKGETSIZE64");
		return NULL;
	}
	if (dev_size == 0 || dev_size > (1024 * 1024)) {
		fprintf(stderr, "Unexpected configuration device size %llu\n",
			(unsigned long long)dev_size);
		return NULL;
	}
	buffer = malloc(dev_size + 1);
	if (!buffer) {
		fprintf(stderr, "Failed to allocate memory for the configuration device\n");
		return NULL;
	}
	while (off < dev_size) {
		ssize_t n = 0;

		n = pread(fd, buffer + off, dev_size - off, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read configuration device");
			free(buffer);
			return NULL;
		}
		if (n == 0)
			break;
		off += (size_t)n;
	}
	buffer[off] = '\0';
	*size = off;

	return buffer;
}

int reboot_command(void) {
	return getenv("URUNIT_POWEROFF") != NULL ? LINUX_REBOOT_CMD_POWER_OFF
						 : LINUX_REBOOT_CMD_RESTART;
}

// setup_console: On Linux the kernel opens the console for init.
int setup_console(void) {
	return 0;
}

// remount_root_rw: On Linux the root filesystem is already writable by the time
// urunit runs (urunc/the kernel set it up), so there is nothing to do here.
int remount_root_rw(void) {
	return 0;
}

// get_boot_var: On Linux, name=value boot parameters reach init as
// environment variables, so there is nothing else to look at.
char *get_boot_var(const char *name) {
	(void)name; // just to suppress the unused warning
	return NULL;
}

// configure_network: On Linux the network is configured by the kernel
// through the ip= boot parameter.
int configure_network(struct net_config *net) {
	(void)net;
	return 0;
}

void request_reboot(void) {
	// urunit shuts the guest down by asking the kernel to reset, which the VMM
	// turns into an exit (qemu with -no-reboot, firecracker on any reset). Cloud
	// Hypervisor instead restarts the VM on a reset, so when urunc sets
	// URUNIT_POWEROFF the guest powers off (ACPI) instead, which Cloud Hypervisor
	// turns into a clean exit. Power off needs ACPI in the guest kernel.
	int cmd = reboot_command();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		cmd, NULL);
}

// request_poweroff: Powers the VM off. Cloud Hypervisor restarts the guest
// on a reset, but exits on a power off.
void request_poweroff(void) {
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
		LINUX_REBOOT_CMD_POWER_OFF, NULL);
}

// The generic netlink family and multicast group on which the kernel reports
// ACPI events (drivers/acpi/event.c), and the event it sends. A press of the
// ACPI power button is an event of the "button/power" device class.
#define ACPI_EVENT_FAMILY	"acpi_event"
#define ACPI_EVENT_GROUP	"acpi_mc_group"
#define ACPI_GENL_ATTR_EVENT	1
#define ACPI_POWER_BUTTON_CLASS	"button/power"

struct acpi_genl_event {
	char device_class[20];
	char bus_id[15];
	uint32_t type;
	uint32_t data;
};

static int acpi_event_fd = -1;
static uint16_t acpi_event_family = 0;

// nla_find: Returns the attribute of the given type among the netlink
// attributes in buf, len bytes long, or NULL.
static const struct nlattr *nla_find(const void *buf, size_t len, uint16_t type) {
	const struct nlattr *a = buf;

	while (len >= NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && a->nla_len <= len) {
		if ((a->nla_type & NLA_TYPE_MASK) == type) {
			return a;
		}
		if ((size_t)NLA_ALIGN(a->nla_len) >= len) {
			break;
		}
		len -= NLA_ALIGN(a->nla_len);
		a = (const struct nlattr *)((const char *)a + NLA_ALIGN(a->nla_len));
	}
	return NULL;
}

// nlmsg_fits: Returns 1 if the netlink message n is whole in the len bytes
// at n.
static int nlmsg_fits(const struct nlmsghdr *n, size_t len) {
	return len >= sizeof(*n) && n->nlmsg_len >= sizeof(*n) && n->nlmsg_len <= len;
}

// resolve_acpi_events: Asks the generic netlink controller on fd for the id
// of the ACPI event family and of its multicast group.
static int resolve_acpi_events(int fd, uint16_t *family, uint32_t *group) {
	char buf[4096] __attribute__((aligned(NLMSG_ALIGNTO)));
	struct nlmsghdr *n = (struct nlmsghdr *)buf;
	struct genlmsghdr *g;
	struct nlattr *a;
	const struct nlattr *attr, *grp;
	size_t len, left;
	ssize_t r;

	memset(buf, 0, sizeof(buf));
	n->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN + NLA_HDRLEN + sizeof(ACPI_EVENT_FAMILY));
	n->nlmsg_type = GENL_ID_CTRL;
	n->nlmsg_flags = NLM_F_REQUEST;
	g = NLMSG_DATA(n);
	g->cmd = CTRL_CMD_GETFAMILY;
	g->version = 1;
	a = (struct nlattr *)((char *)g + GENL_HDRLEN);
	a->nla_type = CTRL_ATTR_FAMILY_NAME;
	a->nla_len = NLA_HDRLEN + sizeof(ACPI_EVENT_FAMILY);
	memcpy((char *)a + NLA_HDRLEN, ACPI_EVENT_FAMILY, sizeof(ACPI_EVENT_FAMILY));
	if (send(fd, buf, n->nlmsg_len, 0) < 0) {
		return -1;
	}

	r = recv(fd, buf, sizeof(buf), 0);
	if (r < 0 || !nlmsg_fits(n, (size_t)r) || n->nlmsg_type == NLMSG_ERROR ||
	    n->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN)) {
		return -1;
	}
	len = n->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
	attr = (const struct nlattr *)((char *)NLMSG_DATA(n) + GENL_HDRLEN);

	a = (struct nlattr *)nla_find(attr, len, CTRL_ATTR_FAMILY_ID);
	if (a == NULL || a->nla_len < NLA_HDRLEN + sizeof(uint16_t)) {
		return -1;
	}
	memcpy(family, (char *)a + NLA_HDRLEN, sizeof(uint16_t));

	a = (struct nlattr *)nla_find(attr, len, CTRL_ATTR_MCAST_GROUPS);
	if (a == NULL) {
		return -1;
	}
	// Each group is a nested set of attributes: its name and its id.
	grp = (const struct nlattr *)((char *)a + NLA_HDRLEN);
	left = a->nla_len - NLA_HDRLEN;
	while (left >= NLA_HDRLEN && grp->nla_len >= NLA_HDRLEN && grp->nla_len <= left) {
		const void *inner = (const char *)grp + NLA_HDRLEN;
		size_t inner_len = grp->nla_len - NLA_HDRLEN;
		const struct nlattr *name = nla_find(inner, inner_len, CTRL_ATTR_MCAST_GRP_NAME);
		const struct nlattr *id = nla_find(inner, inner_len, CTRL_ATTR_MCAST_GRP_ID);

		if (name != NULL && id != NULL && id->nla_len >= NLA_HDRLEN + sizeof(uint32_t) &&
		    strncmp((const char *)name + NLA_HDRLEN, ACPI_EVENT_GROUP,
			    name->nla_len - NLA_HDRLEN) == 0) {
			memcpy(group, (const char *)id + NLA_HDRLEN, sizeof(uint32_t));
			return 0;
		}
		if ((size_t)NLA_ALIGN(grp->nla_len) >= left) {
			break;
		}
		left -= NLA_ALIGN(grp->nla_len);
		grp = (const struct nlattr *)((const char *)grp + NLA_ALIGN(grp->nla_len));
	}
	return -1;
}

// open_power_buttons: Subscribes to the ACPI events of the kernel, among them
// a press of the ACPI power button (x86), and asks the kernel for a SIGIO to
// this process when one arrives. The events need no device node in /dev.
//
// Arguments:
// No arguments.
//
// Return value:
// 1 if it watches the power button, 0 if the kernel reports no ACPI events.
int open_power_buttons(void) {
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
	uint32_t group = 0;
	int fd;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	if (fd < 0) {
		return 0;
	}
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
	    resolve_acpi_events(fd, &acpi_event_family, &group) < 0 ||
	    setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof(group)) < 0 ||
	    fcntl(fd, F_SETOWN, getpid()) < 0 ||
	    fcntl(fd, F_SETFL, O_NONBLOCK | O_ASYNC) < 0) {
		close(fd);
		return 0;
	}
	DEBUG_PRINTF("Watching ACPI events, family %u group %u\n", acpi_event_family, group);
	acpi_event_fd = fd;
	return 1;
}

// power_button_pressed: Reads the pending ACPI events. It is
// async-signal-safe, for the SIGIO handler.
//
// Arguments:
// No arguments.
//
// Return value:
// 1 if the power button was pressed, 0 otherwise.
int power_button_pressed(void) {
	char buf[4096] __attribute__((aligned(NLMSG_ALIGNTO)));
	int pressed = 0;
	ssize_t r;

	if (acpi_event_fd < 0) {
		return 0;
	}
	while ((r = recv(acpi_event_fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		struct nlmsghdr *n = (struct nlmsghdr *)buf;
		size_t left = (size_t)r;

		while (nlmsg_fits(n, left)) {
			const struct nlattr *a = NULL;
			const struct acpi_genl_event *ev;

			if (n->nlmsg_type == acpi_event_family &&
			    n->nlmsg_len >= NLMSG_LENGTH(GENL_HDRLEN)) {
				a = nla_find((char *)NLMSG_DATA(n) + GENL_HDRLEN,
					     n->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN),
					     ACPI_GENL_ATTR_EVENT);
			}
			if (a != NULL && a->nla_len >= NLA_HDRLEN + sizeof(ev->device_class)) {
				ev = (const struct acpi_genl_event *)((const char *)a + NLA_HDRLEN);
				if (strncmp(ev->device_class, ACPI_POWER_BUTTON_CLASS,
					    sizeof(ev->device_class)) == 0) {
					pressed = 1;
				}
			}
			if (NLMSG_ALIGN(n->nlmsg_len) >= left) {
				break;
			}
			left -= NLMSG_ALIGN(n->nlmsg_len);
			n = (struct nlmsghdr *)((char *)n + NLMSG_ALIGN(n->nlmsg_len));
		}
	}
	return pressed;
}
