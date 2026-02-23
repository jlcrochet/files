#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lib/stdio_helpers.h"

static char base_path[PATH_MAX];

static bool gitignore = false;
static bool show_hidden = false;
static bool null_output = false;
static char output_sep = '\n';

static char *git_env[] = {
	"GIT_CONFIG_GLOBAL=/dev/null",
	"GIT_CONFIG_NOSYSTEM=1",
	"GIT_TERMINAL_PROMPT=0",
	NULL
};

static void walk_dir(int dir_fd, char **rel_path, size_t *rel_len, size_t *rel_cap);

static void rel_reserve(char **buf, size_t *cap, size_t needed)
{
	if (needed <= *cap)
		return;

	size_t new_cap = *cap ? *cap : 256;
	while (new_cap < needed) {
		if (new_cap > SIZE_MAX / 2) {
			new_cap = needed;
			break;
		}
		new_cap *= 2;
	}

	char *new_buf = realloc(*buf, new_cap);
	if (!new_buf) {
		PUTS_ERR("Error: out of memory\n");
		exit(EXIT_FAILURE);
	}
	*buf = new_buf;
	*cap = new_cap;
}

static void rel_set_len(char **buf, size_t *cap, size_t *len, size_t new_len)
{
	rel_reserve(buf, cap, new_len + 1);
	(*buf)[new_len] = '\0';
	*len = new_len;
}

static void rel_push(char **buf, size_t *cap, size_t *len, const char *name)
{
	size_t name_len = strlen(name);
	size_t old_len = *len;
	size_t extra = old_len ? 1 : 0;

	rel_reserve(buf, cap, old_len + extra + name_len + 1);
	if (old_len) {
		(*buf)[old_len] = '/';
		memcpy(*buf + old_len + 1, name, name_len + 1);
		*len = old_len + 1 + name_len;
	} else {
		memcpy(*buf, name, name_len + 1);
		*len = name_len;
	}
}

