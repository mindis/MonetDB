/*
 * The contents of this file are subject to the MonetDB Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.monetdb.org/Legal/MonetDBLicense
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the MonetDB Database System.
 *
 * The Initial Developer of the Original Code is CWI.
 * Portions created by CWI are Copyright (C) 1997-July 2008 CWI.
 * Copyright August 2008-2012 MonetDB B.V.
 * All Rights Reserved.
 */

/*
 * @f alarm
 * @a M.L. Kersten, P. Boncz
 *
 * @+ Timers and Timed Interrupts
 * This module handles various signaling/timer functionalities.
 * The Monet interface supports two timer commands: @emph{ alarm} and @emph{ sleep}.
 * Their argument is the number of seconds to wait before the timer goes off.
 * The @emph{ sleep} command blocks till the alarm goes off.
 * The @emph{ alarm} command continues directly, executes off a MIL
 * string when it goes off.
 * The parameterless routines @emph{ time} and @emph{ ctime} provide access to
 * the cpu clock.They return an integer and string, respectively.
 */
#include "monetdb_config.h"
#include "alarm.h"
#include <time.h>

#ifdef WIN32
#if !defined(LIBMAL) && !defined(LIBATOMS) && !defined(LIBKERNEL) && !defined(LIBMAL) && !defined(LIBOPTIMIZER) && !defined(LIBSCHEDULER) && !defined(LIBMONETDB5)
#define alarm_export extern __declspec(dllimport)
#else
#define alarm_export extern __declspec(dllexport)
#endif
#else
#define alarm_export extern
#endif

alarm_export str ALARMprelude(void);
alarm_export str ALARMepilogue(void);
alarm_export str ALARMusec(lng *ret);
alarm_export str ALARMsleep(int *res, int *secs);
alarm_export str ALARMsetalarm(int *res, int *secs, str *action);
alarm_export str ALARMtimers(int *res);
alarm_export str ALARMctime(str *res);
alarm_export str ALARMepoch(int *res);
alarm_export str ALARMtime(int *res);

static monet_timer_t timer[MAXtimer];
static int timerTop = 0;

/*
 * @
 * @-
 * The timer is awakened by a clock interrupt. The interrupt granularity
 * is OS-dependent. The timer should be initialized as long as there
 * are outstanding timer events.
 */
#ifdef SIGALRM
static void
CLKinitTimer(int sec, int usec)
{
	int i = sec - time(0);

	(void) usec;

	alarm(i);
}
#endif
/*
 * @-
 * A new alarm is pushed onto the stack using @%CLKalarm@.
 * The parameter is the real-time value to be approximated.
 */
#if 0
#ifdef SIGALRM
static void
CLKalarm(time_t t, str action)
{
	int j;
	int k;


	if (timerTop == MAXtimer) {
		GDKerror("CLKalarm: timer stack overflow\n");
		return;
	}
	for (j = 0; j < timerTop; j++) {
		if (timer[j].alarm_time > t)
			break;
	}
	for (k = timerTop; k > j; k--) {
		timer[k] = timer[k - 1];
	}
	timer[k].alarm_time = t;
	if (action) {
		timer[k].action = GDKstrdup(action);
	} else {
		timer[k].action = 0;
		MT_sema_init(&timer[k].sema, 0, "timersema");
	}
	if (k == timerTop++) {
		CLKinitTimer(t, 0);	/* set it sooner */
	}
}
#endif
#endif
/*
 * @-
 * Once a timer interrupt occurs, we should inspect the timer queue and
 * emit a notify signal.
 */
#ifdef SIGALRM
/* HACK to pacify compiler */
#if (defined(__INTEL_COMPILER) && (SIZEOF_VOID_P > SIZEOF_INT))
#undef  SIG_ERR			/*((__sighandler_t)-1 ) */
#define SIG_ERR   ((__sighandler_t)-1L)
#endif
static void
CLKsignal(int nr)
{
	/* int restype; */
	int k = timerTop;
	int t;

	(void) nr;

	if (signal(SIGALRM, CLKsignal) == SIG_ERR) {
		GDKsyserror("CLKsignal: call failed\n");
	}

	if (timerTop == 0) {
		return;
	}
	t = time(0);
	while (k-- && t >= timer[k].alarm_time) {
		if (timer[k].action) {
			/* monet_eval(timer[k].action, &restype); */
			GDKfree(timer[k].action);
		} else {
			MT_sema_up(&timer[k].sema, "CLKsignal");
		}
		timerTop--;
	}
	if (timerTop > 0) {
		CLKinitTimer(timer[timerTop - 1].alarm_time, 0);
	}
}
#endif

