/*
 * Spank plugin private-tmpdir (c) HPC2N.umu.se
 * Author: Magnus Jonsson <magnus@hpc2n.umu.se>
 * Author: Lars Viklund <lars@hpc2n.umu.se>
 * Author: Ake Sandgren <ake@hpc2n.umu.se>
 * Author: Pär Lindfors <paran@nsc.liu.se>
 */

/* Needs to be defined before first invocation of features.h so enable
 * it early. */
#define _GNU_SOURCE		/* See feature_test_macros(7) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <sys/mount.h>
#include <slurm/spank.h>
#include <unistd.h>
#include <sched.h>

SPANK_PLUGIN(private-tmpdir, 1);

// Default
static const char *base = "/tmp/slurm";
#define MAX_BIND_DIRS 16

// Globals
static int init_opts = 0;
static int binded = 0;
static char pbase[PATH_MAX + 1] = "";
static uid_t uid = (uid_t) - 1;
static gid_t gid = (gid_t) - 1;
static uint32_t jobid;
static uint32_t restartcount;

static char *bind_dirs[MAX_BIND_DIRS];
static char *bind_path[MAX_BIND_DIRS];
static int bind_dirs_count = 0;
// Globals

static int _tmpdir_bind(spank_t sp, int ac, char **av);
static int _tmpdir_init(spank_t sp, int ac, char **av);
static int _tmpdir_init_opts(spank_t sp, int ac, char **av);

/*
 *  Called from both srun and slurmd.
 */
int slurm_spank_init(spank_t sp, int ac, char **av)
{
	return _tmpdir_init_opts(sp, ac, av);
}

int slurm_spank_job_prolog(spank_t sp, int ac, char **av)
{
	int i;
	if (_tmpdir_init(sp, ac, av))
		return -1;
	if (mkdir(pbase, 0700)) {
		slurm_error("private-tmpdir: mkdir(\"%s\",0700): %m", pbase);
		return -1;
	}
	if (chown(pbase, uid, gid)) {
		slurm_error("private-tmpdir: chown(%s,%u,%u): %m", pbase, uid,
			    gid);
		return -1;
	}
	for (i = 0; i < bind_dirs_count; i++) {
		if (mkdir(bind_path[i], 0700)) {
			slurm_error("private-tmpdir: mkdir(\"%s\",0700): %m",
				    bind_path[i]);
			return -1;
		}
		if (chown(bind_path[i], uid, gid)) {
			slurm_error("private-tmpdir: chown(%s,%u,%u): %m",
				    bind_path[i], uid, gid);
			return -1;
		}
	}
	return _tmpdir_bind(sp, ac, av);
}

int slurm_spank_init_post_opt(spank_t sp, int ac, char **av)
{
	return _tmpdir_bind(sp, ac, av);
}

int slurm_spank_local_user_init(spank_t sp, int ac, char **av)
{
	return _tmpdir_bind(sp, ac, av);
}

static int _tmpdir_bind(spank_t sp, int ac, char **av)
{
	int i;

	// only on cluster nodes
	if (!spank_remote(sp))
		return 0;
	// We have done this already
	if (binded)
		return 0;
	// Don't do this anymore
	binded = 1;

	// Init dirs
	if (_tmpdir_init(sp, ac, av))
		return -1;

	// Make / share (propagate) mounts (same as mount --make-rshared /)
	if (mount("", "/", "dontcare", MS_REC | MS_SHARED, "")) {
		slurm_error
		    ("private-tmpdir: failed to 'mount --make-rshared /' for job: %u, %m",
		     jobid);
		return -1;
	}
	// Create our own namespace
	if (unshare(CLONE_NEWNS)) {
		slurm_error
		    ("private-tmpdir: failed to unshare mounts for job: %u, %m",
		     jobid);
		return -1;
	}
	// Make / slave (same as mount --make-rslave /)
	if (mount("", "/", "dontcare", MS_REC | MS_SLAVE, "")) {
		slurm_error
		    ("private-tmpdir: failed to 'mount --make-rslave /' for job: %u, %m",
		     jobid);
		return -1;
	}
	// mount --bind bind_path[i] bind_dirs[i]
	for (i = 0; i < bind_dirs_count; i++) {
		slurm_debug("private-tmpdir: mounting: %s %s", bind_path[i],
			    bind_dirs[i]);
		if (mount(bind_path[i], bind_dirs[i], "none", MS_BIND, NULL)) {
			slurm_error
			    ("private-tmpdir: failed to mount %s for job: %u, %m",
			     bind_dirs[i], jobid);
			return -1;
		}
	}
	return 0;
}

