/* The control socket (ctl.h): accepting, reading the command line and
 * sending the reply never block the event loop.
 *
 * SPDX-License-Identifier: GPL-2.0-only */
#include "ctl.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "engine.h"
#include "util.h"

static int addr_of(const char *path, struct sockaddr_un *sa)
{
	memset(sa, 0, sizeof(*sa));
	sa->sun_family = AF_UNIX;
	if (!*path || strlen(path) >= sizeof(sa->sun_path))
		return -1;
	strcpy(sa->sun_path, path);
	return 0;
}

void cg_ctl_init(struct cg_ctl *ctl)
{
	memset(ctl, 0, sizeof(*ctl));
	ctl->fd = -1;
	for (int i = 0; i < CG_CTL_CONNS; i++)
		ctl->c[i].fd = -1;
}

static void conn_close(struct cg_ctl *ctl, int ep, int i)
{
	struct cg_ctl_conn *k = &ctl->c[i];

	if (k->fd < 0)
		return;
	epoll_ctl(ep, EPOLL_CTL_DEL, k->fd, NULL); /* fails harmlessly while it waits out of the set */
	close(k->fd);
	free(k->out);
	memset(k, 0, sizeof(*k));
	k->fd = -1;
}

/* Removes path when it is a socket nobody listens on any more. */
static int clear_stale(const char *path, const struct sockaddr_un *sa, char *err, size_t errlen)
{
	struct stat st;
	int fd, e;

	if (lstat(path, &st) < 0) {
		if (errno == ENOENT)
			return 0;
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		return -1;
	}
	if (!S_ISSOCK(st.st_mode)) {
		snprintf(err, errlen, "%s exists and is not a socket", path);
		return -1;
	}
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	e = connect(fd, (const struct sockaddr *)sa, sizeof(*sa)) == 0 ? 0 : errno;
	close(fd);
	if (e == 0 || e == EAGAIN) {
		snprintf(err, errlen, "%s is in use by another cengarde", path);
		return -1;
	}
	if (e != ECONNREFUSED) {
		snprintf(err, errlen, "%s: %s", path, strerror(e));
		return -1;
	}
	unlink(path);
	return 0;
}

static int bind_owner_only(int fd, const struct sockaddr_un *sa)
{
	mode_t old = umask(077);
	int rc = bind(fd, (const struct sockaddr *)sa, sizeof(*sa));

	if (rc < 0 && errno == ENOENT) {
		char dir[sizeof(sa->sun_path)], *slash;

		strcpy(dir, sa->sun_path);
		slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = '\0';
			if (mkdir(dir, 0700) == 0 || errno == EEXIST)
				rc = bind(fd, (const struct sockaddr *)sa, sizeof(*sa));
			else
				errno = ENOENT;
		}
	}
	umask(old);
	return rc;
}

