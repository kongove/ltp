#include "config.h"
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#if HAVE_NUMA_H
#include <numa.h>
#endif
#if HAVE_NUMAIF_H
#include <numaif.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "usctest.h"
#include "safe_macros.h"
#include "_private.h"
#include "mem.h"
#include "numa_helper.h"

/* OOM */

static int _alloc_mem(long int length, int testcase)
{
	void *s;

	tst_resm(TINFO, "allocating %ld bytes.", length);
	s = mmap(NULL, length, PROT_READ | PROT_WRITE,
		 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (s == MAP_FAILED) {
		if (testcase == OVERCOMMIT && errno == ENOMEM)
			return 1;
		else
			tst_brkm(TBROK | TERRNO, cleanup, "mmap");
	}
	if (testcase == MLOCK && mlock(s, length) == -1)
		tst_brkm(TINFO | TERRNO, cleanup, "mlock");
#ifdef HAVE_MADV_MERGEABLE
	if (testcase == KSM && madvise(s, length, MADV_MERGEABLE) == -1)
		tst_brkm(TBROK | TERRNO, cleanup, "madvise");
#endif
	memset(s, '\a', length);

	return 0;
}

static void _test_alloc(int testcase, int lite)
{
	if (lite)
		_alloc_mem(TESTMEM + MB, testcase);
	else
		while (1)
			if (_alloc_mem(LENGTH, testcase))
				return;
}

void oom(int testcase, int mempolicy, int lite)
{
	pid_t pid;
	int status;
#if HAVE_NUMA_H && HAVE_LINUX_MEMPOLICY_H && HAVE_NUMAIF_H \
	&& HAVE_MPOL_CONSTANTS
	unsigned long nmask = 0;
	unsigned int node;

	if (mempolicy)
		node = get_a_numa_node(cleanup);
	nmask += 1 << node;
#endif

	switch (pid = fork()) {
	case -1:
		tst_brkm(TBROK | TERRNO, cleanup, "fork");
	case 0:
#if HAVE_NUMA_H && HAVE_LINUX_MEMPOLICY_H && HAVE_NUMAIF_H \
	&& HAVE_MPOL_CONSTANTS
		if (mempolicy)
			if (set_mempolicy(MPOL_BIND, &nmask, MAXNODES) == -1)
				tst_brkm(TBROK | TERRNO, cleanup,
					 "set_mempolicy");
#endif
		_test_alloc(testcase, lite);
		exit(0);
	default:
		break;
	}
	tst_resm(TINFO, "expected victim is %d.", pid);
	if (waitpid(-1, &status, 0) == -1)
		tst_brkm(TBROK | TERRNO, cleanup, "waitpid");

	if (testcase == OVERCOMMIT) {
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			tst_resm(TFAIL, "the victim unexpectedly failed: %d",
				 status);
	} else {
		if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
			tst_resm(TFAIL, "the victim unexpectedly failed: %d",
				 status);
	}
}

void testoom(int mempolicy, int lite, int numa)
{
	long nodes[MAXNODES];

	if (numa && !mempolicy)
		write_cpusets(get_a_numa_node(cleanup));

	tst_resm(TINFO, "start normal OOM testing.");
	oom(NORMAL, mempolicy, lite);

	tst_resm(TINFO, "start OOM testing for mlocked pages.");
	oom(MLOCK, mempolicy, lite);

	if (access(PATH_KSM, F_OK) == -1)
		tst_brkm(TCONF, NULL, "KSM configuration is not enabled");

	tst_resm(TINFO, "start OOM testing for KSM pages.");
	oom(KSM, mempolicy, lite);
}

/* KSM */

static void _check(char *path, long int value)
{
	FILE *fp;
	char buf[BUFSIZ], fullpath[BUFSIZ];
	long actual_val;

	snprintf(fullpath, BUFSIZ, "%s%s", PATH_KSM, path);
	read_file(fullpath, buf);
	actual_val = SAFE_STRTOL(cleanup, buf, 0, LONG_MAX);

	tst_resm(TINFO, "%s is %ld.", path, actual_val);
	if (actual_val != value)
		tst_resm(TFAIL, "%s is not %ld.", path, value);
}

static void _wait_ksmd_done(void)
{
	char buf[BUFSIZ];
	long pages_shared, pages_sharing, pages_volatile, pages_unshared;
	long old_pages_shared = 0, old_pages_sharing = 0;
	long old_pages_volatile = 0, old_pages_unshared = 0;
	int changing = 1, count = 0;

	while (changing) {
		sleep(10);
		count++;

		read_file(PATH_KSM "pages_shared", buf);
		pages_shared = SAFE_STRTOL(cleanup, buf, 0, LONG_MAX);

		read_file(PATH_KSM "pages_sharing", buf);
		pages_sharing = SAFE_STRTOL(cleanup, buf, 0, LONG_MAX);

		read_file(PATH_KSM "pages_volatile", buf);
		pages_volatile = SAFE_STRTOL(cleanup, buf, 0, LONG_MAX);

		read_file(PATH_KSM "pages_unshared", buf);
		pages_unshared = SAFE_STRTOL(cleanup, buf, 0, LONG_MAX);

		if (pages_shared != old_pages_shared ||
		    pages_sharing != old_pages_sharing ||
		    pages_volatile != old_pages_volatile ||
		    pages_unshared != old_pages_unshared) {
			old_pages_shared = pages_shared;
			old_pages_sharing = pages_sharing;
			old_pages_volatile = pages_volatile;
			old_pages_unshared = pages_unshared;
		} else {
			changing = 0;
		}
	}

	tst_resm(TINFO, "ksm daemon takes %ds to scan all mergeable pages",
		 count * 10);
}

static void _group_check(int run, int pages_shared, int pages_sharing,
			 int pages_volatile, int pages_unshared,
			 int sleep_millisecs, int pages_to_scan)
{
	/* wait for ksm daemon to scan all mergeable pages. */
	_wait_ksmd_done();

	tst_resm(TINFO, "check!");
	_check("run", run);
	_check("pages_shared", pages_shared);
	_check("pages_sharing", pages_sharing);
	_check("pages_volatile", pages_volatile);
	_check("pages_unshared", pages_unshared);
	_check("sleep_millisecs", sleep_millisecs);
	_check("pages_to_scan", pages_to_scan);
}

static void _verify(char **memory, char value, int proc,
		    int start, int end, int start2, int end2)
{
	int i, j;
	void *s = NULL;

	s = malloc((end - start) * (end2 - start2));
	if (s == NULL)
		tst_brkm(TBROK | TERRNO, tst_exit, "malloc");

	tst_resm(TINFO, "child %d verifies memory content.", proc);
	memset(s, value, (end - start) * (end2 - start2));
	if (memcmp(memory[start], s, (end - start) * (end2 - start2))
	    != 0)
		for (j = start; j < end; j++)
			for (i = start2; i < end2; i++)
				if (memory[j][i] != value)
					tst_resm(TFAIL, "child %d has %c at "
						 "%d,%d,%d.",
						 proc, memory[j][i], proc,
						 j, i);
	free(s);
}

void write_memcg(void)
{
	char buf[BUFSIZ], mem[BUFSIZ];

	snprintf(mem, BUFSIZ, "%ld", TESTMEM);
	write_file(MEMCG_PATH_NEW "/memory.limit_in_bytes", mem);

	snprintf(buf, BUFSIZ, "%d", getpid());
	write_file(MEMCG_PATH_NEW "/tasks", buf);
}

static struct ksm_merge_data {
	char data;
	int mergeable_size;
};

static void ksm_child_memset(int child_num, int size, int total_unit,
		 struct ksm_merge_data ksm_merge_data, char **memory)
{
	int i, j;
	int unit = size / total_unit;

	tst_resm(TINFO, "child %d continues...", child_num);

	if (ksm_merge_data.mergeable_size == size * MB) {
		tst_resm(TINFO, "child %d allocates %d MB filled with '%c'",
			child_num, size, ksm_merge_data.data);

	} else {
		tst_resm(TINFO, "child %d allocates %d MB filled with '%c'"
				" except one page with 'e'",
				child_num, size, ksm_merge_data.data);
	}

	for (j = 0; j < total_unit; j++) {
		for (i = 0; i < unit * MB; i++)
			memory[j][i] = ksm_merge_data.data;
	}

	/* if it contains unshared page, then set 'e' char
	 * at the end of the last page
	 */
	if (ksm_merge_data.mergeable_size < size * MB)
		memory[j-1][i-1] = 'e';
}

static void create_ksm_child(int child_num, int size, int unit,
		       struct ksm_merge_data *ksm_merge_data)
{
	int j, total_unit;
	char **memory;

	/* The total units in all */
	total_unit = size / unit;

	/* Apply for the space for memory */
	memory = (char **)malloc(total_unit * sizeof(char *));
	for (j = 0; j < total_unit; j++) {
		memory[j] = mmap(NULL, unit * MB, PROT_READ|PROT_WRITE,
			MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
		if (memory[j] == MAP_FAILED)
			tst_brkm(TBROK|TERRNO, tst_exit, "mmap");
#ifdef HAVE_MADV_MERGEABLE
		if (madvise(memory[j], unit * MB, MADV_MERGEABLE) == -1)
			tst_brkm(TBROK|TERRNO, tst_exit, "madvise");
#endif
	}

	tst_resm(TINFO, "child %d stops.", child_num);
	if (raise(SIGSTOP) == -1)
		tst_brkm(TBROK|TERRNO, tst_exit, "kill");
	fflush(stdout);

	for (j = 0; j < 4; j++) {

		ksm_child_memset(child_num, size, total_unit,
				  ksm_merge_data[j], memory);

		fflush(stdout);

		tst_resm(TINFO, "child %d stops.", child_num);
		if (raise(SIGSTOP) == -1)
			tst_brkm(TBROK|TERRNO, tst_exit, "kill");

		if (ksm_merge_data[j].mergeable_size < size * MB) {
			_verify(memory, 'e', child_num, total_unit - 1,
				total_unit, unit * MB - 1, unit * MB);
			_verify(memory, ksm_merge_data[j].data, child_num,
				0, total_unit, 0, unit * MB - 1);
		} else {
			_verify(memory, ksm_merge_data[j].data, child_num,
				0, total_unit, 0, unit * MB);
		}
	}

	tst_resm(TINFO, "child %d finished.", child_num);
}

static void stop_ksm_children(int *child, int num)
{
	int k, status;

	tst_resm(TINFO, "wait for all children to stop.");
	for (k = 0; k < num; k++) {
		if (waitpid(child[k], &status, WUNTRACED) == -1)
			tst_brkm(TBROK|TERRNO, cleanup, "waitpid");
		if (!WIFSTOPPED(status))
			tst_brkm(TBROK, cleanup, "child %d was not stopped", k);
	}
}

static void resume_ksm_children(int *child, int num)
{
	int k, status;

	tst_resm(TINFO, "resume all children.");
	for (k = 0; k < num; k++) {
		if (kill(child[k], SIGCONT) == -1)
			tst_brkm(TBROK|TERRNO, cleanup, "kill child[%d]", k);
	}
	fflush(stdout);
}

void create_same_memory(int size, int num, int unit)
{
	char buf[BUFSIZ];
	int i, j, status, *child;
	unsigned long ps, pages;
	struct ksm_merge_data **ksm_data;

	struct ksm_merge_data ksm_data0[] = {
	       {'c', size*MB}, {'c', size*MB}, {'d', size*MB}, {'d', size*MB},
	};
	struct ksm_merge_data ksm_data1[] = {
	       {'a', size*MB}, {'b', size*MB}, {'d', size*MB}, {'d', size*MB-1},
	};
	struct ksm_merge_data ksm_data2[] = {
	       {'a', size*MB}, {'a', size*MB}, {'d', size*MB}, {'d', size*MB},
	};

	ps = sysconf(_SC_PAGE_SIZE);
	pages = MB / ps;

	ksm_data = (struct ksm_merge_data **)malloc
		   ((num - 3) * sizeof(struct ksm_merge_data *));
	/* Since from third child, the data is same with the first child's */
	for (i = 0; i < num - 3; i++) {
		ksm_data[i] = (struct ksm_merge_data *)malloc
			      (4 * sizeof(struct ksm_merge_data));
		for (j = 0; j < 4; j++) {
			ksm_data[i][j].data = ksm_data0[j].data;
			ksm_data[i][j].mergeable_size =
				ksm_data0[j].mergeable_size;
		}
	}

	child = (int *)malloc(num * sizeof(int));
	if (child == NULL)
		tst_brkm(TBROK | TERRNO, cleanup, "malloc");

	for (i = 0; i < num; i++) {
		fflush(stdout);
		switch (child[i] = fork()) {
		case -1:
			tst_brkm(TBROK|TERRNO, cleanup, "fork");
		case 0:
			if (i == 0) {
				create_ksm_child(i, size, unit, ksm_data0);
				exit(0);
			} else if (i == 1) {
				create_ksm_child(i, size, unit, ksm_data1);
				exit(0);
			} else if (i == 2) {
				create_ksm_child(i, size, unit, ksm_data2);
				exit(0);
			} else {
				create_ksm_child(i, size, unit, ksm_data[i-3]);
				exit(0);
			}
		}
	}

	stop_ksm_children(child, num);

	tst_resm(TINFO, "KSM merging...");
	write_file(PATH_KSM "run", "1");
	snprintf(buf, BUFSIZ, "%ld", size * pages * num);
	write_file(PATH_KSM "pages_to_scan", buf);
	write_file(PATH_KSM "sleep_millisecs", "0");

	resume_ksm_children(child, num);
	_group_check(1, 2, size * num * pages - 2, 0, 0, 0, size * pages * num);

	stop_ksm_children(child, num);
	resume_ksm_children(child, num);
	_group_check(1, 3, size * num * pages - 3, 0, 0, 0, size * pages * num);

	stop_ksm_children(child, num);
	resume_ksm_children(child, num);
	_group_check(1, 1, size * num * pages - 1, 0, 0, 0, size * pages * num);

	stop_ksm_children(child, num);
	resume_ksm_children(child, num);
	_group_check(1, 1, size * num * pages - 2, 0, 1, 0, size * pages * num);

	stop_ksm_children(child, num);

	tst_resm(TINFO, "KSM unmerging...");
	write_file(PATH_KSM "run", "2");

	resume_ksm_children(child, num);
	_group_check(2, 0, 0, 0, 0, 0, size * pages * num);

	tst_resm(TINFO, "stop KSM.");
	write_file(PATH_KSM "run", "0");
	_group_check(0, 0, 0, 0, 0, 0, size * pages * num);

	while (waitpid(-1, &status, WUNTRACED | WCONTINUED) > 0)
		if (WEXITSTATUS(status) != 0)
			tst_resm(TFAIL, "child exit status is %d",
				 WEXITSTATUS(status));
}

void check_ksm_options(int *size, int *num, int *unit)
{
	if (opt_size) {
		*size = atoi(opt_sizestr);
		if (*size < 1)
			tst_brkm(TBROK, cleanup, "size cannot be less than 1.");
	}
	if (opt_unit) {
		*unit = atoi(opt_unitstr);
		if (*unit > *size)
			tst_brkm(TBROK, cleanup,
				 "unit cannot be greater than size.");
		if (*size % *unit != 0)
			tst_brkm(TBROK, cleanup,
				 "the remainder of division of size by unit is "
				 "not zero.");
	}
	if (opt_num) {
		*num = atoi(opt_numstr);
		if (*num < 3)
			tst_brkm(TBROK, cleanup,
				 "process number cannot be less 3.");
	}
}

void ksm_usage(void)
{
	printf("  -n      Number of processes\n");
	printf("  -s      Memory allocation size in MB\n");
	printf("  -u      Memory allocation unit in MB\n");
}

/* cpuset/memcg */

static void _gather_cpus(char *cpus, long nd)
{
	int ncpus = 0;
	int i;
	char buf[BUFSIZ];

	while (path_exist(PATH_SYS_SYSTEM "/cpu/cpu%d", ncpus))
		ncpus++;

	for (i = 0; i < ncpus; i++)
		if (path_exist(PATH_SYS_SYSTEM "/node/node%ld/cpu%d", nd, i)) {
			sprintf(buf, "%d,", i);
			strcat(cpus, buf);
		}
	/* Remove the trailing comma. */
	cpus[strlen(cpus) - 1] = '\0';
}

void read_cpuset_files(char *prefix, char *filename, char *retbuf)
{
	int fd;
	char path[BUFSIZ];

	/*
	 * try either '/dev/cpuset/XXXX' or '/dev/cpuset/cpuset.XXXX'
	 * please see Documentation/cgroups/cpusets.txt from kernel src
	 * for details
	 */
	snprintf(path, BUFSIZ, "%s/%s", prefix, filename);
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			snprintf(path, BUFSIZ, "%s/cpuset.%s",
				 prefix, filename);
			fd = open(path, O_RDONLY);
			if (fd == -1)
				tst_brkm(TBROK | TERRNO, cleanup,
					 "open %s", path);
		} else
			tst_brkm(TBROK | TERRNO, cleanup, "open %s", path);
	}
	if (read(fd, retbuf, BUFSIZ) < 0)
		tst_brkm(TBROK | TERRNO, cleanup, "read %s", path);
	close(fd);
}

