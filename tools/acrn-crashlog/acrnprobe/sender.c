/*
 * Copyright (C) 2018 Intel Corporation
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <limits.h>
#include "fsutils.h"
#include "strutils.h"
#include "cmdutils.h"
#include "load_conf.h"
#include "sender.h"
#include "probeutils.h"
#include "android_events.h"
#include "history.h"
#include "property.h"
#include "startupreason.h"
#include "log_sys.h"
#include "loop.h"

#ifdef HAVE_TELEMETRICS_CLIENT
#include "telemetry.h"

#define CRASH_SEVERITY 4
#define INFO_SEVERITY 2

struct telemd_data_t {
	char *class;
	char *srcdir;
	char *eventid;
	uint32_t severity;
};
#endif

static int cal_log_filepath(char **out, const struct log_t *log,
				const char *srcname, const char *desdir)
{
	const char *filename;
	int need_timestamp = 0;
	int hours;
	char timebuf[UPTIME_SIZE];

	if (!out || !log || !desdir)
		return -1;

	if (is_ac_filefmt(log->path))
		filename = srcname;
	else
		filename = log->name;

	if (!filename)
		return -1;

	if (!strcmp(log->type, "cmd") || log->lines)
		need_timestamp = 1;

	if (need_timestamp) {
		timebuf[0] = 0;
		get_uptime_string(timebuf, &hours);
		return asprintf(out, "%s/%s_%s", desdir, filename, timebuf);
	}

	return asprintf(out, "%s/%s", desdir, filename);
}

/* get_log_file_* only used to copy regular file which can be mmaped */
static void get_log_file_complete(const char *despath, const char *srcpath)
{
	const int ret = do_copy_tail(srcpath, despath, 0);

	if (ret < 0) {
		LOGE("copy (%s) failed, error (%s)\n", srcpath,
		     strerror(errno));
	}
}

static void get_log_file_tail(const char *despath, const char *srcpath,
				const int lines)
{
	char *start;
	int start_line;
	int file_lines;
	struct mm_file_t *mfile;
	int ret;

	mfile = mmap_file(srcpath);
	if (!mfile) {
		LOGE("mmap (%s) failed, error (%s)\n", srcpath,
		     strerror(errno));
		return;
	}
	file_lines = mm_count_lines(mfile);
	if (file_lines <= 0) {
		LOGW("get lines (%s, %d) failed\n", mfile->path, file_lines);
		goto unmap;
	}
	start_line = MAX(file_lines - lines, 0) + 1;
	start = mm_get_line(mfile, start_line);
	ret = overwrite_file(despath, start);
	if (ret < 0) {
		LOGE("create file with (%s, %p) failed, error (%s)\n",
		     despath, start, strerror(errno));
		goto unmap;
	}

unmap:
	unmap_file(mfile);
}

static void get_log_file(const char *despath, const char *srcpath,
			const char *tail_lines)
{
	int lines;

	if (!tail_lines) {
		get_log_file_complete(despath, srcpath);
		return;
	}

	lines = atoi(tail_lines);
	if (lines > 0)
		get_log_file_tail(despath, srcpath, lines);
	else
		get_log_file_complete(despath, srcpath);
}

static void get_log_node(const char *despath, const char *nodepath)
{
	const int res = do_copy_eof(nodepath, despath);

	if (res < 0) {
		LOGE("copy (%s) failed, error (%s)\n", nodepath,
		     strerror(errno));
	}
}

static void get_log_cmd(const char *despath, const char *cmd)
{
	const int res = exec_out2file(despath, cmd);

	if (res)
		LOGE("get_log_by_cmd exec %s returns (%d)\n", cmd, res);
}

