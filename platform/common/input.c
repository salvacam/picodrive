#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "input.h"
#include "../linux/in_evdev.h"
#include "../gp2x/in_gp2x.h"

typedef struct
{
	int drv_id;
	int drv_fd_hnd;
	void *drv_data;
	char *name;
	int *binds;
	int probed:1;
} in_dev_t;

static in_drv_t in_drivers[IN_DRVID_COUNT];
static in_dev_t in_devices[IN_MAX_DEVS];
static int in_dev_count = 0;
static int in_have_async_devs = 0;

#define DRV(id) in_drivers[(unsigned)(id) < IN_DRVID_COUNT ? (id) : 0]


static int in_bind_count(int drv_id)
{
	int count = DRV(drv_id).get_bind_count();
	if (count <= 0)
		printf("input: failed to get bind count for drv %d\n", drv_id);

	return count;
}

static int *in_alloc_binds(int drv_id)
{
	int count, *binds;

	count = in_bind_count(drv_id);
	if (count <= 0)
		return NULL;

	binds = calloc(count * 2, sizeof(binds[0]));
	if (binds == NULL)
		return NULL;

	DRV(drv_id).get_def_binds(binds + count);
	memcpy(binds, binds + count, count * sizeof(binds[0]));

	return binds;
}

static void in_free(in_dev_t *dev)
{
	if (dev->probed)
		DRV(dev->drv_id).free(dev->drv_data);
	dev->probed = 0;
	dev->drv_data = NULL;
	free(dev->name);
	dev->name = NULL;
	free(dev->binds);
	dev->binds = NULL;
}

/* to be called by drivers
 * async devices must set drv_fd_hnd to -1 */
void in_register(const char *nname, int drv_id, int drv_fd_hnd, void *drv_data)
{
	int i, ret, dupe_count = 0, *binds;
	char name[256], *name_end, *tmp;

	strncpy(name, nname, sizeof(name));
	name[sizeof(name)-12] = 0;
	name_end = name + strlen(name);

	for (i = 0; i < in_dev_count; i++)
	{
		if (in_devices[i].name == NULL)
			continue;
		if (strcmp(in_devices[i].name, name) == 0)
		{
			if (in_devices[i].probed) {
				dupe_count++;
				sprintf(name_end, " [%d]", dupe_count);
				continue;
			}
			goto update;
		}
	}

	if (i >= IN_MAX_DEVS)
	{
		/* try to find unused device */
		for (i = 0; i < IN_MAX_DEVS; i++)
			if (!in_devices[i].probed) break;
		if (i >= IN_MAX_DEVS) {
			printf("input: too many devices, can't add %s\n", name);
			return;
		}
		in_free(&in_devices[i]);
	}

	tmp = strdup(name);
	if (tmp == NULL)
		return;

	binds = in_alloc_binds(drv_id);
	if (binds == NULL) {
		free(tmp);
		return;
	}

	in_devices[i].name = tmp;
	in_devices[i].binds = binds;
	if (i + 1 > in_dev_count)
		in_dev_count = i + 1;

	printf("input: new device #%d \"%s\"\n", i, name);
update:
	in_devices[i].probed = 1;
	in_devices[i].drv_id = drv_id;
	in_devices[i].drv_fd_hnd = drv_fd_hnd;
	in_devices[i].drv_data = drv_data;

	if (in_devices[i].binds != NULL) {
		ret = DRV(drv_id).clean_binds(drv_data, in_devices[i].binds);
		if (ret == 0) {
			/* no useable binds */
			free(in_devices[i].binds);
			in_devices[i].binds = NULL;
		}
	}
}

void in_probe(void)
{
	int i;

	in_have_async_devs = 0;
	for (i = 0; i < in_dev_count; i++)
		in_devices[i].probed = 0;

	for (i = 1; i < IN_DRVID_COUNT; i++)
		in_drivers[i].probe();

	/* get rid of devs without binds and probes */
	for (i = 0; i < in_dev_count; i++) {
		if (!in_devices[i].probed && in_devices[i].binds == NULL) {
			in_dev_count--;
			if (i < in_dev_count) {
				free(in_devices[i].name);
				memmove(&in_devices[i], &in_devices[i+1],
					(in_dev_count - i) * sizeof(in_devices[0]));
			}

			continue;
		}

		if (in_devices[i].probed && in_devices[i].drv_fd_hnd == -1)
			in_have_async_devs = 1;
	}

	if (in_have_async_devs)
		printf("input: async-only devices detected..\n");
}

