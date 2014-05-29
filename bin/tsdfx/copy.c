/*-
 * Copyright (c) 2014 Universitetet i Oslo
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if HAVE_BSD_UNISTD_H
#include <bsd/unistd.h>
#endif

#include <tsd/strutil.h>

#include "tsdfx.h"
#include "tsdfx_log.h"
#include "tsdfx_task.h"
#include "tsdfx_copy.h"

struct copy_task {
	/* what to copy */
	char srcpath[PATH_MAX];
	char dstpath[PATH_MAX];

	/* place in the task list */
	int index;

	/* state */
	enum task_state state;

	/* child process */
	uid_t uid;
	gid_t gid;
	pid_t pid;
};

/* for our purposes, a doubly-linked list would be more efficient */
static struct copy_task **copy_tasks;
static size_t copy_sz;
static int copy_len;
static int copy_running;

/* max concurrent copy tasks */
int tsdfx_copy_max_tasks = 8;

/*
 * Return the index of the first copy task that matches the specified
 * source and / or destination.
 */
int
tsdfx_copy_find(int i, const char *srcpath, const char *dstpath)
{

	for (; i < copy_len; ++i) {
		if (srcpath && strcmp(srcpath, copy_tasks[i]->srcpath) != 0)
			continue;
		if (dstpath && strcmp(dstpath, copy_tasks[i]->dstpath) != 0)
			continue;
		return (i);
	}
	return (-1);
}

/*
 * Add a task to the task list.
 */
int
tsdfx_copy_add(struct copy_task *task)
{
	struct copy_task **tasks;
	size_t sz;

	VERBOSE("%s -> %s", task->srcpath, task->dstpath);

	/* make room */
	while (copy_len >= (int)copy_sz) {
		sz = copy_sz ? copy_sz * 2 : 16;
		if ((tasks = realloc(copy_tasks, sz * sizeof *tasks)) == NULL)
			return (-1);
		memset(tasks + copy_sz, 0, (sz - copy_sz) * sizeof *tasks);
		copy_tasks = tasks;
		copy_sz = sz;
	}

	/* append the task to the list */
	assert(copy_len + 1 <= (int)copy_sz);
	copy_tasks[copy_len] = task;
	task->index = copy_len++;
	VERBOSE("%d jobs, %d running", copy_len, copy_running);
	return (task->index);
}

/*
 * Remove a task from the task list.
 */
int
tsdfx_copy_remove(struct copy_task *task)
{
	int i;

	VERBOSE("%s -> %s", task->srcpath, task->dstpath);

	i = task->index;
	if (copy_tasks[i] != task)
		return (-1);
	if (i + 1 < copy_len)
		memmove(copy_tasks + i, copy_tasks + i + 1,
		    (copy_len - (i + 1)) * sizeof *copy_tasks);
	memset(copy_tasks + copy_len, 0, sizeof *copy_tasks);
	--copy_len;
	VERBOSE("%d jobs, %d running", copy_len, copy_running);
	for (; i < copy_len; ++i)
		copy_tasks[i]->index = i;
	return (0);
}

/*
 * Prepare a copy task.
 * XXX inefficient deduplication, it would be better to lstat the source
 * file and look it up by st_dev / st_ino.
 */
struct copy_task *
tsdfx_copy_new(const char *srcpath, const char *dstpath)
{
	struct copy_task *task;
	struct stat st;
	int serrno;

	VERBOSE("%s -> %s", srcpath, dstpath);
	if (lstat(srcpath, &st) == -1)
		return (NULL);
	if ((task = calloc(1, sizeof *task)) == NULL)
		return (NULL);
	task->uid = st.st_uid;
	task->gid = st.st_gid;
	task->pid = -1;
	if (strlcpy(task->srcpath, srcpath, sizeof task->srcpath) >=
	    sizeof task->srcpath)
		goto fail;
	if (strlcpy(task->dstpath, dstpath, sizeof task->dstpath) >=
	    sizeof task->dstpath)
		goto fail;
	if (tsdfx_copy_add(task) < 0)
		goto fail;
	task->state = TASK_IDLE;
	return (task);
fail:
	serrno = errno;
	free(task);
	errno = serrno;
	return (NULL);
}

/*
 * Delete a copy task.
 */
void
tsdfx_copy_delete(struct copy_task *task)
{

	if (task == NULL)
		return;
	VERBOSE("%s -> %s", task->srcpath, task->dstpath);
	if (task->pid != -1)
		tsdfx_copy_stop(task);
	tsdfx_copy_remove(task);
	free(task);
}

/*
 * Fork a child process and start a copy task inside.
 *
 * It would be great if we could chdir / chroot into the target directory,
 * or something like /var/empty, but we'd have to open the source file
 * (and possibly the destination file as well) before dropping privileges.
 */