static void get_log_by_type(const char *despath, const struct log_t *log,
				const char *srcpath)
{
	if (!despath || !log || !srcpath)
		return;

	if (!strcmp("file", log->type))
		get_log_file(despath, srcpath, log->lines);
	else if (!strcmp("node", log->type))
		get_log_node(despath, log->path);
	else if (!strcmp("cmd", log->type))
		get_log_cmd(despath, log->path);
}
#ifdef HAVE_TELEMETRICS_CLIENT
static int telemd_send_data(char *payload, char *eventid, uint32_t severity,
				char *class)
{
	int res;
	struct telem_ref *handle = NULL;
	const uint32_t version = 1;

	res = tm_create_record(&handle, severity, class, version);
	if (res < 0) {
		LOGE("failed to create record: %s\n",
		     strerror(-res));
		goto fail;
	}

	/* eventid with 32 character length */
	if (eventid) {
		res = tm_set_event_id(handle, eventid);
		if (res < 0) {
			LOGE("failed to set eventid: %s\n", strerror(-res));
			goto free;
		}
	}

	res = tm_set_payload(handle, payload);
	if (res < 0) {
		LOGE("failed to set payload: %s\n", strerror(-res));
		goto free;
	}

	res = tm_send_record(handle);
	if (res < 0) {
		LOGE("failed to send record: %s\n", strerror(-res));
		goto free;
	}

	tm_free_record(handle);
	return 0;

free:
	tm_free_record(handle);
fail:
	return -1;
}

static void telemd_get_log(struct log_t *log, void *data)
{
	const struct telemd_data_t *d = (struct telemd_data_t *)data;
	char fpath[PATH_MAX];
	char *msg;
	int count;
	int res;
	int i;
	struct dirent **filelist;

	if (d->srcdir == NULL)
		goto send_nologs;

	/* search file which use log->name as substring */
	count = ac_scandir(d->srcdir, &filelist, filter_filename_substr,
			 log->name, NULL);
	if (count < 0) {
		LOGE("search (%s) in dir (%s) failed, error (%s)\n", log->name,
		     d->srcdir, strerror(count));
		return;
	}
	if (!count) {
		LOGE("dir (%s) does not contains (%s)\n", d->srcdir,
		     log->name);
		goto send_nologs;
	}

	for (i = 0; i < count; i++) {
		snprintf(fpath, sizeof(fpath), "%s/%s", d->srcdir,
			 filelist[i]->d_name);
		free(filelist[i]);
		telemd_send_data(fpath, d->eventid,
				 d->severity, d->class);
	}

	free(filelist);

	return;

send_nologs:
	res = asprintf(&msg, "no log generated on %s, check probe's log.",
		       log->name);
	if (res < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	telemd_send_data(msg, d->eventid, d->severity, d->class);
	free(msg);
}
#endif

static void crashlog_get_log(struct log_t *log, void *data)
{

	struct sender_t *crashlog;
	unsigned long long start, end;
	int spent;
	int quota;
	int res;
	char *des;
	char *desdir = (char *)data;

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return;

	quota = atoi(crashlog->spacequota);
	if (!space_available(crashlog->outdir, quota)) {
		hist_raise_infoerror("SPACE_FULL");
		return;
	}

	start = get_uptime();
	if (is_ac_filefmt(log->path)) {
		int i;
		char **files;
		char *name;

		const int count = config_fmt_to_files(log->path, &files);

		if (count < 0) {
			LOGE("parse config format (%s) failed, error (%s)\n",
			     log->path, strerror(count));
			return;
		}
		if (!count) {
			LOGW("no logs found for (%s)\n", log->name);
			return;
		}

		for (i = 0; i < count; i++) {
			name = strrchr(files[i], '/') + 1;
			if (name == (char *)1) {
				LOGE("invalid path (%s) in log (%s)", files[i],
				     log->name);
				continue;
			}
			res = cal_log_filepath(&des, log, name, desdir);
			if (res == -1) {
				LOGE("cal_log_filepath failed, error (%s)\n",
				     strerror(errno));
				continue;
			}
			get_log_by_type(des, log, files[i]);
			free(des);
		}

		for (i = 0; i < count; i++)
			free(files[i]);
		free(files);
	} else {
		res = cal_log_filepath(&des, log, log->name, desdir);
		if (res == -1) {
			LOGE("cal_log_filepath failed, error (%s)\n",
			     strerror(errno));
			return;
		}
		get_log_by_type(des, log, log->path);
		free(des);
	}
	end = get_uptime();

	spent = (int)((end - start) / 1000000000LL);
	if (spent < 5)
		LOGD("get (%s) spend %ds\n", log->name, spent);
	else
		LOGW("get (%s) spend %ds\n", log->name, spent);
}

#ifdef HAVE_TELEMETRICS_CLIENT
static void telemd_send_crash(struct event_t *e)
{
	struct crash_t *crash;
	struct log_t *log;
	char *class;
	char *eventid;
	int id;
	int ret;
	struct telemd_data_t data = {
		.srcdir = e->dir,
		.severity = CRASH_SEVERITY,
	};

	crash = (struct crash_t *)e->private;

	ret = asprintf(&class, "clearlinux/crash/%s", crash->name);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	eventid = generate_eventid256(class);
	if (eventid == NULL) {
		LOGE("generate eventid failed, error (%s)\n", strerror(errno));
		goto free_class;
	}

	data.class = class;
	data.eventid = eventid;

	for_each_log_collect(id, log, crash) {
		if (!log)
			continue;

		log->get(log, (void *)&data);
	}
	if (!strcmp(e->channel, "inotify")) {
		char *des;
		/* get the trigger file */
		ret = asprintf(&des, "%s/%s", e->dir, e->path);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			goto free_eventid;
		}

		if (!file_exists(des)) {
			/* find the original path */
			char *ori;

			ret = asprintf(&ori, "%s/%s", crash->trigger->path,
				       e->path);
			if (ret < 0) {
				LOGE("compute string failed, out of memory\n");
				free(des);
				goto free_eventid;
			}

			LOGW("(%s) unavailable, try the original path (%s)\n",
			     des, ori);
			if (!file_exists(ori)) {
				LOGE("original path (%s) is unavailable\n",
				     ori);
			} else {
				telemd_send_data(ori, eventid, CRASH_SEVERITY,
						 class);
			}

			free(ori);
		} else {
			telemd_send_data(des, eventid, CRASH_SEVERITY, class);
		}

		free(des);
	}
free_eventid:
	free(eventid);
free_class:
	free(class);
}

