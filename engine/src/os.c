/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <sched.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "engine.h"
#include "log.h"

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

uint32_t cg_tune(const struct cg_config *cfg)
{
	uint32_t busy = cfg->busy_poll_us;

	if (cfg->cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cfg->cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set) < 0)
			cg_warn("cpu %d: %s", cfg->cpu, strerror(errno));
		else
			cg_info("pinned to CPU %d", cfg->cpu);
	}
	if (cfg->rt_priority) {
		struct sched_param sp = { .sched_priority = (int)cfg->rt_priority };

		if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
			cg_warn("rt_priority %u: %s (needs root or CAP_SYS_NICE)", cfg->rt_priority, strerror(errno));
		} else {
			cg_info("real-time scheduling, priority %u", cfg->rt_priority);
			/* A real-time task that never sleeps would keep the CPU from the
			 * kernel threads that deliver its packets. */
			if (busy && (cfg->cpu >= 0 || sysconf(_SC_NPROCESSORS_ONLN) < 2)) {
				cg_warn("busy_poll_us ignored: with rt_priority it needs more than one CPU and no cpu pinning");
				busy = 0;
			}
		}
	}
	if (busy)
		cg_info("busy polling for %u us after traffic", busy);
	return busy;
}