int
tsdfx_copy_start(struct copy_task *task)
{

	VERBOSE("%s -> %s", task->srcpath, task->dstpath);

	if (task->state == TASK_RUNNING)
		return (0);
	if (task->state != TASK_IDLE)
		return (-1);
	task->state = TASK_STARTING;

	/* fork the copier child */
	fflush(NULL);
	if ((task->pid = fork()) < 0)
		goto fail;

	/* child */
	if (task->pid == 0) {
		VERBOSE("child process for %s -> %s",
		    task->srcpath, task->dstpath);
#if HAVE_SETPROCTITLE
		/* set process title if possible */
		setproctitle("copy %s to %s", task->srcpath, task->dstpath);
#endif

#if HAVE_CLOSEFROM
		/* we really really need a linux alternative... */
		closefrom(3);
#endif

		/* drop privileges */
		/* XXX hard error? */
		setgid(task->gid);
		setuid(task->uid);
		if (geteuid() == 0)
			WARNING("copying %s with uid 0", task->srcpath);
		if (getegid() == 0)
			WARNING("copying %s with gid 0", task->srcpath);

		/* set safe umask */
		umask(007);

		/* run the copy task */
		_exit(!!tsdfx_copier(task->srcpath, task->dstpath));
	}

	/* parent */
	++copy_running;
	VERBOSE("%d jobs, %d running", copy_len, copy_running);
	task->state = TASK_RUNNING;
	return (0);
fail:
	task->state = TASK_DEAD;
	return (-1);
}

/*
 * Poll the state of a child process.
 */
int
tsdfx_copy_poll(struct copy_task *task)
{
	enum task_state state;
	int ret;

	state = task->state;
	ret = tsdfx_task_poll(task->pid, &task->state);
	if (state == TASK_RUNNING && task->state != state) {
		--copy_running;
		VERBOSE("%d jobs, %d running", copy_len, copy_running);
	}
	return (ret);
}

/*
 * Stop a copy task.  We make three attempts: first we check to see if the
 * child is already dead.  Then we wait 10 ms and check again; if it still
 * isn't dead, we send a SIGTERM, wait 10 ms, and check again.  If it
 * still isn't dead after the SIGTERM, we send a SIGKILL, wait 10 ms, and
 * try one last time.
 */
int
tsdfx_copy_stop(struct copy_task *task)
{
	int sig[] = { SIGCONT, SIGTERM, SIGKILL, -1 };
	int i;

	VERBOSE("%s -> %s", task->srcpath, task->dstpath);

	/* check current state */
	if (task->state != TASK_RUNNING)
		return (-1);
	task->state = TASK_STOPPING;
	--copy_running;
	VERBOSE("%d jobs, %d running", copy_len, copy_running);

	/* reap the child */
	for (i = 0; sig[i] >= 0; ++i) {
		tsdfx_copy_poll(task);
		if (task->state != TASK_STOPPING)
			break;
		/* not dead yet; kill, wait 10 ms and retry */
		if (sig[i])
			kill(task->pid, sig[i]);
		kill(task->pid, SIGCONT);
		usleep(10000);
	}

	/* either done or gave up */
	if (sig[i] < 0) {
		warnx("gave up waiting for child %d", (int)task->pid);
		task->state = TASK_DEAD;
	}

	/* in summary... */
	task->pid = -1;
	if (task->state != TASK_STOPPED)
		return (-1);
	return (0);
}

/*
 * Given source and destination directories and a list of files to copy,
 * start copy tasks for each file.
 */
int
tsdfx_copy_wrap(const char *srcdir, const char *dstdir, const char *files)
{
	struct stat srcst, dstst;
	char srcpath[PATH_MAX], *sf, dstpath[PATH_MAX], *df;
	size_t slen, dlen, maxlen;
	const char *p, *q;

	/* prime the source and destination paths */
	slen = strlcpy(srcpath, srcdir, sizeof srcpath);
	dlen = strlcpy(dstpath, dstdir, sizeof dstpath);
	if (slen >= sizeof srcpath || dlen >= sizeof dstpath) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	/* sf and df point to the terminating NULS */
	sf = srcpath + slen;
	df = dstpath + dlen;
	/* maxlen is the maximum acceptable file name length */
	slen = sizeof srcpath - slen - 1;
	dlen = sizeof dstpath - dlen - 1;
	maxlen = slen > dlen ? dlen : slen;

	for (p = q = files; *p != '\0'; p = ++q) {
		while (*q != '\0' && *q != '\n')
			++q;
		if (q == p) {
			/* empty line, XXX should warn */
			continue;
		}
		if ((size_t)(q - p) >= maxlen) {
			/* too long, XXX should warn */
			continue;
		}
		/* XXX copier doesn't create directories, so skip */
		if (memchr(p + 1, '/', q - p)) {
			VERBOSE("ignoring %.*s", (int)(q - p), p);
			continue;
		}
		/* XXX end */
		memcpy(sf, p, q - p);
		sf[q - p] = '\0';
		memcpy(df, p, q - p);
		df[q - p] = '\0';
		if (tsdfx_copy_find(0, srcpath, dstpath) < 0) {
			if (stat(srcpath, &srcst) == 0 &&
			    stat(dstpath, &dstst) == 0 &&
			    srcst.st_size == dstst.st_size &&
			    srcst.st_mtime == dstst.st_mtime)
				continue;
			tsdfx_copy_new(srcpath, dstpath);
		}
	}
	return (0);
}

/*
 * Monitor child processes
 */
int
tsdfx_copy_iter(void)
{
	int i;

	for (i = 0; i < copy_len; ++i) {
		if (copy_tasks[i]->state == TASK_IDLE &&
		    copy_running < tsdfx_copy_max_tasks)
			tsdfx_copy_start(copy_tasks[i]);
		tsdfx_copy_poll(copy_tasks[i]);
		if (copy_tasks[i]->state != TASK_RUNNING)
			tsdfx_copy_delete(copy_tasks[i]);
	}
	return (0);
}

/*
 * Initialize the copier subsystem
 */
int
tsdfx_copy_init(void)
{

	/* nothing for now */
	return (0);
}