static void telemd_send_info(struct event_t *e)
{
	struct info_t *info;
	struct log_t *log;
	char *class;
	char *eventid;
	int id;
	int ret;
	struct telemd_data_t data = {
		.srcdir = e->dir,
		.severity = INFO_SEVERITY,
	};

	info = (struct info_t *)e->private;
	ret = asprintf(&class, "clearlinux/info/%s", info->name);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	eventid = generate_eventid256(class);
	if (eventid == NULL) {
		LOGE("generate eventid failed, error (%s)\n", strerror(errno));
		goto free_class;
	}

	data.class = class;
	data.eventid = eventid;

	for_each_log_collect(id, log, info) {
		if (!log)
			continue;

		log->get(log, (void *)&data);
	}

	free(eventid);

free_class:
	free(class);
}

static void telemd_send_uptime(void)
{
	struct sender_t *telemd;
	struct uptime_t *uptime;
	char *class;
	char boot_time[UPTIME_SIZE];
	int hours;
	int ret;
	static int uptime_hours;
	static int loop_uptime_event = 1;

	ret = get_uptime_string(boot_time, &hours);
	if (ret < 0) {
		LOGE("cannot get uptime - %s\n", strerror(-ret));
		return;
	}
	telemd = get_sender_by_name("telemd");
	uptime = telemd->uptime;
	uptime_hours = atoi(uptime->eventhours);
	if (hours / uptime_hours >= loop_uptime_event) {
		char *content;

		loop_uptime_event = (hours / uptime_hours) + 1;
		ret = asprintf(&class, "clearlinux/uptime/%s", boot_time);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			return;
		}

		ret = asprintf(&content, "system boot time: %s", boot_time);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			free(class);
			return;
		}

		telemd_send_data(content, NULL, INFO_SEVERITY, class);
		free(class);
		free(content);
	}
}