void write_cpuset_files(char *prefix, char *filename, char *buf)
{
	int fd;
	char path[BUFSIZ];

	/*
	 * try either '/dev/cpuset/XXXX' or '/dev/cpuset/cpuset.XXXX'
	 * please see Documentation/cgroups/cpusets.txt from kernel src
	 * for details
	 */
	snprintf(path, BUFSIZ, "%s/%s", prefix, filename);
	fd = open(path, O_WRONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			snprintf(path, BUFSIZ, "%s/cpuset.%s",
				 prefix, filename);
			fd = open(path, O_WRONLY);
			if (fd == -1)
				tst_brkm(TBROK | TERRNO, cleanup,
					 "open %s", path);
		} else
			tst_brkm(TBROK | TERRNO, cleanup, "open %s", path);
	}
	if (write(fd, buf, strlen(buf)) != strlen(buf))
		tst_brkm(TBROK | TERRNO, cleanup, "write %s", path);
	close(fd);
}

void write_cpusets(long nd)
{
	char buf[BUFSIZ];
	char cpus[BUFSIZ] = "";

	snprintf(buf, BUFSIZ, "%ld", nd);
	write_cpuset_files(CPATH_NEW, "mems", buf);

	_gather_cpus(cpus, nd);
	write_cpuset_files(CPATH_NEW, "cpus", cpus);

	snprintf(buf, BUFSIZ, "%d", getpid());
	write_file(CPATH_NEW "/tasks", buf);
}

