#include "cpu_topology.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

int file_read_num(const char *name, int *result)
{
	char buf[64];

	int fd = open(name, O_RDONLY);
	if (fd == -1) {
		perror("Error: file_read_num: open");
		return -1;
	}

	ssize_t ret = read(fd, buf, sizeof(buf) - 1);
	if (ret == -1) {
		perror("Error: file_read_num: read");
		return -1;
	}

	if (close(fd) == -1) {
		perror("Error: file_read_num: close");
		return -1;
	}

	buf[ret] = '\0';
	errno = 0;
	long tmp = strtol(buf, NULL, 10);
	if (errno || tmp < INT_MIN || tmp > INT_MAX) {
		fprintf(stderr, "Error: file_read_num: wrong number\n");
		return -1;
	}

	*result = tmp;
	return 0;
}

int get_cpu_topology(struct cpu_topology *topo)
{
	DIR *sysfs_cpudir = opendir("/sys/bus/cpu/devices");
	if (!sysfs_cpudir) {
		perror("Error: get_cpu_topology: "
		       "opendir /sys/bus/cpu/devices");
		return -1;
	}

	struct dirent *entry;
	char buf[512];
	int n_cpu = 0;
	topo->max_package_id = 0;
	topo->max_core_id = 0;
	topo->max_cpu_id = 0;
	errno = 0;

	while ((entry = readdir(sysfs_cpudir)) != NULL) {
		if (!memcmp(entry->d_name, "cpu", 3)) {
			int cpu_id, core_id, package_id;

			cpu_id = atoi(entry->d_name + 3);

			sprintf(buf,
				"/sys/bus/cpu/devices/%s/topology/"
				"core_id",
				entry->d_name);
			if (file_read_num(buf, &core_id)) {
				fprintf(stderr, "Error: get_cpu_topology: "
						"core_id read failed\n");
				goto handle_err;
			}

			sprintf(buf,
				"/sys/bus/cpu/devices/%s/topology/"
				"physical_package_id",
				entry->d_name);
			if (file_read_num(buf, &package_id)) {
				fprintf(stderr, "Error: get_cpu_topology: "
						"package_id read failed\n");
				goto handle_err;
			}

			topo->cpu[n_cpu].package_id = package_id;
			topo->cpu[n_cpu].core_id = core_id;
			topo->cpu[n_cpu].cpu_id = cpu_id;
			n_cpu++;

			if (package_id > topo->max_package_id)
				topo->max_package_id = package_id;
			if (core_id > topo->max_core_id)
				topo->max_core_id = core_id;
			if (cpu_id > topo->max_cpu_id)
				topo->max_cpu_id = cpu_id;
		}
	}

	if (errno) {
		perror("Error: get_cpu_topology: readdir");
		goto handle_err;
	}

	closedir(sysfs_cpudir);
	return 0;

handle_err:
	closedir(sysfs_cpudir);
	return -1;
}

int dump_cpu_topology(FILE *stream, struct cpu_topology *topo)
{
	fprintf(stream, "--- dump_cpu_topology: ---\n");

	fprintf(stream, "max_package_id: %3.3d\n", topo->max_package_id);
	fprintf(stream, "max_core_id:    %3.3d\n", topo->max_core_id);
	fprintf(stream, "max_cpu_id:     %3.3d\n", topo->max_cpu_id);

	for (int i = 0; i < topo->max_cpu_id + 1; i++) {
		fprintf(stream, "cpu[%d]: ", i);
		fprintf(stream, ".package_id: %3.3d ", topo->cpu[i].package_id);
		fprintf(stream, ".core_id: %3.3d ", topo->cpu[i].core_id);
		fprintf(stream, ".cpu_id: %3.3d\n", topo->cpu[i].cpu_id);
	}

	fprintf(stream, "--- /dump_cpu_topology ---\n");
	return 0;
}

int one_cpu_per_core_cpu_topology(struct cpu_topology *topo, cpu_set_t *cpuset)
{
	int n_packages = topo->max_package_id + 1;
	int n_cores = topo->max_core_id + 1;
	int n_cpus = topo->max_cpu_id + 1;
	int assoc_cpu_s = n_packages * n_cores;

	/* assoc_cpu[n_packages][n_cores] */
	int *assoc_cpu = malloc(sizeof(int) * assoc_cpu_s);
	if (!assoc_cpu) {
		perror("Error: one_cpu_per_core_cpu_topology: malloc");
		return -1;
	}

	for (int i = 0; i < assoc_cpu_s; i++)
		assoc_cpu[i] = -1;

	for (int i = 0; i < n_cpus; i++) {
		assoc_cpu[topo->cpu[i].package_id * n_cores +
			  topo->cpu[i].core_id] = topo->cpu[i].cpu_id;
	}

	CPU_ZERO(cpuset);
	for (int i = 0; i < assoc_cpu_s; i++) {
		if (assoc_cpu[i] != -1)
			CPU_SET(assoc_cpu[i], cpuset);
	}

	free(assoc_cpu);
	return 0;
}

int dump_cpu_set(FILE *stream, cpu_set_t *cpuset)
{
	fprintf(stream, "--- dump_cpu_set: ---\n");

	for (int i = 0; i < CPU_SETSIZE - 1; i++) {
		if (CPU_ISSET(i, cpuset))
			fprintf(stream, "cpu_id: %3.3d is set\n", i);
	}

	fprintf(stream, "--- /dump_cpu_set ---\n");

	return 0;
}

int cpu_set_search_next(int cpu, cpu_set_t *set)
{
	for (int i = cpu + 1; i < CPU_SETSIZE - 1; i++) {
		if (CPU_ISSET(i, set))
			return i;
	}
	return 0;
}

void get_full_cpuset(struct cpu_topology *topo, cpu_set_t *set)
{
	CPU_ZERO(set);
	for (int i = 0; i < topo->max_cpu_id + 1; i++)
		CPU_SET(topo->cpu[i].cpu_id, set);
}