static void telemd_send_reboot(void)
{
	struct sender_t *telemd;
	char *class;
	char reason[REBOOT_REASON_SIZE];
	int ret;

	telemd = get_sender_by_name("telemd");
	if (swupdated(telemd)) {
		char *content;

		ret = asprintf(&class, "clearlinux/swupdate/-");
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			return;
		}

		ret = asprintf(&content, "system update to: %s",
			 gbuildversion);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			free(class);
			return;
		}

		telemd_send_data(content, NULL, INFO_SEVERITY, class);
		free(class);
		free(content);
	}

	read_startupreason(reason, sizeof(reason));
	ret = asprintf(&class, "clearlinux/reboot/%s", reason);
	if (ret < 0) {
		LOGE("compute string failed, out of memory\n");
		return;
	}

	telemd_send_data("reboot", NULL, INFO_SEVERITY, class);
	free(class);
}

static int telemd_new_vmevent(const char *line_to_sync,
				const struct vm_t *vm)
{
	char event[ANDROID_WORD_LEN];
	char longtime[ANDROID_WORD_LEN];
	char type[ANDROID_WORD_LEN];
	char rest[PATH_MAX];
	char *vmlogpath = NULL;
	char vmkey[ANDROID_WORD_LEN];
	char *log;
	char *class;
	char *eventid;
	char *files[512];
	int count;
	int i;
	uint32_t severity;
	int res;
	int ret = VMEVT_HANDLED;

	/* VM events in history_event look like this:
	 *
	 * "CRASH   xxxxxxxxxxxxxxxxxxxx  2017-11-11/03:12:59  JAVACRASH
	 * /data/logs/crashlog0_xxxxxxxxxxxxxxxxxxxx"
	 * "REBOOT  xxxxxxxxxxxxxxxxxxxx  2011-11-11/11:20:51  POWER-ON
	 * 0000:00:00"
	 */
	const char * const vm_format =
		ANDROID_ENEVT_FMT ANDROID_KEY_FMT ANDROID_LONGTIME_FMT
		ANDROID_TYPE_FMT ANDROID_LINE_REST_FMT;

	res = sscanf(line_to_sync, vm_format, event, vmkey, longtime,
		     type, rest);
	if (res != 5) {
		LOGE("get an invalid line from (%s), skip\n", vm->name);
		return VMEVT_HANDLED;
	}

	if (strcmp(event, "CRASH") == 0)
		severity = CRASH_SEVERITY;
	else
		severity = INFO_SEVERITY;

	/* if line contains log, fill vmlogpath */
	log = strstr(rest, "/logs/");
	if (log) {
		struct sender_t *crashlog;

		crashlog = get_sender_by_name("crashlog");
		if (!crashlog)
			return VMEVT_HANDLED;

		res = find_file(crashlog->outdir, log + strlen("/logs/"),
				2, &vmlogpath, 1);
		if (res < 0) {
			LOGE("find (%s) in (%s) failed, strerror (%s)\n",
			     log + strlen("/logs/"), crashlog->outdir,
			     strerror(-res));
			return VMEVT_DEFER;
		}
	}

	res = asprintf(&class, "%s/%s/%s", vm->name, event, type);
	if (res < 0) {
		LOGE("compute string failed, out of memory\n");
		ret = VMEVT_DEFER;
		goto free_vmlogpath;
	}

	eventid = generate_eventid256(class);
	if (eventid == NULL) {
		LOGE("generate eventid failed, error (%s)\n", strerror(errno));
		ret = VMEVT_DEFER;
		goto free_class;
	}

	if (!vmlogpath) {
		res = telemd_send_data("no logs", eventid, severity, class);
		if (res == -1)
			ret = VMEVT_DEFER;

		goto free;
	}

	/* send logs */
	count = lsdir(vmlogpath, files, ARRAY_SIZE(files));
	if (count > 2) {
		for (i = 0; i < count; i++) {
			if (!strstr(files[i], "/.") &&
			    !strstr(files[i], "/..")) {
				res = telemd_send_data(files[i], eventid,
						       severity, class);
				if (res == -1)
					ret = VMEVT_DEFER;
			}
		}
	} else if (count == 2) {
		char *content;

		res = asprintf(&content, "no logs under (%s)", vmlogpath);
		if (res > 0) {
			res = telemd_send_data(content, eventid, severity,
					       class);
			if (res == -1)
				ret = VMEVT_DEFER;
			free(content);
		} else {
			LOGE("compute string failed, out of memory\n");
			ret = VMEVT_DEFER;
		}
	} else if (count < 0) {
		LOGE("lsdir (%s) failed, error (%s)\n", vmlogpath,
		     strerror(-count));
		ret = VMEVT_DEFER;
	} else {
		LOGE("get (%d) files in (%s) ???\n", count, vmlogpath);
		ret = VMEVT_DEFER;
	}

	while (count > 0)
		free(files[--count]);

free:
	free(eventid);
free_class:
	free(class);
free_vmlogpath:
	if (vmlogpath)
		free(vmlogpath);

	return ret;
}