void umount_mem(char *path, char *path_new)
{
	FILE *fp;
	int fd;
	char s_new[BUFSIZ], s[BUFSIZ], value[BUFSIZ];

	/* Move all processes in task to its parent node. */
	sprintf(s, "%s/tasks", path);
	fd = open(s, O_WRONLY);
	if (fd == -1)
		tst_resm(TWARN | TERRNO, "open %s", s);

	snprintf(s_new, BUFSIZ, "%s/tasks", path_new);
	fp = fopen(s_new, "r");
	if (fp == NULL)
		tst_resm(TWARN | TERRNO, "fopen %s", s_new);
	if ((fd != -1) && (fp != NULL)) {
		while (fgets(value, BUFSIZ, fp) != NULL)
			if (write(fd, value, strlen(value) - 1)
			    != strlen(value) - 1)
				tst_resm(TWARN | TERRNO, "write %s", s);
	}
	if (fd != -1)
		close(fd);
	if (fp != NULL)
		fclose(fp);
	if (rmdir(path_new) == -1)
		tst_resm(TWARN | TERRNO, "rmdir %s", path_new);
	if (umount(path) == -1)
		tst_resm(TWARN | TERRNO, "umount %s", path);
	if (rmdir(path) == -1)
		tst_resm(TWARN | TERRNO, "rmdir %s", path);
}

