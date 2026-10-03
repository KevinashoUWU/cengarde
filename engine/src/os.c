/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "engine.h"

int cg_timerfd(unsigned interval_ms)
{
	struct itimerspec its = {
		.it_interval = { .tv_sec = interval_ms / 1000, .tv_nsec = (long)(interval_ms % 1000) * 1000000L },
	};
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

	its.it_value = its.it_interval;
	if (fd >= 0 && timerfd_settime(fd, 0, &its, NULL) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int cg_random(void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = getrandom(p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int cg_epoll_add(int ep, int fd, uint64_t tag)
{
	struct epoll_event ev = { .events = EPOLLIN, .data.u64 = tag };

	return epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}