/* async update */
int in_update(void)
{
	int i, result = 0;

	for (i = 0; i < in_dev_count; i++) {
		in_dev_t *dev = &in_devices[i];
		if (dev->probed && dev->binds != NULL) {
			switch (dev->drv_id) {
#ifdef IN_EVDEV
			case IN_DRVID_EVDEV:
				result |= in_evdev_update(dev->drv_data, dev->binds);
				break;
#endif
#ifdef IN_GP2X
			case IN_DRVID_GP2X:
				result |= in_gp2x_update(dev->drv_data, dev->binds);
				break;
#endif
			}
		}
	}

	return result;
}

static int menu_key_state = 0;

void in_set_blocking(int is_blocking)
{
	int i, ret;

	/* have_async_devs means we will have to do all reads async anyway.. */
	if (!in_have_async_devs) {
		for (i = 0; i < in_dev_count; i++) {
			if (in_devices[i].probed)
				DRV(in_devices[i].drv_id).set_blocking(in_devices[i].drv_data, is_blocking);
		}
	}

	menu_key_state = 0;

	/* flush events */
	do {
		ret = in_update_keycode(NULL, NULL, 0);
	} while (ret >= 0);
}

/* TODO: move.. */
#include <unistd.h>
#include <sys/time.h>
#include <time.h>

static int in_update_kc_async(int *dev_id_out, int *is_down_out, int timeout_ms)
{
	struct timeval start, now;
	int i, is_down, result;

	gettimeofday(&start, NULL);

	while (1)
	{
		for (i = 0; i < in_dev_count; i++) {
			in_dev_t *d = &in_devices[i];
			if (!d->probed)
				continue;

			result = DRV(d->drv_id).update_keycode(d->drv_data, &is_down);
			if (result == -1)
				continue;

			if (dev_id_out)
				*dev_id_out = i;
			if (is_down_out)
				*is_down_out = is_down;
			return result;
		}

		if (timeout_ms >= 0) {
			gettimeofday(&now, NULL);
			if ((now.tv_sec - start.tv_sec) * 1000 +
					(now.tv_usec - start.tv_usec) / 1000 > timeout_ms)
				break;
		}

		usleep(10000);
	}

	return -1;
}

/* 
 * wait for a press, always return some keycode or -1 on timeout or error
 */