static void telemd_send(struct event_t *e)
{
	int id;
	struct log_t *log;

	for_each_log(id, log, conf) {
		if (!log)
			continue;

		log->get = telemd_get_log;
	}

	switch (e->event_type) {
	case CRASH:
		telemd_send_crash(e);
		break;
	case INFO:
		telemd_send_info(e);
		break;
	case UPTIME:
		telemd_send_uptime();
		break;
	case REBOOT:
		telemd_send_reboot();
		break;
	case VM:
		refresh_vm_history(get_sender_by_name("telemd"),
				   telemd_new_vmevent);
		break;
	default:
		LOGE("unsupoorted event type %d\n", e->event_type);
	}
}
#endif

static void crashlog_send_crash(struct event_t *e)
{
	struct crash_t *crash;
	struct log_t *log;
	struct sender_t *crashlog;
	char *key  = NULL;
	char *trfile = NULL;
	char *data0 = NULL;
	char *data1 = NULL;
	char *data2 = NULL;
	int id;
	int ret = 0;
	int quota;
	struct crash_t *rcrash = (struct crash_t *)e->private;

	if (!strcmp(rcrash->trigger->type, "dir")) {
		ret = asprintf(&trfile, "%s/%s", rcrash->trigger->path,
			       e->path);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			return;
		}
	}

	crash = rcrash->reclassify(rcrash, trfile, &data0, &data1, &data2);
	if (crash == NULL) {
		LOGE("reclassify crash (%s) failed\n", rcrash->name);
		goto free_trfile;
	}

	/* change the class for other senders */
	e->private = (void *)crash;
	key = generate_event_id("CRASH", crash->name);
	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		goto free_data;
	}

	if (to_collect_logs(crash) ||
	    !strcmp(e->channel, "inotify")) {
		e->dir = generate_log_dir(MODE_CRASH, key);
		if (e->dir == NULL) {
			LOGE("generate crashlog dir failed\n");
			goto free_key;
		}

		generate_crashfile(e->dir, "CRASH", key,
				   crash->name,
				   data0, data1, data2);
		for_each_log_collect(id, log, crash) {
			if (!log)
				continue;

			log->get(log, (void *)e->dir);
		}

	}

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		goto free_key;

	quota = atoi(crashlog->spacequota);
	if (!space_available(crashlog->outdir, quota)) {
		hist_raise_infoerror("SPACE_FULL");
	} else if (!strcmp(e->channel, "inotify")) {
		/* get the trigger file */
		char *src;
		char *des;

		ret = asprintf(&des, "%s/%s", e->dir, e->path);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			goto free_key;
		}

		ret = asprintf(&src, "%s/%s", crash->trigger->path, e->path);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			free(des);
			goto free_key;
		}

		ret = do_copy_tail(src, des, 0);
		if (ret < 0) {
			LOGE("copy (%s) to (%s) failed, error (%s)\n",
			     src, des, strerror(-ret));
		}

		free(src);
		free(des);
	}

	hist_raise_event("CRASH", crash->name, e->dir, "", key);