void mount_mem(char *name, char *fs, char *options, char *path, char *path_new)
{
	if (mkdir(path, 0777) == -1)
		tst_brkm(TBROK | TERRNO, cleanup, "mkdir %s", path);
	if (mount(name, path, fs, 0, options) == -1) {
		if (errno == ENODEV) {
			if (rmdir(path) == -1)
				tst_resm(TWARN | TERRNO, "rmdir %s failed",
					 path);
			tst_brkm(TCONF, NULL,
				 "file system %s is not configured in kernel",
				 fs);
		}
		tst_brkm(TBROK | TERRNO, cleanup, "mount %s", path);
	}
	if (mkdir(path_new, 0777) == -1)
		tst_brkm(TBROK | TERRNO, cleanup, "mkdir %s", path_new);
}

/* shared */

/* Warning: *DO NOT* use this function in child */
unsigned int get_a_numa_node(void (*cleanup_fn) (void))
{
	unsigned int nd1, nd2;
	int ret;

	ret = get_allowed_nodes(0, 2, &nd1, &nd2);
	switch (ret) {
	case 0:
		break;
	case -3:
		tst_brkm(TCONF, cleanup_fn, "requires a NUMA system.");
	default:
		tst_brkm(TBROK | TERRNO, cleanup_fn, "1st get_allowed_nodes");
	}

	ret = get_allowed_nodes(NH_MEMS | NH_CPUS, 1, &nd1);
	switch (ret) {
	case 0:
		tst_resm(TINFO, "get node%lu.", nd1);
		return nd1;
	case -3:
		tst_brkm(TCONF, cleanup_fn, "requires a NUMA system that has "
			 "at least one node with both memory and CPU "
			 "available.");
	default:
		break;
	}
	tst_brkm(TBROK | TERRNO, cleanup_fn, "2nd get_allowed_nodes");
}