int cg_ctl_open(struct cg_ctl *ctl, const char *path, int ep, char *err, size_t errlen)
{
	struct sockaddr_un sa;
	int fd;

	cg_ctl_init(ctl);
	if (addr_of(path, &sa) < 0) {
		snprintf(err, errlen, "%s: path too long", path);
		return -1;
	}
	if (clear_stale(path, &sa, err, errlen) < 0)
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	if (bind_owner_only(fd, &sa) < 0 || listen(fd, CG_CTL_CONNS) < 0) {
		snprintf(err, errlen, "%s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	if (cg_epoll_add(ep, fd, CG_EV(CG_EV_CTL, CG_CTL_CONNS)) < 0) {
		snprintf(err, errlen, "epoll: %s", strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	ctl->fd = fd;
	strcpy(ctl->path, path);
	return 0;
}

void cg_ctl_close(struct cg_ctl *ctl, int ep)
{
	for (int i = 0; i < CG_CTL_CONNS; i++)
		conn_close(ctl, ep, i);
	if (ctl->fd < 0)
		return;
	epoll_ctl(ep, EPOLL_CTL_DEL, ctl->fd, NULL);
	close(ctl->fd);
	unlink(ctl->path);
	ctl->fd = -1;
}

static void accept_all(struct cg_ctl *ctl, int ep, uint64_t now_ms)
{
	for (;;) {
		int fd = accept4(ctl->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC), i = 0;

		if (fd < 0)
			return;
		while (i < CG_CTL_CONNS && ctl->c[i].fd >= 0)
			i++;
		/* Busy: the caller sees the connection closed without a reply. */
		if (i == CG_CTL_CONNS || cg_epoll_add(ep, fd, CG_EV(CG_EV_CTL, i)) < 0) {
			close(fd);
			continue;
		}
		ctl->c[i].fd = fd;
		ctl->c[i].deadline_ms = now_ms + CG_CTL_TIMEOUT_MS;
	}
}

/* Returns i once the command line is complete, else -1. */
static int conn_read(struct cg_ctl *ctl, int ep, int i)
{
	struct cg_ctl_conn *k = &ctl->c[i];
	ssize_t n = recv(k->fd, k->in + k->inlen, sizeof(k->in) - 1 - k->inlen, MSG_DONTWAIT);
	char *nl;

	if (n < 0) {
		if (errno != EAGAIN && errno != EINTR)
			conn_close(ctl, ep, i);
		return -1;
	}
	if (n == 0 && !k->inlen) {
		conn_close(ctl, ep, i);
		return -1;
	}
	k->inlen += (size_t)n;
	k->in[k->inlen] = '\0';
	nl = memchr(k->in, '\n', k->inlen);
	if (nl)
		*nl = '\0';
	else if (n > 0 && k->inlen < sizeof(k->in) - 1)
		return -1; /* more to come */
	/* Complete (a newline, the end of input or a full buffer): out of the
	 * set until the reply, which also ignores a half-closed caller. */
	epoll_ctl(ep, EPOLL_CTL_DEL, k->fd, NULL);
	return i;
}

static void conn_write(struct cg_ctl *ctl, int ep, int i)
{
	struct cg_ctl_conn *k = &ctl->c[i];

	while (k->outoff < k->outlen) {
		ssize_t n = send(k->fd, k->out + k->outoff, k->outlen - k->outoff, MSG_DONTWAIT | MSG_NOSIGNAL);

		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
			break; /* the caller went away */
		}
		k->outoff += (size_t)n;
	}
	conn_close(ctl, ep, i);
}

int cg_ctl_event(struct cg_ctl *ctl, int ep, uint32_t idx, uint64_t now_ms)
{
	if (idx == CG_CTL_CONNS) {
		if (ctl->fd >= 0)
			accept_all(ctl, ep, now_ms);
		return -1;
	}
	if (idx >= CG_CTL_CONNS || ctl->c[idx].fd < 0)
		return -1;
	if (ctl->c[idx].out) {
		conn_write(ctl, ep, (int)idx);
		return -1;
	}
	return conn_read(ctl, ep, (int)idx);
}

void cg_ctl_reply(struct cg_ctl *ctl, int ep, int i, char *text, size_t len)
{
	struct cg_ctl_conn *k;
	struct epoll_event ev = { .events = EPOLLOUT, .data.u64 = CG_EV(CG_EV_CTL, i) };

	if (i < 0 || i >= CG_CTL_CONNS || ctl->c[i].fd < 0 || ctl->c[i].out) {
		free(text);
		return;
	}
	k = &ctl->c[i];
	if (!text) {
		conn_close(ctl, ep, i);
		return;
	}
	k->out = text;
	k->outlen = len;
	k->outoff = 0;
	k->waiting = 0;
	conn_write(ctl, ep, i);
	if (k->fd >= 0 && epoll_ctl(ep, EPOLL_CTL_ADD, k->fd, &ev) < 0)
		conn_close(ctl, ep, i);
}

void cg_ctl_reply_waiting(struct cg_ctl *ctl, int ep, const char *msg)
{
	for (int i = 0; i < CG_CTL_CONNS; i++) {
		char *t;

		if (ctl->c[i].fd < 0 || !ctl->c[i].waiting)
			continue;
		t = strdup(msg);
		cg_ctl_reply(ctl, ep, i, t, t ? strlen(t) : 0);
	}
}

void cg_ctl_expire(struct cg_ctl *ctl, int ep, uint64_t now_ms)
{
	for (int i = 0; i < CG_CTL_CONNS; i++)
		if (ctl->c[i].fd >= 0 && now_ms >= ctl->c[i].deadline_ms)
			conn_close(ctl, ep, i);
}

int cg_ctl_request(const char *path, const char *line, int timeout_ms, char **reply, size_t *len, char *err,
		   size_t errlen)
{
	struct sockaddr_un sa;
	uint64_t deadline = cg_now_ms() + (uint64_t)timeout_ms;
	char cmd[CG_CTL_LINE + 1], *buf = NULL;
	size_t n = 0, cap = 0, cmdlen = strlen(line);
	int fd, rc = -1;

	*reply = NULL;
	*len = 0;
	if (addr_of(path, &sa) < 0 || cmdlen >= CG_CTL_LINE) {
		snprintf(err, errlen, "%s: path or command too long", path);
		return -1;
	}
	memcpy(cmd, line, cmdlen);
	cmd[cmdlen++] = '\n';
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		snprintf(err, errlen, "socket: %s", strerror(errno));
		return -1;
	}
	if (connect(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int e = errno;

		snprintf(err, errlen, "%s: %s%s", path, strerror(e),
			 e == ENOENT || e == ECONNREFUSED ? " (is cengarde running, with control_socket set to it?)" : "");
		goto out;
	}
	/* A short line on a fresh connection: it fits the socket buffer. */
	if (send(fd, cmd, cmdlen, MSG_NOSIGNAL) != (ssize_t)cmdlen) {
		snprintf(err, errlen, "send: %s", strerror(errno));
		goto out;
	}
	for (;;) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		uint64_t now = cg_now_ms();
		ssize_t got;
		int pr;

		if (now >= deadline) {
			snprintf(err, errlen, "no reply within %d s", timeout_ms / 1000);
			goto out;
		}
		pr = poll(&p, 1, (int)(deadline - now));
		if (pr < 0 && errno != EINTR) {
			snprintf(err, errlen, "poll: %s", strerror(errno));
			goto out;
		}
		if (pr <= 0)
			continue;
		if (cap - n < 4096) {
			char *nb = realloc(buf, cap ? cap * 2 : 65536);

			if (!nb) {
				snprintf(err, errlen, "out of memory");
				goto out;
			}
			buf = nb;
			cap = cap ? cap * 2 : 65536;
		}
		got = recv(fd, buf + n, cap - n - 1, 0);
		if (got < 0) {
			if (errno == EINTR)
				continue;
			snprintf(err, errlen, "recv: %s", strerror(errno));
			goto out;
		}
		if (!got)
			break;
		n += (size_t)got;
	}
	if (!n) {
		snprintf(err, errlen, "no reply (busy, or restarting)");
		goto out;
	}
	buf[n] = '\0';
	*reply = buf;
	*len = n;
	buf = NULL;
	rc = 0;
out:
	free(buf);
	close(fd);
	return rc;
}