free_key:
	free(key);
free_data:
	free(data0);
	free(data1);
	free(data2);
free_trfile:
	free(trfile);
}

static void crashlog_send_info(struct event_t *e)
{
	int id;
	struct info_t *info = (struct info_t *)e->private;
	struct log_t *log;
	char *key = generate_event_id("INFO", info->name);

	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		return;
	}

	if (to_collect_logs(info)) {
		e->dir = generate_log_dir(MODE_STATS, key);
		if (e->dir == NULL) {
			LOGE("generate crashlog dir failed\n");
			goto free_key;
		}

		for_each_log_collect(id, log, info) {
			if (!log)
				continue;
			log->get(log, (void *)e->dir);
		}
	}

	hist_raise_event("INFO", info->name, e->dir, "", key);

free_key:
	free(key);
}

static void crashlog_send_uptime(void)
{
	hist_raise_uptime(NULL);
}

static void crashlog_send_reboot(void)
{
	char reason[REBOOT_REASON_SIZE];
	char *key;
	struct sender_t *crashlog;

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return;

	if (swupdated(crashlog)) {
		key = generate_event_id("INFO", "SWUPDATE");
		if (key == NULL) {
			LOGE("generate event id failed, error (%s)\n",
			     strerror(errno));
			return;
		}

		hist_raise_event("INFO", "SWUPDATE", NULL, "", key);
		free(key);
	}

	read_startupreason(reason, sizeof(reason));
	key = generate_event_id("REBOOT", reason);
	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		return;
	}

	hist_raise_event("REBOOT", reason, NULL, "", key);
	free(key);
}

