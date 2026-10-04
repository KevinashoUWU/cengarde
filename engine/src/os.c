/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "engine.h"
#include "log.h"
#include "status.h"

/* CPUs the process could use before the cpu knob pinned it. */
static cpu_set_t initial_cpus;
static int have_initial_cpus;
/* Taken once, by whichever thread asks first: helper threads start before
 * cg_tune pins the loop, and read the set from their own thread. */
static pthread_once_t initial_once = PTHREAD_ONCE_INIT;

static void initial_save(void)
{
	have_initial_cpus = sched_getaffinity(0, sizeof(initial_cpus), &initial_cpus) == 0;
}

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

	pthread_once(&initial_once, initial_save); /* before pinning */
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

int cg_initial_cpus(cpu_set_t *set)
{
	pthread_once(&initial_once, initial_save);
	if (have_initial_cpus) {
		*set = initial_cpus;
		return 0;
	}
	return sched_getaffinity(0, sizeof(*set), set);
}

int cg_gettid(void)
{
	return (int)syscall(SYS_gettid);
}

uint64_t cg_thread_cpu_ns(pthread_t th, int self)
{
	struct timespec ts;
	clockid_t clk = CLOCK_THREAD_CPUTIME_ID;

	if ((!self && pthread_getcpuclockid(th, &clk)) || clock_gettime(clk, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* The CPU a thread last ran on: field 39 of /proc/self/task/TID/stat (the
 * file exists on OpenWrt, unlike schedstat). -1 when unknown. */
static int task_cpu(int tid)
{
	char path[64], buf[512], *p;
	int fd, n, field = 2;

	snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	buf[n] = '\0';
	p = strrchr(buf, ')'); /* the name may hold spaces */
	if (!p)
		return -1;
	for (p++; *p && field < 39; p++)
		if (*p == ' ')
			field++;
	return field == 39 ? atoi(p) : -1;
}

void cg_threads_head(struct cg_json *j)
{
	cg_json_raw(j, "%-15s %7s %4s %10s %6s\n", "THREAD", "TID", "CPU", "CPU_MS", "%CPU");
}

void cg_threads_row(struct cg_json *j, const char *name, int tid, uint64_t cpu_ns, int permille)
{
	char cpu[16] = "-", pct[16] = "-";
	int last = tid > 0 ? task_cpu(tid) : -1;

	if (last >= 0)
		snprintf(cpu, sizeof(cpu), "%d", last);
	if (permille >= 0)
		snprintf(pct, sizeof(pct), "%d.%d", permille / 10, permille % 10);
	cg_json_raw(j, "%-15s %7d %4s %10llu %6s\n", name, tid, cpu, (unsigned long long)(cpu_ns / 1000000), pct);
}

void cg_thread_normal(void)
{
	struct sched_param sp = { .sched_priority = 0 };

	pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
	pthread_once(&initial_once, initial_save);
	if (have_initial_cpus)
		sched_setaffinity(0, sizeof(initial_cpus), &initial_cpus);
}

void cg_log_warnings(const char *path, char *warn)
{
	char *save = NULL;

	for (char *line = strtok_r(warn, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
		cg_warn("%s: %s", path, line);
}

void cg_reexec(char **argv)
{
	/* Scheduling and CPU affinity survive exec: start from what the
	 * process had, so that the new configuration decides them. */
	cg_thread_normal();
	execv("/proc/self/exe", argv);
	execvp(argv[0], argv);
}

/* ---- configuration loader ---- */

static void *loader_main(void *arg)
{
	struct cg_loader *l = arg;
	uint64_t one = 1;

	pthread_setname_np(pthread_self(), "cg-load");
	cg_thread_normal();
	l->cfg = calloc(1, sizeof(*l->cfg));
	if (!l->cfg) {
		snprintf(l->err, sizeof(l->err), "out of memory");
	} else if (cg_config_load(l->cfg, l->path, l->err, sizeof(l->err), l->warn, sizeof(l->warn)) < 0) {
		free(l->cfg);
		l->cfg = NULL;
	}
	while (write(l->efd, &one, sizeof(one)) < 0 && errno == EINTR)
		;
	return NULL;
}

int cg_loader_init(struct cg_loader *l)
{
	memset(l, 0, sizeof(*l));
	l->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	return l->efd < 0 ? -1 : 0;
}

int cg_loader_start(struct cg_loader *l, const char *path)
{
	if (l->busy)
		return -1;
	l->busy = 1;
	l->path = path;
	l->cfg = NULL;
	l->err[0] = l->warn[0] = '\0';
	l->joinable = !pthread_create(&l->thread, NULL, loader_main, l);
	if (!l->joinable)
		loader_main(l); /* no thread to be had: load here, once */
	return 0;
}

int cg_loader_done(struct cg_loader *l, struct cg_config **cfg)
{
	uint64_t n;

	*cfg = NULL;
	if (read(l->efd, &n, sizeof(n)) != (ssize_t)sizeof(n) || !l->busy)
		return 0;
	if (l->joinable)
		pthread_join(l->thread, NULL);
	l->busy = l->joinable = 0;
	*cfg = l->cfg;
	l->cfg = NULL;
	return 1;
}

void cg_loader_free(struct cg_loader *l)
{
	if (l->busy && l->joinable)
		pthread_join(l->thread, NULL);
	l->busy = l->joinable = 0;
	if (l->cfg) {
		cg_config_free(l->cfg);
		free(l->cfg);
		l->cfg = NULL;
	}
	if (l->efd >= 0)
		close(l->efd);
	l->efd = -1;
}
