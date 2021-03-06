#include "integrate.h"
#include "signal_except.h"

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <dirent.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>

struct task_container {
	long double base;
	long double step_wdth;
	long double accum;

	size_t start_step;
	size_t n_steps;

	int cpu;
};

/* Aligned task_container to avoid cache bouncing */
struct task_container_align {
	struct task_container task;
	uint8_t padding[CACHE_LINE_ALIGN - sizeof(struct task_container)];
};

void *integrate_task_worker(void *arg)
{
	struct task_container *pack = arg;
	register worker_tmp_t base = pack->base;
	register worker_tmp_t step_wdth = pack->step_wdth;
	size_t n_steps = pack->n_steps;
	register size_t cur_step = pack->start_step;
	register worker_tmp_t sum = 0;

	DUMP_LOG_DO(worker_tmp_t dump_from = base + cur_step * step_wdth);
	DUMP_LOG_DO(worker_tmp_t dump_to =
			    base + (cur_step + pack->n_steps) * step_wdth);

	for (register size_t i = n_steps; i != 0; i--, cur_step++) {
		register worker_tmp_t x = base + cur_step * step_wdth;
		sum += INTEGRATE_FUNC(x) * step_wdth;
	}

	pack->accum = sum;

	DUMP_LOG("worker: from: %lg "
		 "to: %lg sum: %lg arg: %p\n",
		 (double)dump_from, (double)dump_to, (double)sum, arg);

	return NULL;
}

void integrate_split_tasks(struct task_container_align *tasks, int n_tasks,
			   cpu_set_t *cpuset, size_t n_steps, long double base,
			   long double step)
{
	int n_cpus = CPU_COUNT(cpuset);
	if (n_tasks < n_cpus)
		n_cpus = n_tasks;

	size_t cur_step = 0;
	int cur_task = 0;

	int cpu = cpu_set_search_next(-1, cpuset);
	for (; n_cpus != 0; n_cpus--, cpu = cpu_set_search_next(cpu, cpuset)) {
		/* Take ~1/n steps and threads per one cpu */
		size_t cpu_steps = n_steps / n_cpus;
		int cpu_tasks = n_tasks / n_cpus;
		n_steps -= cpu_steps;
		n_tasks -= cpu_tasks;

		for (; cpu_tasks != 0; cpu_tasks--, cur_task++) {
			struct task_container *ptr = &tasks[cur_task].task;
			ptr->base = base;
			ptr->step_wdth = step;
			ptr->cpu = cpu;

			size_t task_steps = cpu_steps / cpu_tasks;

			ptr->start_step = cur_step;
			ptr->n_steps = task_steps;

			cur_step += task_steps;
			cpu_steps -= task_steps;
		}
	}
}

int set_this_thread_cpu(int cpu)
{
	cpu_set_t set_tmp;
	CPU_ZERO(&set_tmp);
	CPU_SET(cpu, &set_tmp);
	DUMP_LOG("setting main   to cpu = %2d\n", cpu);
	if (sched_setaffinity(getpid(), sizeof(set_tmp), &set_tmp) == -1) {
		perror("Error: sched_setaffinity");
		return -1;
	}
	return 0;
}

int integrate_run_tasks(struct task_container_align *tasks, pthread_t *threads,
			int n_tasks)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	cpu_set_t cpuset_tmp;
	for (int i = 0; i < n_tasks; i++) {
		struct task_container *ptr = &tasks[i].task;
		CPU_ZERO(&cpuset_tmp);
		CPU_SET(ptr->cpu, &cpuset_tmp);
		DUMP_LOG("setting worker to cpu = %2d\n", ptr->cpu);
		int ret = pthread_attr_setaffinity_np(&attr, sizeof(cpuset_tmp),
						      &cpuset_tmp);
		if (ret) {
			perror("Error: pthread_attr_setaffinity_np");
			return -1;
		}
		ret = pthread_create(&threads[i], &attr, integrate_task_worker,
				     ptr);
		if (ret) {
			perror("Error: pthread_create");
			return -1;
		}
	}
	pthread_attr_destroy(&attr);
	return 0;
}

int integrate_join_tasks(pthread_t *threads, int n_threads)
{
	for (; n_threads != 0; n_threads--, threads++) {
		if (pthread_join(*threads, NULL)) {
			perror("Error: pthread_join");
			return -1;
		}
		*threads = 0;
	}
	return 0;
}

/* Error-cleanup func, no err handler */
int integrate_cancel_tasks(pthread_t *threads, int n_threads)
{
	for (; n_threads != 0; n_threads--, threads++) {
		if (*threads)
			pthread_cancel(*threads);
	}

	return 0;
}

long double integrate_accumulate_result(struct task_container_align *tasks,
					int n_tasks)
{
	long double accum = 0;
	for (int i = 0; i < n_tasks; i++) {
		accum += tasks[i].task.accum;
	}
	return accum;
}

/* Get unused cpuset */
void integrate_tasks_unused_cpus(struct task_container_align *tasks,
				 int n_tasks, cpu_set_t *cpuset,
				 cpu_set_t *result)
{
	CPU_ZERO(result);
	for (int i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, cpuset))
			CPU_SET(i, result);
	}
	for (int i = 0; i < n_tasks; i++)
		CPU_CLR(tasks[i].task.cpu, result);
}