int in_update_keycode(int *dev_id_out, int *is_down_out, int timeout_ms)
{
	int result = 0, dev_id = 0, is_down, result_menu;
	int fds_hnds[IN_MAX_DEVS];
	int i, ret, count = 0;
	in_drv_t *drv;

	if (in_have_async_devs) {
		result = in_update_kc_async(&dev_id, &is_down, timeout_ms);
		if (result == -1)
			return -1;
		drv = &DRV(in_devices[dev_id].drv_id);
		goto finish;
	}

	for (i = 0; i < in_dev_count; i++) {
		if (in_devices[i].probed)
			fds_hnds[count++] = in_devices[i].drv_fd_hnd;
	}

	if (count == 0) {
		/* don't deadlock, fail */
		printf("input: failed to find devices to read\n");
		exit(1);
	}

again:
	/* TODO: move this block to platform/linux */
	{
		struct timeval tv, *timeout = NULL;
		int fdmax = -1;
		fd_set fdset;

		if (timeout_ms >= 0) {
			tv.tv_sec = timeout_ms / 1000;
			tv.tv_usec = (timeout_ms % 1000) * 1000;
			timeout = &tv;
		}

		FD_ZERO(&fdset);
		for (i = 0; i < count; i++) {
			if (fds_hnds[i] > fdmax) fdmax = fds_hnds[i];
			FD_SET(fds_hnds[i], &fdset);
		}

		ret = select(fdmax + 1, &fdset, NULL, NULL, timeout);
		if (ret == -1)
		{
			perror("input: select failed");
			sleep(1);
			return -1;
		}

		if (ret == 0)
			return -1; /* timeout */

		for (i = 0; i < count; i++)
			if (FD_ISSET(fds_hnds[i], &fdset))
				ret = fds_hnds[i];
	}

	for (i = 0; i < in_dev_count; i++) {
		if (in_devices[i].drv_fd_hnd == ret) {
			dev_id = i;
			break;
		}
	}

	drv = &DRV(in_devices[dev_id].drv_id);
	result = drv->update_keycode(in_devices[dev_id].drv_data, &is_down);

	/* update_keycode() might return -1 when some not interesting
	 * event happened, like sync event for evdev.
	 * XXX: timeout restarts.. */
	if (result == -1)
		goto again;

finish:
	/* keep track of menu key state, to allow mixing
	 * in_update_keycode() and in_menu_wait_any() calls */
	result_menu = drv->menu_translate(result);
	if (result_menu != 0) {
		if (is_down)
			menu_key_state |=  result_menu;
		else
			menu_key_state &= ~result_menu;
	}

	if (dev_id_out != NULL)
		*dev_id_out = dev_id;
	if (is_down_out != NULL)
		*is_down_out = is_down;
	return result;
}

/* same as above, only return bitfield of PBTN_*  */
int in_menu_wait_any(int timeout_ms)
{
	int keys_old = menu_key_state;

	while (1)
	{
		int code, is_down = 0, dev_id = 0;

		code = in_update_keycode(&dev_id, &is_down, timeout_ms);
		if (code >= 0)
			code = DRV(in_devices[dev_id].drv_id).menu_translate(code);

		if (timeout_ms >= 0)
			break;
		if (code < 0)
			continue;
		if (keys_old != menu_key_state)
			break;
	}

	return menu_key_state;
}

/* wait for menu input, do autorepeat */
int in_menu_wait(int interesting, int autorep_delay_ms)
{
	static int inp_prev = 0;
	static int repeats = 0;
	int ret, release = 0, wait = 666;

	if (repeats)
		wait = autorep_delay_ms;

	ret = in_menu_wait_any(wait);
	if (ret == inp_prev)
		repeats++;

	while (!(ret & interesting)) {
		ret = in_menu_wait_any(-1);
		release = 1;
	}

	if (release || ret != inp_prev)
		repeats = 0;

	inp_prev = ret;

	/* we don't need diagonals in menus */
	if ((ret & PBTN_UP)   && (ret & PBTN_LEFT))  ret &= ~PBTN_LEFT;
	if ((ret & PBTN_UP)   && (ret & PBTN_RIGHT)) ret &= ~PBTN_RIGHT;
	if ((ret & PBTN_DOWN) && (ret & PBTN_LEFT))  ret &= ~PBTN_LEFT;
	if ((ret & PBTN_DOWN) && (ret & PBTN_RIGHT)) ret &= ~PBTN_RIGHT;

	return ret;
}

const int *in_get_dev_binds(int dev_id)
{
	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return NULL;

	return in_devices[dev_id].binds;
}

const int *in_get_dev_def_binds(int dev_id)
{
	int count;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return NULL;

	count = in_bind_count(in_devices[dev_id].drv_id);
	return in_devices[dev_id].binds + count;
}

int in_get_dev_bind_count(int dev_id)
{
	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return 0;

	return in_bind_count(in_devices[dev_id].drv_id);
}

const char *in_get_dev_name(int dev_id, int must_be_active, int skip_pfix)
{
	const char *name, *tmp;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return NULL;

	if (must_be_active && !in_devices[dev_id].probed)
		return NULL;

	name = in_devices[dev_id].name;
	tmp = strchr(name, ':');
	if (tmp != NULL)
		name = tmp + 1;

	return name;
}