static int crashlog_new_vmevent(const char *line_to_sync,
					const struct vm_t *vm)
{
	struct sender_t *crashlog;
	char event[ANDROID_WORD_LEN];
	char longtime[ANDROID_WORD_LEN];
	char type[ANDROID_WORD_LEN];
	char rest[PATH_MAX];
	char vmkey[ANDROID_WORD_LEN];
	char *vmlogpath = NULL;
	char *key;
	char *log;
	int ret = VMEVT_HANDLED;
	int res;
	int quota;
	int cnt;
	char *dir;

	/* VM events in history_event like this:
	 *
	 * "CRASH   xxxxxxxxxxxxxxxxxxxx  2017-11-11/03:12:59  JAVACRASH
	 * /data/logs/crashlog0_xxxxxxxxxxxxxxxxxxxx"
	 * "REBOOT  xxxxxxxxxxxxxxxxxxxx  2011-11-11/11:20:51  POWER-ON
	 * 0000:00:00"
	 */
	const char * const vm_format =
		ANDROID_ENEVT_FMT ANDROID_KEY_FMT ANDROID_LONGTIME_FMT
		ANDROID_TYPE_FMT ANDROID_LINE_REST_FMT;

	res = sscanf(line_to_sync, vm_format, event, vmkey, longtime,
		     type, rest);
	if (res != 5) {
		LOGE("get an invalid line from (%s), skip\n", vm->name);
		return ret;
	}

	crashlog = get_sender_by_name("crashlog");
	if (!crashlog)
		return ret;

	quota = atoi(crashlog->spacequota);
	if (!space_available(crashlog->outdir, quota)) {
		hist_raise_infoerror("SPACE_FULL");
		return ret;
	}

	key = generate_event_id("SOS", vmkey);
	if (key == NULL) {
		LOGE("generate event id failed, error (%s)\n",
		     strerror(errno));
		return VMEVT_DEFER;
	}

	dir = generate_log_dir(MODE_VMEVENT, key);
	if (dir == NULL) {
		LOGE("generate crashlog dir failed\n");
		ret = VMEVT_DEFER;
		goto free_key;
	}

	/* if line contains log, we need dump each file in the logdir
	 */
	log = strstr(rest, "/logs/");
	if (log) {
		vmlogpath = log + 1;
		res = e2fs_dump_dir_by_dpath(vm->datafs, vmlogpath, dir, &cnt);
		if (res == -1) {
			if (cnt) {
				LOGE("dump (%s) abort at (%d)\n", vmlogpath,
				     cnt);
				ret = VMEVT_DEFER;
			} else {
				LOGW("(%s) is missing\n", vmlogpath);
				ret = VMEVT_HANDLED; /* missing logdir */
			}
			res = remove(dir);
			if (res == -1 && errno != ENOENT)
				LOGE("remove %s faield (%d)\n", dir, -errno);

			goto free_dir;
		}
	}

	generate_crashfile(dir, event, key, type, vm->name,
			   vmkey, NULL);
	hist_raise_event(vm->name, type, dir, "", key);

free_dir:
	free(dir);
free_key:
	free(key);

	return ret;
}

static void crashlog_send(struct event_t *e)
{

	int id;
	struct log_t *log;

	for_each_log(id, log, conf) {
		if (!log)
			continue;

		log->get = crashlog_get_log;
	}
	switch (e->event_type) {
	case CRASH:
		crashlog_send_crash(e);
		break;
	case INFO:
		crashlog_send_info(e);
		break;
	case UPTIME:
		crashlog_send_uptime();
		break;
	case REBOOT:
		crashlog_send_reboot();
		break;
	case VM:
		refresh_vm_history(get_sender_by_name("crashlog"),
				   crashlog_new_vmevent);
		break;
	default:
		LOGE("unsupoorted event type %d\n", e->event_type);
	}
}

int init_sender(void)
{
	int ret;
	int id;
	int fd;
	struct sender_t *sender;
	struct uptime_t *uptime;

	for_each_sender(id, sender, conf) {
		if (!sender)
			continue;

		ret = asprintf(&sender->log_vmrecordid, "%s/VM_eventsID.log",
			       sender->outdir);
		if (ret < 0) {
			LOGE("compute string failed, out of memory\n");
			return -ENOMEM;
		}

		if (!directory_exists(sender->outdir))
			if (mkdir_p(sender->outdir) < 0) {
				LOGE("mkdir (%s) failed, error (%s)\n",
				     sender->outdir, strerror(errno));
				return -errno;
			}

		ret = init_properties(sender);
		if (ret) {
			LOGE("init sender failed\n");
			exit(-1);
		}

		/* touch uptime file, to add inotify */
		uptime = sender->uptime;
		if (uptime) {
			fd = open(uptime->path, O_RDWR | O_CREAT, 0666);
			if (fd < 0) {
				LOGE("open failed with (%s, %d), error (%s)\n",
				     uptime->path, atoi(uptime->frequency),
				     strerror(errno));
				return -errno;
			}
			close(fd);
		}

		if (!strncmp(sender->name, "crashlog",
			     strlen(sender->name))) {
			sender->send = crashlog_send;
			ret = prepare_history();
			if (ret)
				return -1;
#ifdef HAVE_TELEMETRICS_CLIENT
		} else if (!strncmp(sender->name, "telemd",
				    strlen(sender->name))) {
			sender->send = telemd_send;
#endif
		}
	}

	return 0;
}