static int
CMDsleep(int *secs)
{

	if (*secs < 0) {
		GDKerror("CMDsleep: negative delay\n");
		return GDK_FAIL;
	} else {
#ifdef __CYGWIN__
		/* CYGWIN cannot handle SIGALRM with sleep */
		lng t = GDKusec() + (*secs)*1000000;

		while (GDKusec() < t)
			;
#else
		MT_sleep_ms(*secs * 1000);
#endif
	}
	return GDK_SUCCEED;
}

/*
 * @-
 * Problem with CMDtimers is that they use static buffers that
 * may be overwritten under parallel processing.
 * Therefore, the code below is dangerous (!) and the re-entrant code
 * should be used.  However, on Windows where ctime_r is not available,
 * ctime is actually thread-safe.
 */
#if 0
static int
CMDtimers(BAT **retval)
{
	char buf[27];
	int k;

	*retval = BATnew(TYPE_str, TYPE_str, timerTop);
	if (*retval == NULL)
		return GDK_FAIL;
	BATroles(*retval, "alarm", "action");
	for (k = 0; k < timerTop; k++) {
		time_t t = timer[k].alarm_time;

#ifdef HAVE_CTIME_R3
		ctime_r(&t, buf, sizeof(buf));
#else
#ifdef HAVE_CTIME_R
		ctime_r(&t, buf);
#else
		strncpy(buf, ctime(&t), sizeof(buf));
#endif
#endif
		BUNins(*retval, buf, timer[k].action ? timer[k].action : "barrier", FALSE);
	}
	return GDK_SUCCEED;
}
#endif

static int
CMDctime(str *retval)
{
	time_t t = time(0);
	char *base, *c;

#ifdef HAVE_CTIME_R3
	char buf[26];

	ctime_r(&t, buf, sizeof(buf));
	base = buf;
#else
#ifdef HAVE_CTIME_R
	char buf[26];

	ctime_r(&t, buf);
	base = buf;
#else
	base = ctime(&t);
#endif
#endif
	if (base == NULL) {
		/* very unlikely to happen... */
		GDKerror("CMDctime: failed to format time\n");
		return GDK_FAIL;
	}
	c = strchr(base, '\n');
	if (c)
		*c = 0;
	*retval = GDKstrdup(base);
	return GDK_SUCCEED;
}

static int
CMDepoch(int *retval)		/* XXX should be lng */
{
	*retval = (int) time(0);
	return GDK_SUCCEED;
}

/* should return lng */
static int
CMDusec(lng *retval)
{
	*retval = GDKusec();
	return GDK_SUCCEED;
}

static int
CMDtime(int *retval)
{
	*retval = GDKms();
	return GDK_SUCCEED;
}

/*
 * @- Wrapping
 * Wrapping the Version 4 code base
 */
#include "mal.h"
#include "mal_exception.h"

#if 0
void
ALARMinitTimer(int sec, int usec)
{
	(void) sec;
	(void) usec;
#ifdef SIGALRM
	CLKinitTimer(sec, usec);
#endif
}

#ifdef SIGALRM
str
ALARMalarm(int t, str *action)
{
	CLKalarm(t, *action);
	return MAL_SUCCEED;
}
#endif
#endif

str
ALARMprelude(void)
{
#ifdef SIGALRM
	(void) signal(SIGALRM, (void (*)()) CLKsignal);
#endif
	return MAL_SUCCEED;
}

str
ALARMepilogue(void)
{
	int k;

#if (defined(SIGALRM) && defined(SIG_IGN))
/* HACK to pacify compiler */
#if (defined(__INTEL_COMPILER) && (SIZEOF_VOID_P > SIZEOF_INT))
#undef  SIG_IGN			/*((__sighandler_t)1 ) */
#define SIG_IGN   ((__sighandler_t)1L)
#endif
	(void) signal(SIGALRM, SIG_IGN);
#endif
	for (k = 0; k < timerTop; k++) {
		if (timer[k].action)
			GDKfree(timer[k].action);
	}
	return MAL_SUCCEED;
}

str
ALARMusec(lng *ret)
{
	CMDusec(ret);
	return MAL_SUCCEED;
}

str
ALARMsleep(int *res, int *secs)
{
	(void) res;		/* fool compilers */
	CMDsleep(secs);
	return MAL_SUCCEED;
}

str
ALARMsetalarm(int *res, int *secs, str *action)
{
	(void) res;
	(void) secs;
	(void) action;		/* foolc compiler */
	throw(MAL, "alarm.setalarm", PROGRAM_NYI);
}

str
ALARMtimers(int *res)
{
	(void) res;		/* fool compiler */
	throw(MAL, "alarm.timers", PROGRAM_NYI);
}

str
ALARMctime(str *res)
{
	CMDctime(res);
	return MAL_SUCCEED;
}

str
ALARMepoch(int *res)
{
	CMDepoch(res);
	return MAL_SUCCEED;
}

str
ALARMtime(int *res)
{
	CMDtime(res);
	return MAL_SUCCEED;
}