/* never returns NULL */
const char *in_get_key_name(int dev_id, int keycode)
{
	static char xname[16];
	const char *name;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return "Unkn0";

	name = DRV(in_devices[dev_id].drv_id).get_key_name(keycode);
	if (name != NULL)
		return name;

	/* assume scancode */
	if ((keycode >= '0' && keycode <= '9') || (keycode >= 'a' && keycode <= 'z')
			|| (keycode >= 'A' && keycode <= 'Z'))
		sprintf(xname, "%c", keycode);
	else
		sprintf(xname, "\\x%02X", keycode);
	return xname;
}

int in_bind_key(int dev_id, int keycode, int mask, int force_unbind)
{
	int ret, count;
	in_dev_t *dev;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return -1;
	dev = &in_devices[dev_id];

	if (dev->binds == NULL) {
		if (force_unbind)
			return 0;
		dev->binds = in_alloc_binds(dev->drv_id);
		if (dev->binds == NULL)
			return -1;
	}

	count = in_bind_count(dev->drv_id);
	if (keycode < 0 || keycode >= count)
		return -1;
	
	if (force_unbind)
		dev->binds[keycode] &= ~mask;
	else
		dev->binds[keycode] ^=  mask;
	
	ret = DRV(dev->drv_id).clean_binds(dev->drv_data, dev->binds);
	if (ret == 0) {
		free(dev->binds);
		dev->binds = NULL;
	}

	return 0;
}

/* returns device id, or -1 on error */
int in_config_parse_dev(const char *name)
{
	int drv_id = -1, i;

	for (i = 0; i < IN_DRVID_COUNT; i++) {
		int len = strlen(in_drivers[i].prefix);
		if (strncmp(name, in_drivers[i].prefix, len) == 0) {
			drv_id = i;
			break;
		}
	}

	if (drv_id < 0) {
		printf("input: missing driver for %s\n", name);
		return -1;
	}

	for (i = 0; i < in_dev_count; i++)
	{
		if (in_devices[i].name == NULL)
			continue;
		if (strcmp(in_devices[i].name, name) == 0)
			return i;
	}

	if (i >= IN_MAX_DEVS)
	{
		/* try to find unused device */
		for (i = 0; i < IN_MAX_DEVS; i++)
			if (in_devices[i].name == NULL) break;
		if (i >= IN_MAX_DEVS) {
			printf("input: too many devices, can't add %s\n", name);
			return -1;
		}
	}

	memset(&in_devices[i], 0, sizeof(in_devices[i]));

	in_devices[i].name = strdup(name);
	if (in_devices[i].name == NULL)
		return -1;

	if (i + 1 > in_dev_count)
		in_dev_count = i + 1;
	in_devices[i].drv_id = drv_id;

	return i;
}

/*
 * To reduce size of game specific configs, default binds are not saved.
 * So we mark default binds in in_config_start(), override them in in_config_bind_key(),
 * and restore whatever default binds are left in in_config_end().
 */
void in_config_start(void)
{
	int i;

	/* mark all default binds, so they get overwritten by func below */
	for (i = 0; i < IN_MAX_DEVS; i++) {
		int n, count, *binds, *def_binds;

		binds = in_devices[i].binds;
		if (binds == NULL)
			continue;

		count = in_bind_count(in_devices[i].drv_id);
		def_binds = binds + count;

		for (n = 0; n < count; n++)
			if (binds[n] == def_binds[n])
				binds[n] = -1;
	}
}

int in_config_bind_key(int dev_id, const char *key, int binds)
{
	int count, kc;
	in_dev_t *dev;

	if (dev_id < 0 || dev_id >= IN_MAX_DEVS)
		return -1;
	dev = &in_devices[dev_id];

	count = in_bind_count(dev->drv_id);

	/* maybe a raw code? */
	if (key[0] == '\\' && key[1] == 'x') {
		char *p = NULL;
		kc = (int)strtoul(key + 2, &p, 16);
		if (p == NULL || *p != 0)
			kc = -1;
	}
	else {
		/* device specific key name */
		if (dev->binds == NULL) {
			dev->binds = in_alloc_binds(dev->drv_id);
			if (dev->binds == NULL)
				return -1;
			in_config_start();
		}

		kc = DRV(dev->drv_id).get_key_code(key);
		if (kc < 0 && strlen(key) == 1) {
			/* assume scancode */
			kc = key[0];
		}
	}

	if (kc < 0 || kc >= count) {
		printf("input: bad key: %s\n", key);
		return -1;
	}

	if (dev->binds[kc] == -1)
		dev->binds[kc] = 0;
	dev->binds[kc] |= binds;

	return 0;
}