static bool is_dot_or_dotdot(const char *name)
{
	return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static int open_devnull(void)
{
	int fd = open("/dev/null", O_WRONLY);
	return fd;
}

static int wait_child(pid_t pid, int *status)
{
	int rc;
	do {
		rc = waitpid(pid, status, 0);
	} while (rc < 0 && errno == EINTR);
	return rc;
}

static bool path_has_hidden_component(const char *path, size_t len)
{
	if (len == 0)
		return false;

	for (size_t i = 0; i < len; ) {
		bool component_start = i == 0 || path[i - 1] == '/';
		if (component_start && path[i] == '.') {
			size_t next = i + 1;
			// Treat ".foo" as hidden, but not "." or ".." path segments.
			if (next < len && path[next] != '/') {
				if (!(path[next] == '.' && (next + 1 == len || path[next + 1] == '/')))
					return true;
			}
		}

		while (i < len && path[i] != '/')
			i++;
		if (i < len)
			i++;
	}

	return false;
}

static bool git_ls_files_stream(char *argv[], const char *prefix, size_t prefix_len)
{
	int fd[2];
	if (pipe(fd) != 0)
		return false;

	int null_fd = open_devnull();

	posix_spawn_file_actions_t actions;
	int action_rc = posix_spawn_file_actions_init(&actions);
	if (action_rc != 0) {
		close(fd[0]);
		close(fd[1]);
		return false;
	}

	action_rc = posix_spawn_file_actions_adddup2(&actions, fd[1], 1);
	if (action_rc != 0)
		goto actions_fail;
	if (null_fd >= 0)
		action_rc = posix_spawn_file_actions_adddup2(&actions, null_fd, 2);
	if (action_rc != 0)
		goto actions_fail;

	action_rc = posix_spawn_file_actions_addclose(&actions, fd[0]);
	if (action_rc != 0)
		goto actions_fail;
	action_rc = posix_spawn_file_actions_addclose(&actions, fd[1]);
	if (action_rc != 0)
		goto actions_fail;
	if (null_fd >= 0)
		action_rc = posix_spawn_file_actions_addclose(&actions, null_fd);
	if (action_rc != 0)
		goto actions_fail;

	pid_t pid;
	int code = posix_spawnp(&pid, argv[0], &actions, NULL, argv, git_env);
	posix_spawn_file_actions_destroy(&actions);

	if (null_fd >= 0)
		close(null_fd);

	if (code != 0) {
		close(fd[0]);
		close(fd[1]);
		return false;
	}

	close(fd[1]);

	char *tok = NULL;
	size_t tok_len = 0, tok_cap = 0;
	char buf[8192];
	ssize_t rn;
	bool ok = true;

	for (;;) {
		rn = read(fd[0], buf, sizeof(buf));
		if (rn == 0)
			break;
		if (rn < 0) {
			if (errno == EINTR)
				continue;
			ok = false;
			break;
		}
		for (ssize_t i = 0; i < rn; i++) {
			unsigned char ch = (unsigned char)buf[i];
			if (ch == '\0') {
				const char *out = tok;
				size_t out_len = tok_len;

				if (out_len > 0) {
					if (prefix_len > 0) {
						if (out_len > prefix_len && out[prefix_len] == '/' && memcmp(out, prefix, prefix_len) == 0) {
							out += prefix_len + 1;
							out_len -= prefix_len + 1;
						} else {
							out_len = 0;
						}
					}
					if (out_len > 0 && !show_hidden && path_has_hidden_component(out, out_len))
						out_len = 0;

					if (out_len > 0) {
						WRITE(out, out_len);
						PUTC(output_sep);
					}
				}

				tok_len = 0;
				continue;
			}

			if (tok_len + 1 > tok_cap) {
				size_t new_cap = tok_cap ? tok_cap * 2 : 256;
				while (new_cap < tok_len + 1) {
					if (new_cap > SIZE_MAX / 2) {
						new_cap = tok_len + 1;
						break;
					}
					new_cap *= 2;
				}
				char *new_tok = realloc(tok, new_cap);
				if (!new_tok) {
					free(tok);
					close(fd[0]);
					int status;
					(void)wait_child(pid, &status);
					PUTS_ERR("Error: out of memory\n");
					exit(EXIT_FAILURE);
				}
				tok = new_tok;
				tok_cap = new_cap;
			}
			tok[tok_len++] = (char)ch;
		}
	}

	if (ok && tok_len > 0) {
		const char *out = tok;
		size_t out_len = tok_len;

		if (prefix_len > 0) {
			if (out_len > prefix_len && out[prefix_len] == '/' && memcmp(out, prefix, prefix_len) == 0) {
				out += prefix_len + 1;
				out_len -= prefix_len + 1;
			} else {
				out_len = 0;
			}
		}
		if (out_len > 0 && !show_hidden && path_has_hidden_component(out, out_len))
			out_len = 0;

		if (out_len > 0) {
			WRITE(out, out_len);
			PUTC(output_sep);
		}
	}

	free(tok);
	close(fd[0]);

	int status;
	if (wait_child(pid, &status) < 0)
		return false;
	return ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;

actions_fail:
	posix_spawn_file_actions_destroy(&actions);
	if (null_fd >= 0)
		close(null_fd);
	close(fd[0]);
	close(fd[1]);
	return false;
}

static bool try_git_ls_files_from_base(void)
{
	// Run git ls-files from base_path; output is relative to base_path by default.
	char *ls_argv_dedup[] = {
		"git", "-C", base_path,
		"ls-files", "--cached", "--others", "--exclude-standard", "--deduplicate", "-z",
		NULL
	};

	char *ls_argv[] = {
		"git", "-C", base_path,
		"ls-files", "--cached", "--others", "--exclude-standard", "-z",
		NULL
	};

	if (git_ls_files_stream(ls_argv_dedup, "", 0))
		return true;
	return git_ls_files_stream(ls_argv, "", 0);
}

static void walk_dir(int dir_fd, char **rel_path, size_t *rel_len, size_t *rel_cap)
{
	DIR *dir = fdopendir(dir_fd);
	if (!dir) {
		close(dir_fd);
		return;
	}
	int parent_fd = dirfd(dir);
	if (parent_fd < 0) {
		closedir(dir);
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		const char *name = entry->d_name;

		if (show_hidden) {
			if (is_dot_or_dotdot(name))
				continue;
		} else if (name[0] == '.') {
			continue;
		}

		bool is_dir = false;
		if (entry->d_type == DT_DIR) {
			is_dir = true;
		} else if (entry->d_type == DT_UNKNOWN) {
			struct stat st;
			if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
				is_dir = true;
		}

		size_t old_len = *rel_len;
		rel_push(rel_path, rel_cap, rel_len, name);

		if (is_dir) {
			int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
			if (fd >= 0)
				walk_dir(fd, rel_path, rel_len, rel_cap);
		} else {
			PUTS(*rel_path);
			PUTC(output_sep);
		}

		rel_set_len(rel_path, rel_cap, rel_len, old_len);
	}

	closedir(dir);
}

int main(int argc, char **argv)
{
	struct option options[] = {
		{ "gitignore", no_argument, 0, 'i' },
		{ "show-hidden", no_argument, 0, 'H' },
		{ "null", no_argument, 0, '0' },
		{ "help", no_argument, 0, 'h' },
		{ 0 }
	};

	int c;

	while ((c = getopt_long(argc, argv, "iH0h", options, NULL)) != -1) {
		switch (c) {
			case '?':
				break;
			case 'i':
				gitignore = true;
				break;
			case 'H':
				show_hidden = true;
				break;
			case '0':
				null_output = true;
				break;
			case 'h':
				PUTS(
					"Usage: files [DIR]\n"
					"\n"
					"-i, --gitignore    Use git ignore rules if inside a repo\n"
					"-H, --show-hidden  Show hidden files\n"
					"-0, --null         Separate entries with NUL\n"
					"-h, --help         Print this help"
				);
				return EXIT_SUCCESS;
		}
	}

	if (optind + 1 < argc) {
		PUTS_ERR("Usage: files [DIR]\n");
		return EXIT_FAILURE;
	}

	if (null_output)
		output_sep = '\0';

	char *dir = argv[optind];
	if (!realpath(dir ? dir : ".", base_path)) {
		perror(dir ? dir : ".");
		return EXIT_FAILURE;
	}

	char *rel_path = NULL;
	size_t rel_len = 0, rel_cap = 0;
	rel_set_len(&rel_path, &rel_cap, &rel_len, 0);

	int root_fd = open(base_path, O_RDONLY | O_DIRECTORY);
	if (root_fd < 0) {
		perror(base_path);
		free(rel_path);
		return EXIT_FAILURE;
	}

		if (gitignore) {
			close(root_fd);
			if (try_git_ls_files_from_base()) {
				free(rel_path);
				return EXIT_SUCCESS;
			}
			root_fd = open(base_path, O_RDONLY | O_DIRECTORY);
			if (root_fd < 0) {
				perror(base_path);
				free(rel_path);
				return EXIT_FAILURE;
			}
		}

	walk_dir(root_fd, &rel_path, &rel_len, &rel_cap);
	free(rel_path);
	return EXIT_SUCCESS;
}
