/* $OpenBSD$ */

/*
 * Copyright (c) 2022 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/un.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>
#include <systemd/sd-id128.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tmux.h"

#ifndef SD_ID128_UUID_FORMAT_STR
#define SD_ID128_UUID_FORMAT_STR \
	"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"
#endif

int
systemd_activated(void)
{
	return (sd_listen_fds(0) >= 1);
}

int
systemd_create_socket(int flags, char **cause)
{
	int			fds;
	int			fd;
	struct sockaddr_un	sa;
	socklen_t		addrlen = sizeof sa;

	fds = sd_listen_fds(0);
	if (fds > 1) { /* too many file descriptors */
		errno = E2BIG;
		goto fail;
	}

	if (fds == 1) { /* socket-activated */
		fd = SD_LISTEN_FDS_START;
		if (!sd_is_socket_unix(fd, SOCK_STREAM, 1, NULL, 0)) {
			errno = EPFNOSUPPORT;
			goto fail;
		}
		if (getsockname(fd, (struct sockaddr *)&sa, &addrlen) == -1)
			goto fail;
		socket_path = xstrdup(sa.sun_path);
		return (fd);
	}

	return (server_create_socket(flags, cause));

fail:
	if (cause != NULL)
		xasprintf(cause, "systemd socket error (%s)", strerror(errno));
	return (-1);
}

struct systemd_job_watch {
	const char	*path;
	int		 done;
};

static int
job_removed_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
	struct systemd_job_watch *watch = userdata;
	const char		 *path = NULL;
	uint32_t		 id;
	int			 r;

	/* This handler could be called during the sd_bus_call. */
	if (watch->path == NULL)
		return 0;

	r = sd_bus_message_read(m, "uo", &id, &path);
	if (r < 0)
		return (r);

	if (strcmp(path, watch->path) == 0)
		watch->done = 1;

	return (0);
}