void in_config_end(void)
{
	int i;

	for (i = 0; i < IN_MAX_DEVS; i++) {
		int n, ret, count, *binds, *def_binds;
		in_dev_t *dev = &in_devices[i];

		if (dev->binds == NULL)
			continue;

		count = in_bind_count(dev->drv_id);
		binds = dev->binds;
		def_binds = binds + count;

		for (n = 0; n < count; n++)
			if (binds[n] == -1)
				binds[n] = def_binds[n];

		if (dev->drv_data == NULL)
			continue;

		ret = DRV(dev->drv_id).clean_binds(dev->drv_data, binds);
		if (ret == 0) {
			/* no useable binds */
			free(dev->binds);
			dev->binds = NULL;
		}
	}
}

void in_debug_dump(void)
{
	int i;

	printf("# drv probed binds name\n");
	for (i = 0; i < IN_MAX_DEVS; i++) {
		in_dev_t *d = &in_devices[i];
		if (!d->probed && d->name == NULL && d->binds == NULL)
			continue;
		printf("%d %3d %6c %5c %s\n", i, d->drv_id, d->probed ? 'y' : 'n',
			d->binds ? 'y' : 'n', d->name);
	}
}

/* handlers for unknown/not_preset drivers */

static void in_def_probe(void) {}
static void in_def_free(void *drv_data) {}
static int  in_def_get_bind_count(void) { return 0; }
static void in_def_get_def_binds(int *binds) {}
static int  in_def_clean_binds(void *drv_data, int *binds) { return 0; }
static void in_def_set_blocking(void *data, int y) {}
static int  in_def_update_keycode(void *drv_data, int *is_down) { return 0; }
static int  in_def_menu_translate(int keycode) { return keycode; }
static int  in_def_get_key_code(const char *key_name) { return 0; }
static const char *in_def_get_key_name(int keycode) { return NULL; }

void in_init(void)
{
	int i;

	memset(in_drivers, 0, sizeof(in_drivers));
	memset(in_devices, 0, sizeof(in_devices));
	in_dev_count = 0;

	for (i = 0; i < IN_DRVID_COUNT; i++) {
		in_drivers[i].prefix = "none:";
		in_drivers[i].probe = in_def_probe;
		in_drivers[i].free = in_def_free;
		in_drivers[i].get_bind_count = in_def_get_bind_count;
		in_drivers[i].get_def_binds = in_def_get_def_binds;
		in_drivers[i].clean_binds = in_def_clean_binds;
		in_drivers[i].set_blocking = in_def_set_blocking;
		in_drivers[i].update_keycode = in_def_update_keycode;
		in_drivers[i].menu_translate = in_def_menu_translate;
		in_drivers[i].get_key_code = in_def_get_key_code;
		in_drivers[i].get_key_name = in_def_get_key_name;
	}

#ifdef IN_GP2X
	in_gp2x_init(&in_drivers[IN_DRVID_GP2X]);
#endif
#ifdef IN_EVDEV
	in_evdev_init(&in_drivers[IN_DRVID_EVDEV]);
#endif
}

#if 0
int main(void)
{
	int ret;

	in_init();
	in_probe();

	in_set_blocking(1);

#if 1
	while (1) {
		int dev = 0, down;
		ret = in_update_keycode(&dev, &down);
		printf("#%i: %i %i (%s)\n", dev, down, ret, in_get_key_name(dev, ret));
	}
#else
	while (1) {
		ret = in_menu_wait_any();
		printf("%08x\n", ret);
	}
#endif

	return 0;
}
#endif