int path_exist(const char *path, ...)
{
	va_list ap;
	char pathbuf[PATH_MAX];

	va_start(ap, path);
	vsnprintf(pathbuf, sizeof(pathbuf), path, ap);
	va_end(ap);

	return access(pathbuf, F_OK) == 0;
}

long read_meminfo(char *item)
{
	FILE *fp;
	char line[BUFSIZ], buf[BUFSIZ];
	long val;

	fp = fopen(PATH_MEMINFO, "r");
	if (fp == NULL)
		tst_brkm(TBROK | TERRNO, cleanup, "fopen %s", PATH_MEMINFO);

	while (fgets(line, BUFSIZ, fp) != NULL) {
		if (sscanf(line, "%64s %ld", buf, &val) == 2)
			if (strcmp(buf, item) == 0) {
				fclose(fp);
				return val;
			}
		continue;
	}
	fclose(fp);

	tst_brkm(TBROK, cleanup, "cannot find \"%s\" in %s",
		 item, PATH_MEMINFO);
}

void set_sys_tune(char *sys_file, long tune, int check)
{
	long val;
	char buf[BUFSIZ], path[BUFSIZ];

	tst_resm(TINFO, "set %s to %ld", sys_file, tune);

	snprintf(path, BUFSIZ, "%s%s", PATH_SYSVM, sys_file);
	snprintf(buf, BUFSIZ, "%ld", tune);
	write_file(path, buf);

	if (check) {
		val = get_sys_tune(sys_file);
		if (val != tune)
			tst_brkm(TBROK, cleanup, "%s = %ld, but expect %ld",
				 sys_file, val, tune);
	}
}