/* Good version, but unnecessary for my task */
/* For real usage please replace _scalable with this function */
int integrate_multicore(cpu_set_t *cpuset, size_t n_steps, long double base,
			long double step, long double *result)
{
	if (setjmp(sig_exc_buf)) {
		fprintf(stderr, "Error: signal-exception caught\n");
		goto handle_err;
	}

	int n_threads = CPU_COUNT(cpuset);

	/* Allocate cache-aligned task containers */
	struct task_container_align *tasks =
		aligned_alloc(sizeof(*tasks), sizeof(*tasks) * n_threads);
	if (!tasks) {
		perror("Error: aligned_alloc");
		goto handle_err;
	}

	pthread_t *threads = malloc(sizeof(*threads) * n_threads);
	if (!threads) {
		perror("Error: malloc");
		goto handle_err;
	}

	/* Split task btw cpus and threads */
	integrate_split_tasks(tasks, n_threads, cpuset, n_steps, base, step);

	/* Move main thread to other cpu */
	if (set_this_thread_cpu(tasks[0].task.cpu))
		goto handle_err;

	/* Run non-main tasks */
	if (integrate_run_tasks(tasks + 1, threads + 1, n_threads - 1))
		goto handle_err;

	/* Run main task */
	integrate_task_worker(&tasks[0].task);

	/* Finish non-main tasks */
	if (integrate_join_tasks(threads + 1, n_threads - 1))
		goto handle_err;

	/* Sumary */
	*result = integrate_accumulate_result(tasks, n_threads);

	free(tasks);
	return 0;

handle_err:
	if (threads) {
		integrate_cancel_tasks(threads + 1, n_threads - 1);
		free(threads);
	}

	if (tasks)
		free(tasks);
	return -1;
}

/* Time-scalability with TurboBoost requires this function with trash-threads */
int integrate_multicore_scalable(int n_threads, cpu_set_t *cpuset,
				 size_t n_steps, long double base,
				 long double step, long double *result)
{
	if (setjmp(sig_exc_buf)) {
		fprintf(stderr, "Error: signal-exception caught\n");
		goto handle_err;
	}

	/* Allocate cache-aligned task containers */
	struct task_container_align *tasks = NULL;
	tasks = aligned_alloc(sizeof(*tasks), sizeof(*tasks) * n_threads);
	if (!tasks) {
		perror("Error: aligned_alloc");
		goto handle_err;
	}

	/* The same with overloading threads */
	int n_bad_threads = 0;
	struct task_container_align *bad_tasks = NULL;
	if (CPU_COUNT(cpuset) > n_threads) {
		n_bad_threads = CPU_COUNT(cpuset) - n_threads;
		bad_tasks = aligned_alloc(sizeof(*bad_tasks),
					  sizeof(*bad_tasks) * n_bad_threads);
		if (!bad_tasks) {
			perror("Error: aligned_alloc");
			goto handle_err;
		}
	}

	pthread_t *threads = NULL;
	threads = calloc(sizeof(*threads), n_threads);
	if (!threads) {
		perror("Error: malloc");
		goto handle_err;
	}

	pthread_t *bad_threads = NULL;
	if (n_bad_threads) {
		bad_threads = calloc(sizeof(*bad_threads), n_bad_threads);
		if (!bad_threads) {
			perror("Error: malloc");
			goto handle_err;
		}
	}

	/* Split task btw cpus and threads */
	integrate_split_tasks(tasks, n_threads, cpuset, n_steps, base, step);

	/* Split bad tasks */
	if (n_bad_threads) {
		cpu_set_t bad_cpuset;
		integrate_tasks_unused_cpus(tasks, n_threads, cpuset,
					    &bad_cpuset);
		size_t n_bad_steps = (n_steps / n_threads) * n_bad_threads;
		integrate_split_tasks(bad_tasks, n_bad_threads, &bad_cpuset,
				      n_bad_steps, base, step);
	}

	/* Move main thread to other cpu */
	if (set_this_thread_cpu(tasks[0].task.cpu))
		goto handle_err;

	/* Run bad tasks */
	if (n_bad_threads &&
	    integrate_run_tasks(bad_tasks, bad_threads, n_bad_threads))
		goto handle_err;

	/* Run non-main tasks */
	if (integrate_run_tasks(tasks + 1, threads + 1, n_threads - 1))
		goto handle_err;

	/* Run main task */
	integrate_task_worker(&tasks[0].task);

	/* Finish bad tasks */
	if (n_bad_threads && integrate_join_tasks(bad_threads, n_bad_threads))
		goto handle_err;

	/* Finish non-main tasks */
	if (integrate_join_tasks(threads + 1, n_threads - 1))
		goto handle_err;

	/* Sumary */
	*result = integrate_accumulate_result(tasks, n_threads);

	if (n_bad_threads) {
		free(bad_tasks);
		free(bad_threads);
	}
	free(tasks);
	free(threads);
	return 0;

handle_err:
	if (bad_threads) {
		integrate_cancel_tasks(bad_threads, n_bad_threads);
		free(bad_threads);
	}
	if (threads) {
		integrate_cancel_tasks(threads + 1, n_threads - 1);
		free(threads);
	}
	if (bad_tasks)
		free(bad_tasks);
	if (tasks)
		free(tasks);
	return -1;
}
