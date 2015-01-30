/*-
 * Copyright (c) 2014-2015 Universitetet i Oslo
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

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if HAVE_BSD_UNISTD_H
#include <bsd/unistd.h>
#endif

#include <tsd/hash.h>
#include <tsd/log.h>
#include <tsd/strutil.h>
#include <tsd/task.h>

/*
 * Clear task credentials.
 */
static void
tsd_task_clearcred(struct tsd_task *t)
{

	memset(t->user, 0, sizeof t->user);
	t->uid = (uid_t)-1;
	memset(t->gids, 0, sizeof t->gids);
	t->gids[0] = (gid_t)-1;
	t->ngids = 1;
}

/*
 * Create a new task.
 */
struct tsd_task *
tsd_task_create(const char *name, tsd_task_func *func, void *ud)
{
	struct tsd_task *t;

	if ((t = calloc(1, sizeof *t)) == NULL)
		return (NULL);
	if (strlcpy(t->name, name, sizeof t->name) >= sizeof t->name) {
		memset(t, 0, sizeof *t);
		free(t);
		errno = ENAMETOOLONG;
		return (NULL);
	}
	t->h = tsd_strhash(t->name);
	t->state = TASK_IDLE;
	tsd_task_clearcred(t);
	t->func = func;
	t->pid = -1;
	t->ud = ud;
	VERBOSE("%s(\"%s\") = %p", __func__, name, t);
	return (t);
}

/*
 * Destroy a task.
 */
void
tsd_task_destroy(struct tsd_task *t)
{

	VERBOSE("%s(%p)", __func__, t);
	if (t == NULL)
		return;
	if (t->state == TASK_RUNNING)
		tsd_task_stop(t);
	/* assert(t->state != TASK_STOPPING); */
	memset(t, 0, sizeof *t);
	free(t);
}

/*
 * Set the task credentials to those of the given user.
 */
int
tsd_task_setuser(struct tsd_task *t, const char *user)
{
	struct passwd *pwd;

	if (t->state != TASK_IDLE) {
		errno = EBUSY;
		return (-1);
	}
	tsd_task_clearcred(t);
	t->ngids = sizeof t->gids / sizeof t->gids[0];
	if ((pwd = getpwnam(user)) == NULL ||
	    strlcpy(t->user, pwd->pw_name, sizeof t->user) >= sizeof t->user ||
	    (t->ngids = getgroups(t->ngids, t->gids)) < 1) {
		tsd_task_clearcred(t);
		return (-1);
	}
	t->uid = pwd->pw_uid;
	return (0);
}

/*
 * Set the task credentials to the given UID and GID.
 */
int
tsd_task_setcred(struct tsd_task *t, uid_t uid, gid_t *gids, int ngids)
{

	if (t->state != TASK_IDLE) {
		errno = EBUSY;
		return (-1);
	}
	tsd_task_clearcred(t);
	if (ngids < 1 || ngids > (int)(sizeof t->gids / sizeof t->gids[0]))
		return (-1);
	snprintf(t->user, sizeof t->user, "(%lu:%lu)",
	    (unsigned long)uid, (unsigned long)gids[0]);
	t->uid = uid;
	memcpy(t->gids, gids, ngids * sizeof *gids);
	t->ngids = ngids;
	return (0);
}

/*
 * Fork a child process and start a task inside it.
 *
 * XXX consider adding support for chroot
 */
