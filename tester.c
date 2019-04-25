#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE  201509L
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define FNMAX 256
#define PROCMAX 256
#define FDMAX 256

#define E(s) perror(s), exit(1)
#define RAND(min, max) (min + (rand()%(max-min)))

int opt_seed;
int opt_nproc;
int opt_nfd;

struct wfile {
	int fd;
	int flags;
	char fn[FNMAX];
	bool opened;
};


void usage (void)
{
	printf("Usage: ./tester SEED NPROC NFD MNTPATH...\n");
	exit(0);
}

// hashes an integer (seed) to a unique filename
void filename (int seed, char *buf)
{
	const char sym[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_=";
	do {
		int r = seed % 64;
		*buf++ = sym[r];
		seed /= 64;

	} while (seed > 0);

	*buf = 0;
}

int worker (void)
{
	int i, j;
	struct wfile files[FDMAX] = {0};
	pid_t pid = getpid();
	struct timespec req = {0};
	struct flock flock;
	int rc;

	for (j = 0; j < 100; j++) {
		for (i = 0; i < opt_nfd; i++) {
			bool to_read  = i % 2;
			bool lockwait = j % 2;
			struct wfile *f = files + i;
			bool do_unlock = true;

			filename(i, f->fn);
			f->flags = O_CREAT | (to_read ? O_RDONLY : O_WRONLY);
			//printf("pid %d open(%s, %s)\n", pid, f->fn, to_read ? "READ":"WRITE");
			f->fd = open(f->fn, f->flags, 0666);
			if (f->fd < 0) {
				f->opened = false;
				perror("open fail");
				errno = 0;
			} else {
				f->opened = true;
			}

			req.tv_nsec = 1000000L * RAND(100,1000);
			nanosleep(&req, NULL);

			if (!f->opened || to_read)
				continue;

			flock = (struct flock) {
				.l_type = F_WRLCK,
				.l_whence = SEEK_SET,
				.l_start = 0,
				.l_len = 0, // whole file
			};


			rc = fcntl(f->fd, lockwait ? F_OFD_SETLKW : F_OFD_SETLK, &flock);
			if (rc < 0) {
				perror("fcntl lock");
				do_unlock = false;
			}

			const char buf[] = "abc";
			ssize_t r = write(f->fd, buf, sizeof(buf));
			if (r < 0) {
				perror("write fail");
			}

			flock = (struct flock) {
				.l_type = F_UNLCK,
				.l_whence = SEEK_SET,
				.l_start = 0,
				.l_len = 0, // whole file
			};

			if (false && do_unlock) {
				rc = fcntl(f->fd, F_OFD_SETLK, &flock);
				if (rc < 0) {
					perror("fcntl unlock");
				}
			}
		}

		for (i = 0; i < opt_nfd; i++) {
			int rc;
			struct wfile *f = files + i;

			if (f->opened) {
				rc = close(f->fd);
				if (rc < 0)
					perror("close");
			}
		}
	}

	return 0;
}


bool is_writeable_dir (const char *path)
{
	struct stat st;
	int rc;

	rc = stat(path, &st);
	if (rc < 0) {
		perror("stat");
		return false;
	}

	if (!S_ISDIR(st.st_mode))
		return false;

	rc = access(path, W_OK);
	if (rc < 0) {
		perror("access");
		return false;
	}

	return true;
}

int main (int argc, char **argv)
{
	int i, j;
	const char *dir;
	pid_t pid;
	int rc;
	pid_t workers[PROCMAX];
	int nworkers;
	int nwait;

	int min_args = 4;

	if (argc <= min_args)
		usage();

	opt_seed = atoi(argv[1]);
	opt_nproc = atoi(argv[2]);
	opt_nfd = atoi(argv[3]);

	// check all dirs
	for (i = min_args; i < argc; i++) {
		dir = argv[i];
		if (!is_writeable_dir(dir)) {
			printf("can write to dir %s\n", dir);
			return 1;
		}
	}

	printf("seed = %d, nproc = %d\n", opt_seed, opt_nproc);

	srand(opt_seed);

	// start all workers
	nworkers = 0;
	for (i = min_args; i < argc; i++) {
		dir = argv[i];
		for (j = 0; j < opt_nproc; j++) {
			pid = fork();
			if (pid < 0)
				E("fork");
			if (pid == 0) {
				rc = chdir(dir);
				if (rc < 0)
					E("chdir");
				return worker();
			}
			//printf("started worker %d\n", pid);
			workers[nworkers++] = pid;
		}
	}

	nwait = nworkers;

	// wait for all workers
	while (nwait > 0) {
		siginfo_t si;
		printf("%d workers left...\n", nwait);
		rc = waitid(P_ALL, 0, &si, WEXITED|WSTOPPED);
		if (rc < 0) {
			perror("waitid");
			sleep(1);
			continue;
		}
		printf("pid %06d ", si.si_pid);
		switch (si.si_code) {
		case CLD_EXITED:
			printf("exited %d\n", si.si_status);
			nwait--;
			break;
		case CLD_KILLED:
		case CLD_DUMPED:
			printf("killed sig %d\n", si.si_status);
			nwait--;
			break;
		default:
			printf("si_code = %d\n", si.si_code);
			break;
		}
	}

	printf("end of test.\n");
	return 0;
}