static int _tmpdir_init(spank_t sp, int ac, char **av)
{
	int n;

	// if length(pbase) > 0, we have already bin here..
	if (pbase[0] != '\0')
		return 0;

	if (_tmpdir_init_opts(sp, ac, av))
		return 0;

	// Get JobID
	if (spank_get_item(sp, S_JOB_ID, &jobid) != ESPANK_SUCCESS) {
		slurm_error("private-tmpdir: Failed to get jobid from SLURM");
		return -1;
	}
	// Get UID
	if (spank_get_item(sp, S_JOB_UID, &uid) != ESPANK_SUCCESS) {
		slurm_error("private-tmpdir: Unable to get job's user id");
		return -1;
	}
	// Get GID
	if (spank_get_item(sp, S_JOB_GID, &gid) != ESPANK_SUCCESS) {
		slurm_debug("private-tmpdir: Unable to get job's group id");
		gid = 0;
	}
	// Get Restart count
	if (spank_get_item(sp, S_SLURM_RESTART_COUNT, &restartcount) !=
	    ESPANK_SUCCESS) {
		slurm_debug
		    ("private-tmpdir: Unable to get job's restart count");
		restartcount = 0;
	}
	// Init base path
	n = snprintf(pbase, sizeof(pbase), "%s.%u.%u", base, jobid,
		     restartcount);
	if (n < 0 || n > sizeof(pbase) - 1) {
		slurm_error("private-tmpdir: \"%s.%u.%u\" too large. Aborting",
			    base, jobid, restartcount);
		return -1;
	}
	// Init bind dirs path(s)
	for (int i = 0; i < bind_dirs_count; i++) {
		bind_path[i] = malloc(strlen(pbase) + strlen(bind_dirs[i]) + 2);
		if (!bind_path[i]) {
			slurm_error
			    ("private-tmpdir: Can't malloc bind_path[i]: %m");
			return -1;
		}
		char *tmp = strdup(bind_dirs[i]);
		if (!tmp) {
			slurm_error
			    ("private-tmpdir: Can't strdup bind_dirs[i]: %m");
			return -1;
		}
		for (int j = 1; j < strlen(tmp); j++) {
			if (tmp[j] == '/') {
				tmp[j] = '_';
			}
		}
		n = snprintf(bind_path[i], PATH_MAX, "%s%s", pbase, tmp);
		if (n < 0 || n > PATH_MAX - 1) {
			slurm_error
			    ("private-tmpdir: \"%s/%s\" too large. Aborting",
			     pbase, tmp);
			free(tmp);
			return -1;
		}
		free(tmp);
	}
	return 0;
}

static int _tmpdir_init_opts(spank_t sp, int ac, char **av)
{
	int i;

	if (init_opts)
		return 0;
	init_opts = 1;

	// Init
	memset(bind_dirs, '\0', sizeof(bind_dirs));

	// for each argument in plugstack.conf
	for (i = 0; i < ac; i++) {
		if (strncmp("base=", av[i], 5) == 0) {
			const char *optarg = av[i] + 5;
			if (!strlen(optarg)) {
				slurm_error
				    ("private-tmpdir: no argument given to base= option");
				return -1;
			}
			base = strdup(optarg);
			if (!base) {
				slurm_error("private-tmpdir: can't malloc :-(");
				return -1;
			}
			continue;
		}
		if (strncmp("mount=", av[i], 6) == 0) {
			const char *optarg = av[i] + 6;
			if (bind_dirs_count == MAX_BIND_DIRS) {
				slurm_error
				    ("private-tmpdir: Reached MAX_BIND_DIRS (%d)",
				     MAX_BIND_DIRS);
				return -1;
			}
			if (optarg[0] != '/') {
				slurm_error
				    ("private-tmpdir: mount= option must start with a '/': (%s)",
				     optarg);
				return -1;
			}
			if (!strlen(optarg)) {
				slurm_error
				    ("private-tmpdir: no argument given to mount= option");
				return -1;
			}
			bind_dirs[bind_dirs_count] = strdup(optarg);
			if (!bind_dirs[bind_dirs_count]) {
				slurm_error("private-tmpdir: can't malloc :-(");
				return -1;
			}
			bind_dirs_count++;
			continue;
		}
		slurm_error("private-tmpdir: Invalid option \"%s\"", av[i]);
		return -1;
	}
	return 0;
}
