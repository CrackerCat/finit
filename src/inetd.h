/* Classic inetd services launcher for Finit
 *
 * Copyright (c) 2015-2020  Joachim Wiberg <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef FINIT_INETD_H_
#define FINIT_INETD_H_

#include <netdb.h>
#include <net/if.h>
#include <uev/uev.h>
#include <lite/queue.h>		/* BSD sys/queue.h API */

typedef struct svc svc_t;

typedef struct inetd_filter {
	TAILQ_ENTRY(inetd_filter) link;
	int  deny;		/* 0:allow, 1:deny */
	char ifname[IFNAMSIZ];	/* E.g., eth0 */
} inetd_filter_t;

typedef struct {
	uev_t  watcher;
	svc_t *svc;		/* svc_t pointer for the socket callback */

	int    type;		/* Socket type: SOCK_STREAM/SOCK_DGRAM    */
	int    std;		/* Standard proto/port from /etc/services */
	int    proto;
	int    port;
	int    forking;
	int    builtin;		/* Set by built-in inetd services only */
	int    next_id;		/* Next child job's id */
	char   name[10];
	int  (*cmd)(int type);	/* internal inetd service, like 'time' */

	TAILQ_HEAD(, inetd_filter) filters;
} inetd_t;

int     inetd_check_loop(struct sockaddr *sa, socklen_t len, char *name);

int     inetd_start     (inetd_t *inetd);
void    inetd_stop      (inetd_t *inetd);
void    inetd_stop_children (inetd_t *inetd, int check_allowed);

int     inetd_new       (inetd_t *inetd, char *name, char *service, char *proto, int forking, svc_t *svc);
int     inetd_del       (inetd_t *inetd);

svc_t  *inetd_find_svc  (char *path, char *service, char *proto);

int     inetd_match     (inetd_t *inetd, char *service, char *proto);
int     inetd_filter_str(inetd_t *inetd, char *str, size_t len);

int     inetd_flush     (inetd_t *inetd);
int     inetd_allow     (inetd_t *inetd, char *ifname);
int     inetd_deny      (inetd_t *inetd, char *ifname);
int     inetd_is_allowed(inetd_t *inetd, char *ifname);

#endif	/* FINIT_INETD_H_ */

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