int
systemd_move_to_new_cgroup(char **cause)
{
	sd_bus_error		 error = SD_BUS_ERROR_NULL;
	sd_bus_message		*m = NULL, *reply = NULL;
	sd_bus 			*bus = NULL;
	sd_bus_slot		*slot = NULL;
	char			*name, *desc, *slice;
	sd_id128_t		 uuid;
	int			 r;
	uint64_t		 elapsed_usec;
	pid_t			 pid, parent_pid;
	struct timeval		 start, now;
	struct systemd_job_watch watch = {};

	gettimeofday(&start, NULL);

	/* Connect to the session bus. */
	r = sd_bus_default_user(&bus);
	if (r < 0) {
		xasprintf(cause, "failed to connect to session bus: %s",
		    strerror(-r));
		goto finish;
	}

	/* Start watching for JobRemoved events */
	r = sd_bus_match_signal(bus, &slot,
	    "org.freedesktop.systemd1",
	    "/org/freedesktop/systemd1",
	    "org.freedesktop.systemd1.Manager",
	    "JobRemoved",
	    job_removed_handler,
	    &watch);
	if (r < 0) {
		xasprintf(cause, "failed to create match signal: %s",
		    strerror(-r));
		goto finish;
	}

	/* Start building the method call. */
	r = sd_bus_message_new_method_call(bus, &m,
	    "org.freedesktop.systemd1",
	    "/org/freedesktop/systemd1",
	    "org.freedesktop.systemd1.Manager",
	    "StartTransientUnit");
	if (r < 0) {
		xasprintf(cause, "failed to create bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Generate a unique name for the new scope, to avoid collisions. */
	r = sd_id128_randomize(&uuid);
	if (r < 0) {
		xasprintf(cause, "failed to generate uuid: %s", strerror(-r));
		goto finish;
	}
	xasprintf(&name, "tmux-spawn-" SD_ID128_UUID_FORMAT_STR ".scope",
	    SD_ID128_FORMAT_VAL(uuid));
	r = sd_bus_message_append(m, "s", name);
	free(name);
	if (r < 0) {
		xasprintf(cause, "failed to append to bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Mode: fail if there's a queued unit with the same name. */
	r = sd_bus_message_append(m, "s", "fail");
	if (r < 0) {
		xasprintf(cause, "failed to append to bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Start properties array. */
	r = sd_bus_message_open_container(m, 'a', "(sv)");
	if (r < 0) {
		xasprintf(cause, "failed to start properties array: %s",
		    strerror(-r));
		goto finish;
	}

	pid = getpid();
	parent_pid = getppid();
	xasprintf(&desc, "tmux child pane %ld launched by process %ld",
	    (long)pid, (long)parent_pid);
	r = sd_bus_message_append(m, "(sv)", "Description", "s", desc);
	free(desc);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/*
	 * Make sure that the session shells are terminated with SIGHUP since
	 * bash and friends tend to ignore SIGTERM.
	 */
	r = sd_bus_message_append(m, "(sv)", "SendSIGHUP", "b", 1);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/*
	 * Inherit the slice from the parent process, or default to
	 * "app-tmux.slice" if that fails.
	 */
	r = sd_pid_get_user_slice(parent_pid, &slice);
	if (r < 0) {
		slice = xstrdup("app-tmux.slice");
	}
	r = sd_bus_message_append(m, "(sv)", "Slice", "s", slice);
	free(slice);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/* PIDs to add to the scope: length - 1 array of uint32_t. */
	r = sd_bus_message_append(m, "(sv)", "PIDs", "au", 1, pid);
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/* Clean up the scope even if it fails. */
	r = sd_bus_message_append(m, "(sv)", "CollectMode", "s",
	    "inactive-or-failed");
	if (r < 0) {
		xasprintf(cause, "failed to append to properties: %s",
		    strerror(-r));
		goto finish;
	}

	/* End properties array. */
	r = sd_bus_message_close_container(m);
	if (r < 0) {
		xasprintf(cause, "failed to end properties array: %s",
		    strerror(-r));
		goto finish;
	}

	/* aux is currently unused and should be passed an empty array. */
	r = sd_bus_message_append(m, "a(sa(sv))", 0);
	if (r < 0) {
		xasprintf(cause, "failed to append to bus message: %s",
		    strerror(-r));
		goto finish;
	}

	/* Call the method with a timeout of 1 second = 1e6 us. */
	r = sd_bus_call(bus, m, 1000000, &error, &reply);
	if (r < 0) {
		if (error.message != NULL) {
			/* We have a specific error message from sd-bus. */
			xasprintf(cause, "StartTransientUnit call failed: %s",
			    error.message);
		} else {
			xasprintf(cause, "StartTransientUnit call failed: %s",
			    strerror(-r));
		}
		goto finish;
	}

	/* Get the job (object path) from the reply */
	r = sd_bus_message_read(reply, "o", &watch.path);
	if (r < 0) {
		xasprintf(cause, "failed to parse method reply: %s",
		    strerror(-r));
		goto finish;
	}

	while (!watch.done) {
		/* Process events including callbacks. */
		r = sd_bus_process(bus, NULL);
		if (r < 0) {
			xasprintf(cause,
			    "failed waiting for cgroup allocation: %s",
			    strerror(-r));
			goto finish;
		}

		/*
		 * A positive return means we handled an event and should keep
		 * processing; zero indicates no events available, so wait.
		 */
		if (r > 0)
			continue;

		gettimeofday(&now, NULL);
		elapsed_usec = (now.tv_sec - start.tv_sec) * 1000000 +
		    now.tv_usec - start.tv_usec;

		if (elapsed_usec >= 1000000) {
			xasprintf(cause,
			    "timeout waiting for cgroup allocation");
			goto finish;
		}

		r = sd_bus_wait(bus, 1000000 - elapsed_usec);
		if (r < 0) {
			xasprintf(cause,
			    "failed waiting for cgroup allocation: %s",
			    strerror(-r));
			goto finish;
		}
	}

finish:
	sd_bus_error_free(&error);
	sd_bus_message_unref(m);
	sd_bus_message_unref(reply);
	sd_bus_slot_unref(slot);
	sd_bus_unref(bus);

	return (r);
}
