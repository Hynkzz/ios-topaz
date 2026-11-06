/*
 * getpwnam/getpwuid for dropbear on the SWEET/ginkgo iOS port.
 *
 * libinfo's user lookups fail on the port (id root -> "no such user", by
 * name and by uid, with usermanagerd running), so sshd rejects every login
 * as "Invalid user". Defining the two functions in the dropbear binary
 * makes its own calls bind here instead of libSystem: they read the BSD
 * /etc/master.passwd (10 fields) directly, or /etc/passwd (7 fields) when
 * that is not readable.
 */
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static struct passwd pw;
static char line[1024];

static int
split(char *s, char **f, int max)
{
	int n = 0;
	f[n++] = s;
	for (; *s && n < max; s++) {
		if (*s == ':') {
			*s = '\0';
			f[n++] = s + 1;
		}
	}
	return n;
}

static struct passwd *
lookup(const char *name, uid_t uid, int byname)
{
	const char *files[] = { "/etc/master.passwd", "/etc/passwd" };
	for (int i = 0; i < 2; i++) {
		FILE *fp = fopen(files[i], "r");
		if (fp == NULL) {
			continue;
		}
		while (fgets(line, sizeof(line), fp) != NULL) {
			char *f[10];
			line[strcspn(line, "\r\n")] = '\0';
			if (line[0] == '#' || line[0] == '\0') {
				continue;
			}
			int n = split(line, f, 10);
			int master = (i == 0);
			if (n != (master ? 10 : 7)) {
				continue;
			}
			uid_t u = (uid_t)strtoul(f[2], NULL, 10);
			if (byname ? strcmp(f[0], name) != 0 : u != uid) {
				continue;
			}
			memset(&pw, 0, sizeof(pw));
			pw.pw_name = f[0];
			pw.pw_passwd = f[1];
			pw.pw_uid = u;
			pw.pw_gid = (gid_t)strtoul(f[3], NULL, 10);
#ifdef __APPLE__
			pw.pw_class = master ? f[4] : "";
#endif
			pw.pw_gecos = master ? f[7] : f[4];
			pw.pw_dir = master ? f[8] : f[5];
			pw.pw_shell = master ? f[9] : f[6];
			fclose(fp);
			return &pw;
		}
		fclose(fp);
	}
	return NULL;
}

struct passwd *
getpwnam(const char *name)
{
	return lookup(name, 0, 1);
}

struct passwd *
getpwuid(uid_t uid)
{
	return lookup(NULL, uid, 0);
}