long get_sys_tune(char *sys_file)
{
	char buf[BUFSIZ], path[BUFSIZ];

	snprintf(path, BUFSIZ, "%s%s", PATH_SYSVM, sys_file);
	read_file(path, buf);
	return SAFE_STRTOL(cleanup, buf, LONG_MIN, LONG_MAX);
}

void write_file(char *filename, char *buf)
{
	int fd;

	fd = open(filename, O_WRONLY);
	if (fd == -1)
		tst_brkm(TBROK | TERRNO, cleanup, "open %s", filename);
	if (write(fd, buf, strlen(buf)) != strlen(buf))
		tst_brkm(TBROK | TERRNO, cleanup, "write %s", filename);
	close(fd);
}

void read_file(char *filename, char *retbuf)
{
	int fd;

	fd = open(filename, O_RDONLY);
	if (fd == -1)
		tst_brkm(TBROK | TERRNO, cleanup, "open %s", filename);
	if (read(fd, retbuf, BUFSIZ) < 0)
		tst_brkm(TBROK | TERRNO, cleanup, "read %s", filename);
	close(fd);
}

void update_shm_size(size_t * shm_size)
{
	char buf[BUFSIZ];
	size_t shmmax;

	read_file(PATH_SHMMAX, buf);
	shmmax = SAFE_STRTOUL(cleanup, buf, 0, ULONG_MAX);
	if (*shm_size > shmmax) {
		tst_resm(TINFO, "Set shm_size to shmmax: %ld", shmmax);
		*shm_size = shmmax;
	}
}