int
tsd_task_start(struct tsd_task *t)
{
	int ret;

	VERBOSE("%s(%p)", __func__, t);

	if (t->state == TASK_RUNNING)
		return (0);
	if (t->state != TASK_IDLE)
		return (-1);
	t->state = TASK_STARTING;

	/* fork the child */
	fflush(NULL);
	if ((t->pid = fork()) < 0)
		goto fail;

	/* child */
	if (t->pid == 0) {
//		VERBOSE("copy child for %s", t->name);
#if HAVE_SETPROCTITLE
		/* set process title if possible */
		setproctitle("%s", t->name);
#endif

#if HAVE_CLOSEFROM
		/* we really really need a linux alternative... */
		closefrom(3);
#endif

		/* drop privileges */
		if (geteuid() != t->uid || getuid() != t->uid ||
		    getegid() != t->gids[0] || getgid() != t->gids[0]) {
#if HAVE_SETGROUPS
			ret = setgroups(t->ngids, t->gids);
#else
			ret = setgid(t->gids[0]);
#endif
			if (ret != 0)
				WARNING("failed to set process group");
#if HAVE_INITGROUPS
//			if (*t->user && ret == 0)
//				if ((ret = initgroups(t->user, t->gid)) != 0)
//					WARNING("failed to set additional groups");
#endif
			if (ret == 0 && (ret = setuid(t->uid)) != 0)
				WARNING("failed to set process user");
			if (ret != 0)
				_exit(1);
		}
		(*t->func)(t->ud);
		_exit(1);
	}

	/* parent */
//	++copy_running;
//	VERBOSE("%d jobs, %d running", copy_len, copy_running);
	t->state = TASK_RUNNING;
	return (0);
fail:
	t->state = TASK_DEAD;
	return (-1);
}

/*
 * Stop a task.  We make three attempts: first we check to see if the
 * child is already dead.  Then we wait 10 ms and check again; if it still
 * isn't dead, we send a SIGTERM, wait 10 ms, and check again.  If it
 * still isn't dead after the SIGTERM, we send a SIGKILL, wait 10 ms, and
 * try one last time.
 */
int
tsd_task_stop(struct tsd_task *t)
{
	static const int sig[] = { SIGCONT, SIGTERM, SIGKILL, -1 };
	int i;

	VERBOSE("%s(%p)", __func__, t);

	/* check current state */
	if (t->state != TASK_RUNNING)
		return (-1);
	t->state = TASK_STOPPING;

	/* reap the child */
	for (i = 0; sig[i] >= 0; ++i) {
		tsd_task_poll(t);
		if (t->state != TASK_STOPPING)
			break;
		/* not dead yet; kill, wait 100 ms and retry */
		if (sig[i])
			kill(t->pid, sig[i]);
		kill(t->pid, SIGCONT);
		usleep(100000);
	}

	/* either done or gave up */
	if (sig[i] < 0) {
		WARNING("gave up waiting for child %d", (int)t->pid);
		t->state = TASK_DEAD;
	}

	/* in summary... */
//	--copy_running;
//	VERBOSE("%d jobs, %d running", copy_len, copy_running);
	t->pid = -1;
	if (t->state != TASK_STOPPED)
		return (-1);
	return (0);
}


/*
 * Poll a task to see if it's still running.
 */
int
tsd_task_poll(struct tsd_task *t)
{
	int ret, serrno;

	VERBOSE("%s(%p)", __func__, t);

	if (t->state != TASK_RUNNING && t->state != TASK_STOPPING)
		return (-1);
	ret = waitpid(t->pid, &t->status, WNOHANG);
	if (ret < 0) {
		serrno = errno;
		/* already reaped, or something is wrong */
		WARNING("waitpid(%lu): %s",
		    (unsigned long)t->pid, strerror(errno));
		t->state = TASK_DEAD;
		errno = serrno;
		return (-1);
	} else if (ret == 0) {
		/* still running */
		return (0);
	} else if (ret == t->pid) {
		if (!WIFEXITED(t->status))
			t->state = TASK_DEAD;
		else if (WEXITSTATUS(t->status) != 0)
			t->state = TASK_FAILED;
		else
			t->state = TASK_STOPPED;
		t->pid = -1;
		return (0);
	} else {
		/* wtf? */
		ERROR("waitpid(%lu) returned %d",
		    (unsigned long)t->pid, ret);
		errno = EAGAIN;
		return (-1);
	}
}
