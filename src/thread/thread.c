/*
 * Copyright (C) 2008 Search Solution Corporation. All rights reserved by Search Solution.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/*
 * thread.c - Thread management module at the server
 */

#ident "$Id$"

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#if defined(WINDOWS)
#include <process.h>
#else /* WINDOWS */
#include <sys/time.h>
#include <unistd.h>
#endif /* WINDOWS */
#include <assert.h>

#include "porting.h"
#include "connection_error.h"
#include "job_queue.h"
#include "thread.h"
#include "critical_section.h"
#include "system_parameter.h"
#include "memory_alloc.h"
#include "environment_variable.h"
#include "connection_defs.h"
#include "storage_common.h"
#include "page_buffer.h"
#include "lock_manager.h"
#include "log_impl.h"
#include "log_manager.h"
#include "boot_sr.h"
#include "transaction_sr.h"
#include "boot_sr.h"
#include "connection_sr.h"
#include "server_support.h"
#include "log_compress.h"
#include "perf_monitor.h"
#include "session.h"
#if defined(WINDOWS)
#include "wintcp.h"
#else /* WINDOWS */
#include "tcp.h"
#endif /* WINDOWS */

#if defined(WINDOWS)
#include "heartbeat.h"
#endif


#if defined(HPUX)
#define thread_initialize_key()
#endif /* HPUX */


/* Thread Manager structure */
typedef struct thread_manager THREAD_MANAGER;
struct thread_manager
{
  THREAD_ENTRY *thread_array;	/* thread entry array */
  int num_total;
  int num_workers;
  int num_daemons;
  bool initialized;
};

static const int THREAD_RETRY_MAX_SLAM_TIMES = 10;

#if defined(HPUX)
static __thread THREAD_ENTRY *tsd_ptr;
#else /* HPUX */
static pthread_key_t css_Thread_key;
#endif /* HPUX */

static THREAD_MANAGER thread_Manager;

/*
 * For special Purpose Threads: deadlock detector, checkpoint daemon
 *    Under the win32-threads system, *_cond variables are an auto-reset event
 */
static DAEMON_THREAD_MONITOR
  thread_Deadlock_detect_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Checkpoint_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Purge_archive_logs_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Oob_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Page_flush_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Flush_control_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Session_control_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
DAEMON_THREAD_MONITOR
  thread_Log_flush_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Check_ha_delay_info_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Auto_volume_expansion_thread = DAEMON_THREAD_MONITOR_INITIALIZER;
static DAEMON_THREAD_MONITOR
  thread_Log_clock_thread = DAEMON_THREAD_MONITOR_INITIALIZER;

static int thread_initialize_entry (THREAD_ENTRY * entry_ptr);
static int thread_finalize_entry (THREAD_ENTRY * entry_ptr);

static THREAD_ENTRY *thread_find_entry_by_tran_index (int tran_index);
static void thread_stop_oob_handler_thread ();
static void thread_stop_daemon (DAEMON_THREAD_MONITOR * daemon_monitor);
static void thread_wakeup_daemon_thread (DAEMON_THREAD_MONITOR *
					 daemon_monitor);
static int thread_compare_shutdown_sequence_of_daemon (const void *p1,
						       const void *p2);

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_deadlock_detect_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_checkpoint_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_purge_archive_logs_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_page_flush_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_flush_control_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_log_flush_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_session_control_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_check_ha_delay_info_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_auto_volume_expansion_thread (void *);
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_log_clock_thread (void *);

typedef struct thread_daemon THREAD_DAEMON;
struct thread_daemon
{
  DAEMON_THREAD_MONITOR *daemon_monitor;
  int shutdown_sequence;
  void *daemon_function;
};

static THREAD_DAEMON thread_Daemons[] = {
  {&thread_Oob_thread, 1,
   (void *) css_oob_handler_thread},
  {&thread_Deadlock_detect_thread, 2,
   (void *) thread_deadlock_detect_thread},
  {&thread_Purge_archive_logs_thread, 3,
   (void *) thread_purge_archive_logs_thread},
  {&thread_Checkpoint_thread, 4,
   (void *) thread_checkpoint_thread},
  {&thread_Session_control_thread, 5,
   (void *) thread_session_control_thread},
  {&thread_Check_ha_delay_info_thread, 6,
   (void *) thread_check_ha_delay_info_thread},
  {&thread_Auto_volume_expansion_thread, 7,
   (void *) thread_auto_volume_expansion_thread},
  {&thread_Log_clock_thread, 8,
   (void *) thread_log_clock_thread},


  /* the following threads must be shutdown at the last iterations */

  {&thread_Page_flush_thread, INT_MAX - 2,
   (void *) thread_page_flush_thread},
  {&thread_Flush_control_thread, INT_MAX - 1,
   (void *) thread_flush_control_thread},
  {&thread_Log_flush_thread, INT_MAX,
   (void *) thread_log_flush_thread}
};

static int css_initialize_sync_object (void);
static int thread_wakeup_internal (THREAD_ENTRY * thread_p, int resume_reason,
				   bool had_mutex);
static void thread_reset_nrequestors_of_log_flush_thread (void);

extern int catcls_get_apply_info_log_record_time (THREAD_ENTRY * thread_p,
						  time_t * log_record_time);

/*
 * Thread Specific Data management
 *
 * All kind of thread has its own information like request id, error code,
 * synchronization informations, etc. We use THREAD_ENTRY structure
 * which saved as TSD(thread specific data) to manage these informations.
 * Global thread manager(thread_mgr) has an array of these entries which is
 * initialized by the 'thread_mgr'.
 * Each worker thread picks one up from this array.
 */

#if defined(HPUX)
/*
 * thread_get_thread_entry_info() - retrieve TSD of its own.
 *   return: thread entry
 */
THREAD_ENTRY *
thread_get_thread_entry_info ()
{
#if defined (SERVER_MODE)
  assert (tsd_ptr != NULL);
#endif
  return tsd_ptr;
}
#else /* HPUX */
/*
 * thread_initialize_key() - allocates a key for TSD
 *   return: 0 if no error, or error code
 */
static int
thread_initialize_key (void)
{
  int r;

  r = pthread_key_create (&css_Thread_key, NULL);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_KEY_CREATE, 0);
      return ER_CSS_PTHREAD_KEY_CREATE;
    }
  return r;
}

/*
 * thread_set_thread_entry_info() - associates TSD with entry
 *   return: 0 if no error, or error code
 *   entry_p(in): thread entry
 */
int
thread_set_thread_entry_info (THREAD_ENTRY * entry_p)
{
  int r;

  r = pthread_setspecific (css_Thread_key, (void *) entry_p);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_SETSPECIFIC, 0);
      return ER_CSS_PTHREAD_SETSPECIFIC;
    }
  return r;
}

/*
 * thread_get_thread_entry_info() - retrieve TSD of its own.
 *   return: thread entry
 */
THREAD_ENTRY *
thread_get_thread_entry_info (void)
{
  void *p;

  p = pthread_getspecific (css_Thread_key);
#if defined (SERVER_MODE)
  assert (p != NULL);
#endif
  return (THREAD_ENTRY *) p;
}
#endif /* HPUX */

/*
 * Thread Manager related functions
 *
 * Global thread manager, thread_mgr, related functions. It creates/destroys
 * TSD and takes control over actual threads, for example master, worker,
 * oob-handler.
 */

/*
 * thread_is_manager_initialized() -
 *   return:
 */
int
thread_is_manager_initialized (void)
{
  return thread_Manager.initialized;
}

/*
 * thread_initialize_manager() - Create and initialize all necessary threads.
 *   return: 0 if no error, or error code
 *
 * Note: It includes a main thread, service handler, a deadlock detector
 *       and a checkpoint daemon. Some other threads like signal handler
 *       might be needed later.
 */
int
thread_initialize_manager (void)
{
  int i, r;
  size_t size;
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* not HPUX */

  assert (NUM_NORMAL_TRANS >= 10);

  if (thread_Manager.initialized == false)
    {
      r = thread_initialize_key ();
      if (r != NO_ERROR)
	{
	  return r;
	}

#if defined(WINDOWS)
      r = pthread_mutex_init (&css_Internal_mutex_for_mutex_initialize, NULL);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_MUTEX_INIT, 0);
	  return ER_CSS_PTHREAD_MUTEX_INIT;
	}
      css_initialize_sync_object ();
#endif /* WINDOWS */

    }
  else
    {
      /* destroy mutex and cond */
      for (i = 1; i < thread_Manager.num_total; i++)
	{
	  r = thread_finalize_entry (&thread_Manager.thread_array[i]);
	  if (r != NO_ERROR)
	    {
	      return r;
	    }
	}
      r = thread_finalize_entry (&thread_Manager.thread_array[0]);
      if (r != NO_ERROR)
	{
	  return r;
	}
      free_and_init (thread_Manager.thread_array);
    }

  thread_Manager.num_workers = NUM_NON_SYSTEM_TRANS * 2;
  thread_Manager.num_daemons = DIM (thread_Daemons);
  thread_Manager.num_total = (thread_Manager.num_workers
			      + thread_Manager.num_daemons +
			      NUM_SYSTEM_TRANS);

  size = thread_Manager.num_total * sizeof (THREAD_ENTRY);
  tsd_ptr = thread_Manager.thread_array = (THREAD_ENTRY *) malloc (size);
  if (tsd_ptr == NULL)
    {
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_OUT_OF_VIRTUAL_MEMORY, 1,
	      size);
      return ER_OUT_OF_VIRTUAL_MEMORY;
    }

  /* initialize master thread */
  r = thread_initialize_entry (tsd_ptr);
  if (r != NO_ERROR)
    {
      return r;
    }

  tsd_ptr->index = 0;
  tsd_ptr->tid = pthread_self ();
  tsd_ptr->status = TS_RUN;
  tsd_ptr->resume_status = THREAD_RESUME_NONE;
  tsd_ptr->tran_index = 0;	/* system transaction */
  thread_set_thread_entry_info (tsd_ptr);

  /* init worker/deadlock-detection/checkpoint daemon/audit-flush
     oob-handler thread/page flush thread/log flush thread
     thread_mgr.thread_array[0] is used for main thread */
  for (i = 1; i < thread_Manager.num_total; i++)
    {
      r = thread_initialize_entry (&thread_Manager.thread_array[i]);
      if (r != NO_ERROR)
	{
	  return r;
	}
      thread_Manager.thread_array[i].index = i;
    }

  thread_Manager.initialized = true;

  return NO_ERROR;
}

/*
 * thread_start_workers() - Boot up every threads.
 *   return: 0 if no error, or error code
 *
 * Note: All threads are set ready to execute when activation condition is
 *       satisfied.
 */
int
thread_start_workers (void)
{
  int i;
  int thread_index, r;
  THREAD_ENTRY *thread_p;
  pthread_attr_t thread_attr;
#if defined(_POSIX_THREAD_ATTR_STACKSIZE)
  size_t ts_size;
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */

  assert (thread_Manager.initialized == true);

#if !defined(WINDOWS)
  r = pthread_attr_init (&thread_attr);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_ATTR_INIT, 0);
      return ER_CSS_PTHREAD_ATTR_INIT;
    }

  r = pthread_attr_setdetachstate (&thread_attr, PTHREAD_CREATE_DETACHED);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_ATTR_SETDETACHSTATE, 0);
      return ER_CSS_PTHREAD_ATTR_SETDETACHSTATE;
    }

#if defined(AIX)
  /* AIX's pthread is slightly different from other systems.
     Its performance highly depends on the pthread's scope and its related
     kernel parameters. */
  r = pthread_attr_setscope (&thread_attr,
			     prm_get_bool_value (PRM_ID_PTHREAD_SCOPE_PROCESS)
			     ? PTHREAD_SCOPE_PROCESS : PTHREAD_SCOPE_SYSTEM);
#else /* AIX */
  r = pthread_attr_setscope (&thread_attr, PTHREAD_SCOPE_SYSTEM);
#endif /* AIX */
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_ATTR_SETSCOPE, 0);
      return ER_CSS_PTHREAD_ATTR_SETSCOPE;
    }

#if defined(_POSIX_THREAD_ATTR_STACKSIZE)
  r = pthread_attr_getstacksize (&thread_attr, &ts_size);
  if (ts_size != (size_t) prm_get_bigint_value (PRM_ID_THREAD_STACKSIZE))
    {
      r =
	pthread_attr_setstacksize (&thread_attr,
				   prm_get_bigint_value
				   (PRM_ID_THREAD_STACKSIZE));
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_ATTR_SETSTACKSIZE, 0);
	  return ER_CSS_PTHREAD_ATTR_SETSTACKSIZE;
	}
    }
#endif /* _POSIX_THREAD_ATTR_STACKSIZE */
#endif /* WINDOWS */

  /* start worker thread */
  for (thread_index = 1; thread_index <= thread_Manager.num_workers;
       thread_index++)
    {
      thread_p = &thread_Manager.thread_array[thread_index];

      r = pthread_mutex_lock (&thread_p->th_entry_lock);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_MUTEX_LOCK, 0);
	  return ER_CSS_PTHREAD_MUTEX_LOCK;
	}

      /* If win32, then "thread_attr" is ignored, else "p->thread_handle". */
      r = pthread_create (&thread_p->tid, &thread_attr, thread_worker,
			  thread_p);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_CREATE, 0);
	  pthread_mutex_unlock (&thread_p->th_entry_lock);
	  return ER_CSS_PTHREAD_CREATE;
	}

      r = pthread_mutex_unlock (&thread_p->th_entry_lock);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
	  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
	}
    }

  for (i = 0; thread_index < thread_Manager.num_total; thread_index++, i++)
    {
      thread_Daemons[i].daemon_monitor->thread_index = thread_index;
      thread_p = &thread_Manager.thread_array[thread_index];

      r = pthread_mutex_lock (&thread_p->th_entry_lock);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_MUTEX_LOCK, 0);
	  return ER_CSS_PTHREAD_MUTEX_LOCK;
	}

      /* If win32, then "thread_attr" is ignored, else "p->thread_handle". */
      r = pthread_create (&thread_p->tid, &thread_attr,
			  thread_Daemons[i].daemon_function, thread_p);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_CREATE, 0);
	  pthread_mutex_unlock (&thread_p->th_entry_lock);
	  return ER_CSS_PTHREAD_CREATE;
	}

      r = pthread_mutex_unlock (&thread_p->th_entry_lock);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
	  return ER_CSS_PTHREAD_MUTEX_UNLOCK;
	}
    }

  /* destroy thread_attribute */
  r = pthread_attr_destroy (&thread_attr);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_ATTR_DESTROY, 0);
      return ER_CSS_PTHREAD_ATTR_DESTROY;
    }

  return NO_ERROR;
}

/*
 * thread_stop_active_workers() - Stop active work thread.
 *   return: 0 if no error, or error code
 *
 * Node: This function is invoked when system is going shut down.
 */
int
thread_stop_active_workers (unsigned short stop_phase)
{
  int i;
  int r;
  bool repeat_loop;
  THREAD_ENTRY *thread_p;
  CSS_CONN_ENTRY *conn_p;

  assert (thread_Manager.initialized == true);

  if (stop_phase == THREAD_STOP_WORKERS_EXCEPT_LOGWR)
    {
      css_block_all_active_conn (stop_phase);
    }

loop:
  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];

      conn_p = thread_p->conn_entry;
      if ((stop_phase == THREAD_STOP_LOGWR && conn_p == NULL)
	  || (conn_p && conn_p->stop_phase != stop_phase))
	{
	  continue;
	}

      if (thread_p->tran_index != -1)
	{
	  if (stop_phase == THREAD_STOP_WORKERS_EXCEPT_LOGWR)
	    {
	      logtb_set_tran_index_interrupt (NULL, thread_p->tran_index, 1);
	    }

	  if (thread_p->status == TS_WAIT)
	    {
	      if (stop_phase == THREAD_STOP_WORKERS_EXCEPT_LOGWR)
		{
		  thread_p->interrupted = true;
		  thread_wakeup (thread_p, THREAD_RESUME_DUE_TO_INTERRUPT);
		}
	      else if (stop_phase == THREAD_STOP_LOGWR)
		{
		  /*
		   * we can only wakeup LWT when waiting on THREAD_LOGWR_SUSPENDED.
		   */
		  r =
		    thread_check_suspend_reason_and_wakeup (thread_p,
							    THREAD_RESUME_DUE_TO_INTERRUPT,
							    THREAD_LOGWR_SUSPENDED);
		  if (r == NO_ERROR)
		    {
		      thread_p->interrupted = true;
		    }
		}
	    }
	}
    }

  thread_sleep (50);		/* 50 msec */

  lock_force_timeout_lock_wait_transactions (stop_phase);

  /* Signal for blocked on job queue */
  /* css_broadcast_shutdown_thread(); */

  repeat_loop = false;
  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];

      conn_p = thread_p->conn_entry;
      if ((stop_phase == THREAD_STOP_LOGWR && conn_p == NULL)
	  || (conn_p && conn_p->stop_phase != stop_phase))
	{
	  continue;
	}

      if (thread_p->status != TS_FREE)
	{
	  repeat_loop = true;
	}
    }

  if (repeat_loop)
    {
      if (css_is_shutdown_timeout_expired ())
	{
#if CUBRID_DEBUG
	  logtb_dump_trantable (NULL, stderr);
#endif
	  er_log_debug (ARG_FILE_LINE,
			"thread_stop_active_workers: _exit(0)\n");
	  /* exit process after some tries */
	  _exit (0);
	}

      goto loop;
    }

  /*
   * we must not block active connection before terminating log writer thread.
   */
  if (stop_phase == THREAD_STOP_LOGWR)
    {
      css_block_all_active_conn (stop_phase);
    }

  return NO_ERROR;
}

/*
 * thread_wakeup_daemon_thread() -
 *
 */
static void
thread_wakeup_daemon_thread (DAEMON_THREAD_MONITOR * daemon_monitor)
{
  int rv;

  rv = pthread_mutex_lock (&daemon_monitor->lock);
  pthread_cond_signal (&daemon_monitor->cond);
  pthread_mutex_unlock (&daemon_monitor->lock);
}

/*
 * thread_stop_daemon() -
 *
 */
static void
thread_stop_daemon (DAEMON_THREAD_MONITOR * daemon_monitor)
{
  THREAD_ENTRY *thread_p;

  thread_p = &thread_Manager.thread_array[daemon_monitor->thread_index];
  thread_p->shutdown = true;

  while (thread_p->status != TS_DEAD)
    {
      thread_wakeup_daemon_thread (daemon_monitor);

      if (css_is_shutdown_timeout_expired ())
	{
	  er_log_debug (ARG_FILE_LINE,
			"thread_stop_daemon(%d): _exit(0)\n",
			daemon_monitor->thread_index);
	  /* exit process after some tries */
	  _exit (0);
	}
      thread_sleep (10);	/* 10 msec */
    }
}

/*
 * thread_compare_shutdown_sequence_of_daemon () -
 *   return: p1 - p2
 *   p1(in): daemon thread 1
 *   p2(in): daemon thread 2
 */
static int
thread_compare_shutdown_sequence_of_daemon (const void *p1, const void *p2)
{
  THREAD_DAEMON *daemon1, *daemon2;

  daemon1 = (THREAD_DAEMON *) p1;
  daemon2 = (THREAD_DAEMON *) p2;

  return daemon1->shutdown_sequence - daemon2->shutdown_sequence;
}

/*
 * thread_stop_oob_handler_thread () -
 */
static void
thread_stop_oob_handler_thread ()
{
  THREAD_ENTRY *thread_p;

  thread_p = &thread_Manager.thread_array[thread_Oob_thread.thread_index];
  thread_p->shutdown = true;

  while (thread_p->status != TS_DEAD)
    {
      thread_wakeup_oob_handler_thread ();

      if (css_is_shutdown_timeout_expired ())
	{
	  er_log_debug (ARG_FILE_LINE,
			"thread_stop_oob_handler_thread: _exit(0)\n");
	  /* exit process after some tries */
	  _exit (0);
	}
      thread_sleep (10);	/* 10 msec */
    }
}

/*
 * thread_stop_active_daemons() - Stop deadlock detector/checkpoint threads
 *   return: NO_ERROR
 */
int
thread_stop_active_daemons (void)
{
  int i;

  thread_stop_oob_handler_thread ();

  for (i = 0; i < thread_Manager.num_daemons; i++)
    {
      assert ((i == 0)
	      || (thread_Daemons[i - 1].shutdown_sequence
		  < thread_Daemons[i].shutdown_sequence));

      thread_stop_daemon (thread_Daemons[i].daemon_monitor);
    }

  return NO_ERROR;
}

/*
 * thread_kill_all_workers() - Signal all worker threads to exit.
 *   return: 0 if no error, or error code
 */
int
thread_kill_all_workers (void)
{
  int i;
  bool repeat_loop;
  THREAD_ENTRY *thread_p;

  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];
      thread_p->interrupted = true;
      thread_p->shutdown = true;
    }

loop:

  /* Signal for blocked on job queue */
  css_broadcast_shutdown_thread ();

  repeat_loop = false;
  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];
      if (thread_p->status != TS_DEAD)
	{
	  if (thread_p->status == TS_FREE
	      && thread_p->resume_status == THREAD_JOB_QUEUE_SUSPENDED)
	    {
	      /* Defence code of a bug.
	       * wake up a thread which is not in the list of Job queue,
	       * but is waiting for a job.
	       */
	      thread_wakeup (thread_p, THREAD_RESUME_DUE_TO_SHUTDOWN);
	    }
	  repeat_loop = true;
	}
    }

  if (repeat_loop)
    {
      if (css_is_shutdown_timeout_expired ())
	{
#if CUBRID_DEBUG
	  xlogtb_dump_trantable (NULL, stderr);
#endif
	  er_log_debug (ARG_FILE_LINE, "thread_kill_all_workers: _exit(0)\n");
	  /* exit process after some tries */
	  _exit (0);
	}
      thread_sleep (1000);	/* 1000 msec */
      goto loop;
    }

  return NO_ERROR;
}

/*
 * thread_final_manager() -
 *   return: void
 */
void
thread_final_manager (void)
{
  int i;

  for (i = 1; i < thread_Manager.num_total; i++)
    {
      (void) thread_finalize_entry (&thread_Manager.thread_array[i]);
    }
  (void) thread_finalize_entry (&thread_Manager.thread_array[0]);
  free_and_init (thread_Manager.thread_array);

#ifndef HPUX
  pthread_key_delete (css_Thread_key);
#endif /* not HPUX */
}

/*
 * thread_initialize_entry() - Initialize thread entry
 *   return: void
 *   entry_ptr(in): thread entry to initialize
 */
static int
thread_initialize_entry (THREAD_ENTRY * entry_p)
{
  int r;
  struct timeval t;

  entry_p->index = -1;
  entry_p->tid = ((pthread_t) 0);
  entry_p->client_id = -1;
  entry_p->tran_index = -1;
  r = pthread_mutex_init (&entry_p->tran_index_lock, NULL);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_INIT, 0);
      return ER_CSS_PTHREAD_MUTEX_INIT;
    }

  entry_p->rid = 0;
  entry_p->status = TS_DEAD;
  entry_p->interrupted = false;
  entry_p->shutdown = false;
  entry_p->cnv_adj_buffer[0] = NULL;
  entry_p->cnv_adj_buffer[1] = NULL;
  entry_p->cnv_adj_buffer[2] = NULL;
  entry_p->conn_entry = NULL;
  entry_p->worker_thrd_list = NULL;
  gettimeofday (&t, NULL);
  entry_p->rand_seed = (unsigned int) t.tv_usec;
  srand48_r ((long) t.tv_usec, &entry_p->rand_buf);

  r = pthread_mutex_init (&entry_p->th_entry_lock, NULL);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_INIT, 0);
      return ER_CSS_PTHREAD_MUTEX_INIT;
    }

  r = pthread_cond_init (&entry_p->wakeup_cond, NULL);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_INIT, 0);
      return ER_CSS_PTHREAD_COND_INIT;
    }

  entry_p->resume_status = THREAD_RESUME_NONE;
  entry_p->er_Msg = NULL;
  entry_p->victim_request_fail = false;
  entry_p->next_wait_thrd = NULL;

  entry_p->lockwait = NULL;
  entry_p->lockwait_state = -1;
  entry_p->query_entry = NULL;
  entry_p->tran_next_wait = NULL;

  entry_p->check_interrupt = true;
  entry_p->check_page_validation = true;
  entry_p->type = TT_WORKER;	/* init */

  entry_p->private_heap_id = db_create_private_heap ();

  if (entry_p->private_heap_id == 0)
    {
      return ER_CSS_ALLOC;
    }

  entry_p->log_zip_undo = NULL;
  entry_p->log_zip_redo = NULL;
  entry_p->log_data_length = 0;
  entry_p->log_data_ptr = NULL;

  thread_clear_recursion_depth (entry_p);

  memset (&(entry_p->event_stats), 0, sizeof (EVENT_STAT));

  entry_p->on_trace = false;
  entry_p->clear_trace = false;
  entry_p->sort_stats_active = false;

  return NO_ERROR;
}

/*
 * thread_finalize_entry() -
 *   return:
 *   entry_p(in):
 */
static int
thread_finalize_entry (THREAD_ENTRY * entry_p)
{
  int r, i, error = NO_ERROR;

  entry_p->index = -1;
  entry_p->tid = ((pthread_t) 0);
  entry_p->client_id = -1;
  entry_p->tran_index = -1;
  entry_p->rid = 0;
  entry_p->status = TS_DEAD;
  entry_p->interrupted = false;
  entry_p->shutdown = false;

  for (i = 0; i < 3; i++)
    {
      if (entry_p->cnv_adj_buffer[i] != NULL)
	{
	  adj_ar_free (entry_p->cnv_adj_buffer[i]);
	  entry_p->cnv_adj_buffer[i] = NULL;
	}
    }

  entry_p->conn_entry = NULL;

  r = pthread_mutex_destroy (&entry_p->tran_index_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_DESTROY, 0);
      error = ER_CSS_PTHREAD_MUTEX_DESTROY;
    }
  r = pthread_mutex_destroy (&entry_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_DESTROY, 0);
      error = ER_CSS_PTHREAD_MUTEX_DESTROY;
    }
  r = pthread_cond_destroy (&entry_p->wakeup_cond);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_DESTROY, 0);
      error = ER_CSS_PTHREAD_COND_DESTROY;
    }
  entry_p->resume_status = THREAD_RESUME_NONE;

  entry_p->check_interrupt = true;

  if (entry_p->log_zip_undo)
    {
      log_zip_free (entry_p->log_zip_undo);
      entry_p->log_zip_undo = NULL;
    }
  if (entry_p->log_zip_redo)
    {
      log_zip_free (entry_p->log_zip_redo);
      entry_p->log_zip_redo = NULL;
    }
  if (entry_p->log_data_ptr)
    {
      free_and_init (entry_p->log_data_ptr);
      entry_p->log_data_length = 0;
    }

  db_destroy_private_heap (entry_p, entry_p->private_heap_id);

  return error;
}

/*
 * thread_print_entry_info() -
 *   return: void
 *   thread_p(in):
 */
void
thread_print_entry_info (THREAD_ENTRY * thread_p)
{
  fprintf (stderr,
	   "THREAD_ENTRY(tid(%ld),client_id(%d),tran_index(%d),rid(%d),status(%d))\n",
	   thread_p->tid, thread_p->client_id, thread_p->tran_index,
	   thread_p->rid, thread_p->status);

  if (thread_p->conn_entry != NULL)
    {
      css_print_conn_entry_info (thread_p->conn_entry);
    }

  fflush (stderr);
}

/*
 * Thread entry related functions
 * Information retrieval modules.
 * Inter thread synchronization modules.
 */

/*
 * thread_find_entry_by_tran_index_except_me() -
 *   return:
 *   tran_index(in):
 */
THREAD_ENTRY *
thread_find_entry_by_tran_index_except_me (int tran_index)
{
  THREAD_ENTRY *thread_p;
  int i;
  pthread_t me = pthread_self ();

  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];
      if (thread_p->tran_index == tran_index && thread_p->tid != me)
	{
	  return thread_p;
	}
    }

  return NULL;
}

/*
 * thread_find_entry_by_tran_index() -
 *   return:
 *   tran_index(in):
 */
static THREAD_ENTRY *
thread_find_entry_by_tran_index (int tran_index)
{
  THREAD_ENTRY *thread_p;
  int i;

  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];
      if (thread_p->tran_index == tran_index)
	{
	  return thread_p;
	}
    }

  return NULL;
}

/*
 * thread_get_current_entry_index() -
 *   return:
 */
int
thread_get_current_entry_index (void)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  return thread_p->index;
}

/*
 * thread_get_current_session_id () - get session id for current thread
 *   return: session id
 */
SESSION_ID
thread_get_current_session_id (void)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  if (thread_p->conn_entry == NULL)
    {
      return 0;
    }
  return thread_p->conn_entry->session_id;
}

/*
 * thread_get_current_tran_index() - get transaction index if current
 *                                       thread
 *   return:
 */
int
thread_get_current_tran_index (void)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  return thread_p->tran_index;
}

/*
 * thread_set_current_tran_index() -
 *   return: void
 *   tran_index(in):
 */
void
thread_set_current_tran_index (THREAD_ENTRY * thread_p, int tran_index)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  assert (thread_p != NULL);

  thread_p->tran_index = tran_index;
}

#if defined (ENABLE_UNUSED_FUNCTION)
void
thread_set_tran_index (THREAD_ENTRY * thread_p, int tran_index)
{
  if (thread_p == NULL)
    {
      thread_set_current_tran_index (thread_p, tran_index);
    }
  else
    {
      thread_p->tran_index = tran_index;
    }
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * thread_get_current_conn_entry() -
 *   return:
 */
CSS_CONN_ENTRY *
thread_get_current_conn_entry (void)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  return thread_p->conn_entry;
}

/*
 * thread_lock_entry() -
 *   return:
 *   thread_p(in):
 */
int
thread_lock_entry (THREAD_ENTRY * thread_p)
{
  int r;

  assert (thread_p != NULL);

  r = pthread_mutex_lock (&thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  return r;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * thread_lock_entry_with_tran_index() -
 *   return:
 *   tran_index(in):
 */
int
thread_lock_entry_with_tran_index (int tran_index)
{
  int r;
  THREAD_ENTRY *thread_p;

  thread_p = thread_find_entry_by_tran_index (tran_index);
  assert (thread_p != NULL);

  r = pthread_mutex_lock (&thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  return r;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * thread_unlock_entry() -
 *   return:
 *   thread_p(in):
 */
int
thread_unlock_entry (THREAD_ENTRY * thread_p)
{
  int r;

  assert (thread_p != NULL);

  r = pthread_mutex_unlock (&thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  return r;
}

/*
 * thread_suspend_wakeup_and_unlock_entry() -
 *   return:
 *   thread_p(in):
 *   suspended_reason(in):
 *
 * Note: this function must be called by current thread
 *       also, the lock must have already been acquired.
 */
int
thread_suspend_wakeup_and_unlock_entry (THREAD_ENTRY * thread_p,
					int suspended_reason)
{
  int r;
  int old_status;
  struct timeval start, end;

  assert (thread_p->status == TS_RUN || thread_p->status == TS_CHECK);
  old_status = thread_p->status;
  thread_p->status = TS_WAIT;

  thread_p->resume_status = suspended_reason;

  if (thread_p->event_stats.trace_slow_query == true)
    {
      gettimeofday (&start, NULL);
    }

  r = pthread_cond_wait (&thread_p->wakeup_cond, &thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_WAIT, 0);
      return ER_CSS_PTHREAD_COND_WAIT;
    }

  if (thread_p->event_stats.trace_slow_query == true)
    {
      gettimeofday (&end, NULL);
      if (suspended_reason == THREAD_LOCK_SUSPENDED)
	{
	  ADD_TIMEVAL (thread_p->event_stats.lock_waits, start, end);
	}
      else if (suspended_reason == THREAD_PGBUF_SUSPENDED)
	{
	  ADD_TIMEVAL (thread_p->event_stats.latch_waits, start, end);
	}
    }

  thread_p->status = old_status;

  r = pthread_mutex_unlock (&thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  return NO_ERROR;
}

/*
 * thread_suspend_timeout_wakeup_and_unlock_entry() -
 *   return:
 *   thread_p(in):
 *   time_p(in):
 *   suspended_reason(in):
 */
int
thread_suspend_timeout_wakeup_and_unlock_entry (THREAD_ENTRY * thread_p,
						struct timespec *time_p,
						int suspended_reason)
{
  int r;
  int old_status;
  int error = NO_ERROR;

  assert (thread_p->status == TS_RUN || thread_p->status == TS_CHECK);
  old_status = thread_p->status;
  thread_p->status = TS_WAIT;

  thread_p->resume_status = suspended_reason;

  r =
    pthread_cond_timedwait (&thread_p->wakeup_cond, &thread_p->th_entry_lock,
			    time_p);

  if (r != 0 && r != ETIMEDOUT)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_TIMEDWAIT, 0);
      return ER_CSS_PTHREAD_COND_TIMEDWAIT;
    }

  if (r == ETIMEDOUT)
    {
      error = ER_CSS_PTHREAD_COND_TIMEDOUT;
    }

  thread_p->status = old_status;

  r = pthread_mutex_unlock (&thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  return error;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * thread_suspend_wakeup_and_unlock_entry_with_tran_index() -
 *   return: 0 if no error, or error code
 *   tran_index(in):
 */
int
thread_suspend_wakeup_and_unlock_entry_with_tran_index (int tran_index,
							int suspended_reason)
{
  THREAD_ENTRY *thread_p;
  int r;
  int old_status;

  thread_p = thread_find_entry_by_tran_index (tran_index);
  if (thread_p == NULL)
    {
      return ER_FAILED;
    }

  /*
   * this function must be called by current thread
   * also, the lock must have already been acquired before.
   */
  assert (thread_p->status == TS_RUN || thread_p->status == TS_CHECK);
  old_status = thread_p->status;
  thread_p->status = TS_WAIT;

  thread_p->resume_status = suspended_reason;

  r = pthread_cond_wait (&thread_p->wakeup_cond, &thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_WAIT, 0);
      return ER_CSS_PTHREAD_COND_WAIT;
    }

  thread_p->status = old_status;

  r = pthread_mutex_unlock (&thread_p->th_entry_lock);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  return NO_ERROR;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * thread_wakeup_internal () -
 *   return:
 *   thread_p(in/out):
 *   resume_reason:
 */
static int
thread_wakeup_internal (THREAD_ENTRY * thread_p, int resume_reason,
			bool had_mutex)
{
  int r = NO_ERROR;

  if (had_mutex == false)
    {
      r = thread_lock_entry (thread_p);
      if (r != 0)
	{
	  return r;
	}
    }

  r = pthread_cond_signal (&thread_p->wakeup_cond);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_SIGNAL, 0);
      if (had_mutex == false)
	{
	  thread_unlock_entry (thread_p);
	}
      return ER_CSS_PTHREAD_COND_SIGNAL;
    }

  thread_p->resume_status = resume_reason;

  if (had_mutex == false)
    {
      r = thread_unlock_entry (thread_p);
    }

  return r;
}

/*
 * thread_check_suspend_reason_and_wakeup_internal () -
 *   return:
 *   thread_p(in):
 *   resume_reason:
 *   suspend_reason:
 *   had_mutex:
 */
static int
thread_check_suspend_reason_and_wakeup_internal (THREAD_ENTRY * thread_p,
						 int resume_reason,
						 int suspend_reason,
						 bool had_mutex)
{
  int r = NO_ERROR;

  if (had_mutex == false)
    {
      r = thread_lock_entry (thread_p);
      if (r != 0)
	{
	  return r;
	}
    }

  if (thread_p->resume_status != suspend_reason)
    {
      r = thread_unlock_entry (thread_p);
      return (r == NO_ERROR) ? ER_FAILED : r;
    }

  r = pthread_cond_signal (&thread_p->wakeup_cond);
  if (r != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_SIGNAL, 0);
      thread_unlock_entry (thread_p);
      return ER_CSS_PTHREAD_COND_SIGNAL;
    }

  thread_p->resume_status = resume_reason;

  r = thread_unlock_entry (thread_p);

  return r;
}


/*
 * thread_wakeup () -
 *   return:
 *   thread_p(in/out):
 *   resume_reason:
 */
int
thread_wakeup (THREAD_ENTRY * thread_p, int resume_reason)
{
  return thread_wakeup_internal (thread_p, resume_reason, false);
}

int
thread_check_suspend_reason_and_wakeup (THREAD_ENTRY * thread_p,
					int resume_reason, int suspend_reason)
{
  return thread_check_suspend_reason_and_wakeup_internal (thread_p,
							  resume_reason,
							  suspend_reason,
							  false);
}

/*
 * thread_wakeup_already_had_mutex () -
 *   return:
 *   thread_p(in/out):
 *   resume_reason:
 */
int
thread_wakeup_already_had_mutex (THREAD_ENTRY * thread_p, int resume_reason)
{
  return thread_wakeup_internal (thread_p, resume_reason, true);
}

/*
 * thread_wakeup_with_tran_index() -
 *   return:
 *   tran_index(in):
 */
int
thread_wakeup_with_tran_index (int tran_index, int resume_reason)
{
  THREAD_ENTRY *thread_p;
  int r = NO_ERROR;

  thread_p = thread_find_entry_by_tran_index_except_me (tran_index);
  if (thread_p == NULL)
    {
      return r;
    }

  r = thread_wakeup (thread_p, resume_reason);

  return r;
}

/*
 * thread_waiting_for_function() - wait until func return TRUE.
 *   return: void
 *   func(in) : a pointer to a function that will return a non-zero value when
 *	        the thread should resume execution.
 *   arg(in)  : an integer argument to be passed to func.
 *
 * Note: The thread is blocked for execution until func returns a non-zero
 *       value. Halts exection of the currently running thread.
 */
void
thread_waiting_for_function (THREAD_ENTRY * thread_p, CSS_THREAD_FN func,
			     CSS_THREAD_ARG arg)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  while ((*func) (thread_p, arg) == false && thread_p->interrupted != true
	 && thread_p->shutdown != true)
    {
      thread_sleep (10);	/* 10 msec */
    }
}

/*
 * thread_suspend_with_other_mutex() -
 *   return: 0 if no error, or error code
 *   thread_p(in):
 *   mutex_p():
 *   timeout(in):
 *   to(in):
 *   suspended_reason(in):
 */
int
thread_suspend_with_other_mutex (THREAD_ENTRY * thread_p,
				 pthread_mutex_t * mutex_p, int timeout,
				 struct timespec *to, int suspended_reason)
{
  int r;
  int old_status;
  int error = NO_ERROR;

  assert (thread_p != NULL);
  old_status = thread_p->status;

  r = pthread_mutex_lock (&thread_p->th_entry_lock);
  if (r != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  thread_p->status = TS_WAIT;
  thread_p->resume_status = suspended_reason;

  r = pthread_mutex_unlock (&thread_p->th_entry_lock);
  if (r != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  if (timeout == INF_WAIT)
    {
      r = pthread_cond_wait (&thread_p->wakeup_cond, mutex_p);
    }
  else
    {
      r = pthread_cond_timedwait (&thread_p->wakeup_cond, mutex_p, to);
    }

  /* we should restore thread's status */
  if (r != NO_ERROR)
    {
      error = (r == ETIMEDOUT) ?
	ER_CSS_PTHREAD_COND_TIMEDOUT : ER_CSS_PTHREAD_COND_WAIT;
      if (timeout == INF_WAIT || r != ETIMEDOUT)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE, error, 0);
	}
    }

  r = pthread_mutex_lock (&thread_p->th_entry_lock);
  if (r != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_LOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_LOCK;
    }

  thread_p->status = old_status;

  r = pthread_mutex_unlock (&thread_p->th_entry_lock);
  if (r != NO_ERROR)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_MUTEX_UNLOCK, 0);
      return ER_CSS_PTHREAD_MUTEX_UNLOCK;
    }

  return error;
}

/*
 * thread_sleep() - Halts the currently running thread for <milliseconds>
 *   return: void
 *   milliseconds(in): The number of milliseconds for the thread to sleep
 *
 *  Note: Used to temporarly halt the current process.
 */
void
thread_sleep (double milliseconds)
{
#if defined(WINDOWS)
  Sleep ((int) milliseconds);
#else /* WINDOWS */
  struct timeval to;

  to.tv_sec = (int) (milliseconds / 1000);
  to.tv_usec = ((int) (milliseconds * 1000)) % 1000000;

  select (0, NULL, NULL, NULL, &to);
#endif /* WINDOWS */
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * thread_exit() - The program will exit.
 *   return: void
 *   exit_id(in): an integer argument to be returned as the exit value.
 */
void
thread_exit (int exit_id)
{
  UINTPTR thread_exit_id = exit_id;

  THREAD_EXIT (thread_exit_id);
}
#endif

/*
 * thread_get_client_id() - returns the unique client identifier
 *   return: returns the unique client identifier, on error, returns -1
 *
 * Note: WARN: this function doesn't lock on thread_entry
 */
int
thread_get_client_id (THREAD_ENTRY * thread_p)
{
  CSS_CONN_ENTRY *conn_p;

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  assert (thread_p != NULL);

  conn_p = thread_p->conn_entry;
  if (conn_p != NULL)
    {
      return conn_p->client_id;
    }
  else
    {
      return -1;
    }
}

/*
 * thread_get_comm_request_id() - returns the request id that started the current thread
 *   return: returns the comm system request id for the client request that
 *           started the thread. On error, returns -1
 *
 * Note: WARN: this function doesn't lock on thread_entry
 */
unsigned int
thread_get_comm_request_id (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  assert (thread_p != NULL);

  return thread_p->rid;
}

#if defined (ENABLE_UNUSED_FUNCTION)
/*
 * thread_set_comm_request_id() - sets the comm system request id to the client request
  *                     that started the thread
 *   return: void
 *   request_id(in): the comm request id to save for thread_get_comm_request_id
 *
 * Note: WARN: this function doesn't lock on thread_entry
 */
void
thread_set_comm_request_id (unsigned int request_id)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  thread_p->rid = request_id;
}
#endif /* ENABLE_UNUSED_FUNCTION */

/*
 * thread_has_threads() - check if any thread is processing job of transaction
 *                          tran_index
 *   return:
 *   tran_index(in):
 *   client_id(in):
 *
 * Note: WARN: this function doesn't lock thread_mgr
 */
int
thread_has_threads (THREAD_ENTRY * caller, int tran_index, int client_id)
{
  int i, n, rv;
  THREAD_ENTRY *thread_p;
  CSS_CONN_ENTRY *conn_p;

  for (i = 1, n = 0; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];
      if (thread_p == caller)
	{
	  continue;
	}
      rv = pthread_mutex_lock (&thread_p->tran_index_lock);
      if (thread_p->tid != pthread_self () && thread_p->status != TS_DEAD
	  && thread_p->status != TS_FREE && thread_p->status != TS_CHECK)
	{
	  conn_p = thread_p->conn_entry;
	  if (tran_index == NULL_TRAN_INDEX
	      && (conn_p != NULL && conn_p->client_id == client_id))
	    {
	      n++;
	    }
	  else if (tran_index == thread_p->tran_index
		   && (conn_p == NULL || conn_p->client_id == client_id))
	    {
	      n++;
	    }
	}
      pthread_mutex_unlock (&thread_p->tran_index_lock);
    }

  return n;
}

/*
 * thread_get_info_threads() - get statistics of threads
 *   return: void
 *   num_total_threads(out):
 *   num_worker_threads(out):
 *   num_free_threads(out):
 *   num_suspended_threads(out):
 *
 * Note: Find the number of threads, number of suspended threads, and maximum
 *       of threads that can be created.
 *       WARN: this function doesn't lock threadmgr
 */
void
thread_get_info_threads (int *num_total_threads, int *num_worker_threads,
			 int *num_free_threads, int *num_suspended_threads)
{
  THREAD_ENTRY *thread_p;
  int i;

  if (num_total_threads)
    {
      *num_total_threads = thread_Manager.num_total;
    }
  if (num_worker_threads)
    {
      *num_worker_threads = thread_Manager.num_workers;
    }
  if (num_free_threads)
    {
      *num_free_threads = 0;
    }
  if (num_suspended_threads)
    {
      *num_suspended_threads = 0;
    }
  if (num_free_threads || num_suspended_threads)
    {
      for (i = 1; i <= thread_Manager.num_workers; i++)
	{
	  thread_p = &thread_Manager.thread_array[i];
	  if (num_free_threads && thread_p->status == TS_FREE)
	    {
	      (*num_free_threads)++;
	    }
	  if (num_suspended_threads && thread_p->status == TS_WAIT)
	    {
	      (*num_suspended_threads)++;
	    }
	}
    }
}

int
thread_num_worker_threads (void)
{
  return thread_Manager.num_workers;
}

int
thread_num_total_threads (void)
{
  return thread_Manager.num_total;
}

/*
 * thread_dump_threads() - dump all thread
 *   return: void
 *
 * Note: for debug
 *       WARN: this function doesn't lock threadmgr
 */
void
thread_dump_threads (void)
{
  const char *status[] = {
    "dead", "free", "run", "wait", "check"
  };
  THREAD_ENTRY *thread_p;
  int i;

  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &thread_Manager.thread_array[i];

      fprintf (stderr,
	       "thread %d(tid(%ld),client_id(%d),tran_index(%d),"
	       "rid(%d),status(%s),interrupt(%d))\n",
	       thread_p->index, thread_p->tid, thread_p->client_id,
	       thread_p->tran_index, thread_p->rid,
	       status[thread_p->status], thread_p->interrupted);
    }

  fflush (stderr);
}

/*
 * css_get_private_heap () -
 *   return:
 *   thread_p(in):
 */
HL_HEAPID
css_get_private_heap (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  assert (thread_p != NULL);

  return thread_p->private_heap_id;
}

/*
 * css_set_private_heap() -
 *   return:
 *   thread_p(in):
 *   heap_id(in):
 */
HL_HEAPID
css_set_private_heap (THREAD_ENTRY * thread_p, HL_HEAPID heap_id)
{
  HL_HEAPID old_heap_id = 0;

  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  assert (thread_p != NULL);

  old_heap_id = thread_p->private_heap_id;
  thread_p->private_heap_id = heap_id;

  return old_heap_id;
}

/*
 * css_get_cnv_adj_buffer() -
 *   return:
 *   idx(in):
 */
ADJ_ARRAY *
css_get_cnv_adj_buffer (int idx)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  return thread_p->cnv_adj_buffer[idx];
}

/*
 * css_set_cnv_adj_buffer() -
 *   return: void
 *   idx(in):
 *   buffer_p(in):
 */
void
css_set_cnv_adj_buffer (int idx, ADJ_ARRAY * buffer_p)
{
  THREAD_ENTRY *thread_p;

  thread_p = thread_get_thread_entry_info ();
  assert (thread_p != NULL);

  thread_p->cnv_adj_buffer[idx] = buffer_p;
}

/*
 * thread_set_check_interrupt() -
 *   return:
 *   flag(in):
 */
bool
thread_set_check_interrupt (THREAD_ENTRY * thread_p, bool flag)
{
  bool old_val = true;

  if (BO_IS_SERVER_RESTARTED ())
    {
      if (thread_p == NULL)
	{
	  thread_p = thread_get_thread_entry_info ();
	}

      old_val = thread_p->check_interrupt;
      thread_p->check_interrupt = flag;
    }

  return old_val;
}

/*
 * thread_get_check_interrupt() -
 *   return:
 */
bool
thread_get_check_interrupt (THREAD_ENTRY * thread_p)
{
  bool ret_val = true;

  if (BO_IS_SERVER_RESTARTED ())
    {
      if (thread_p == NULL)
	{
	  thread_p = thread_get_thread_entry_info ();
	}

      ret_val = thread_p->check_interrupt;
    }

  return ret_val;
}

/*
 * thread_set_check_page_validation() -
 *   return:
 *   flag(in):
 */
bool
thread_set_check_page_validation (THREAD_ENTRY * thread_p, bool flag)
{
  bool old_val = true;

  if (BO_IS_SERVER_RESTARTED ())
    {
      if (thread_p == NULL)
	{
	  thread_p = thread_get_thread_entry_info ();
	}

      old_val = thread_p->check_page_validation;
      thread_p->check_page_validation = flag;
    }

  return old_val;
}

/*
 * thread_get_check_page_validation() -
 *   return:
 */
bool
thread_get_check_page_validation (THREAD_ENTRY * thread_p)
{
  bool ret_val = true;

  if (BO_IS_SERVER_RESTARTED ())
    {
      if (thread_p == NULL)
	{
	  thread_p = thread_get_thread_entry_info ();
	}

      ret_val = thread_p->check_page_validation;
    }

  return ret_val;
}

/*
 * thread_worker() - Dequeue request from job queue and then call handler
 *                       function
 *   return:
 *   arg_p(in):
 */
THREAD_RET_T THREAD_CALLING_CONVENTION
thread_worker (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  CSS_JOB_ENTRY *job_entry_p;
  CSS_THREAD_FN handler_func;
  CSS_THREAD_ARG handler_func_arg;
  int rv;

  tsd_ptr = (THREAD_ENTRY *) arg_p;

  /* wait until THREAD_CREATE() finish */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_WORKER;	/* not defined yet */
  tsd_ptr->status = TS_FREE;	/* set thread stat as free */

  /* during server is active */
  while (!tsd_ptr->shutdown)
    {
      er_stack_clearall ();
      er_clear ();

      job_entry_p = css_get_new_job ();	/* get new job entry */
      if (job_entry_p == NULL)
	{
	  /* if there was no job to process */
	  pthread_mutex_unlock (&tsd_ptr->tran_index_lock);
	  continue;
	}

#ifdef _TRACE_THREADS_
      CSS_TRACE4 ("processing job_entry(%p, %p, %p)\n",
		  job_entry_p->conn_entry, job_entry_p->func,
		  job_entry_p->arg);
#endif /* _TRACE_THREADS_ */

      /* set tsd_ptr information */
      tsd_ptr->conn_entry = job_entry_p->conn_entry;

      tsd_ptr->status = TS_RUN;	/* set thread status as running */

      handler_func = job_entry_p->func;
      handler_func_arg = job_entry_p->arg;

      css_free_job_entry (job_entry_p);

      handler_func (tsd_ptr, handler_func_arg);	/* invoke request handler */

      /* reset tsd_ptr information */
      tsd_ptr->conn_entry = NULL;
      tsd_ptr->status = TS_FREE;
      rv = pthread_mutex_lock (&tsd_ptr->tran_index_lock);
      tsd_ptr->tran_index = -1;
      pthread_mutex_unlock (&tsd_ptr->tran_index_lock);
      tsd_ptr->check_interrupt = true;

      memset (&(tsd_ptr->event_stats), 0, sizeof (EVENT_STAT));
      tsd_ptr->on_trace = false;
    }

  er_final (0);

  tsd_ptr->conn_entry = NULL;
  tsd_ptr->tran_index = -1;
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/* Special Purpose Threads
   deadlock detector, check point daemon */

#if defined(WINDOWS)
/*
 * css_initialize_sync_object() -
 *   return:
 */
static int
css_initialize_sync_object (void)
{
  int r, i;

  r = NO_ERROR;

  for (i = 0; i < DIM (thread_Daemons); i++)
    {
      r = pthread_cond_init (&thread_Daemons[i].daemon_monitor->cond, NULL);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_COND_INIT, 0);
	  return ER_CSS_PTHREAD_COND_INIT;
	}
      r = pthread_mutex_init (&thread_Daemons[i].daemon_monitor->lock, NULL);
      if (r != 0)
	{
	  er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			       ER_CSS_PTHREAD_MUTEX_INIT, 0);
	  return ER_CSS_PTHREAD_MUTEX_INIT;
	}
    }

  return r;
}
#endif /* WINDOWS */

/*
 * thread_deadlock_detect_thread() -
 *   return:
 */
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_deadlock_detect_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv;
  THREAD_ENTRY *thread_p;
  int thrd_index;
  bool state;
  int lockwait_count;

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finish */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Deadlock_detect_thread.is_available = true;
  thread_Deadlock_detect_thread.is_running = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  /* during server is active */
  while (!tsd_ptr->shutdown)
    {
      thread_sleep (100);	/* 100 msec */
      if (!lock_check_local_deadlock_detection ())
	{
	  continue;
	}

      er_clear ();

      /* check if the lock-wait thread exists */
      thread_p = thread_find_first_lockwait_entry (&thrd_index);
      if (thread_p == (THREAD_ENTRY *) NULL)
	{
	  /* none is lock-waiting */
	  rv = pthread_mutex_lock (&thread_Deadlock_detect_thread.lock);
	  thread_Deadlock_detect_thread.is_running = false;

	  if (tsd_ptr->shutdown)
	    {
	      pthread_mutex_unlock (&thread_Deadlock_detect_thread.lock);
	      break;
	    }
	  pthread_cond_wait (&thread_Deadlock_detect_thread.cond,
			     &thread_Deadlock_detect_thread.lock);

	  thread_Deadlock_detect_thread.is_running = true;

	  pthread_mutex_unlock (&thread_Deadlock_detect_thread.lock);
	  continue;
	}

      /* One or more threads are lock-waiting */
      lockwait_count = 0;
      while (thread_p != (THREAD_ENTRY *) NULL)
	{
	  /*
	   * The transaction, for which the current thread is working,
	   * might be interrupted. The interrupt checking is also performed
	   * within lock_force_timeout_expired_wait_transactions().
	   */
	  state = lock_force_timeout_expired_wait_transactions (thread_p);
	  if (state == false)
	    {
	      lockwait_count++;
	    }
	  thread_p = thread_find_next_lockwait_entry (&thrd_index);
	}

      if (lockwait_count >= 2)
	{
	  (void) lock_detect_local_deadlock (tsd_ptr);
	}
    }

  rv = pthread_mutex_lock (&thread_Deadlock_detect_thread.lock);
  thread_Deadlock_detect_thread.is_running = false;
  thread_Deadlock_detect_thread.is_available = false;
  pthread_mutex_unlock (&thread_Deadlock_detect_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/*
 * thread_wakeup_deadlock_detect_thread() -
 *   return:
 */
void
thread_wakeup_deadlock_detect_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Deadlock_detect_thread.lock);
  if (thread_Deadlock_detect_thread.is_running == false)
    {
      pthread_cond_signal (&thread_Deadlock_detect_thread.cond);
    }
  pthread_mutex_unlock (&thread_Deadlock_detect_thread.lock);
}

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_session_control_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr = NULL;
#endif /* !HPUX */
  struct timeval timeout;
  struct timespec to = {
    0, 0
  };
  int rv = 0;

  tsd_ptr = (THREAD_ENTRY *) arg_p;

  /* wait until THREAD_CREATE() finishes */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */
  thread_Session_control_thread.is_available = true;
  thread_Session_control_thread.is_running = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  while (!tsd_ptr->shutdown)
    {
      er_clear ();

      gettimeofday (&timeout, NULL);
      to.tv_sec = timeout.tv_sec + 60;

      rv = pthread_mutex_lock (&thread_Session_control_thread.lock);
      pthread_cond_timedwait (&thread_Session_control_thread.cond,
			      &thread_Session_control_thread.lock, &to);
      pthread_mutex_unlock (&thread_Session_control_thread.lock);

      if (tsd_ptr->shutdown)
	{
	  break;
	}

      session_remove_expired_sessions (&timeout);
    }
  rv = pthread_mutex_lock (&thread_Session_control_thread.lock);
  thread_Session_control_thread.is_available = false;
  thread_Session_control_thread.is_running = false;
  pthread_mutex_unlock (&thread_Session_control_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * thread_wakeup_session_control_thread() -
 *   return:
 */
void
thread_wakeup_session_control_thread (void)
{
  pthread_mutex_lock (&thread_Session_control_thread.lock);
  pthread_cond_signal (&thread_Session_control_thread.cond);
  pthread_mutex_unlock (&thread_Session_control_thread.lock);
}
#endif

/*
 * css_checkpoint_thread() -
 *   return:
 *   arg_p(in):
 */

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_checkpoint_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv;

  struct timespec to = {
    0, 0
  };

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finish */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */
  thread_Checkpoint_thread.is_available = true;
  thread_Checkpoint_thread.is_running = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  /* during server is active */
  while (!tsd_ptr->shutdown)
    {
      er_clear ();

      to.tv_sec =
	time (NULL) +
	prm_get_integer_value (PRM_ID_LOG_CHECKPOINT_INTERVAL_SECS);

      rv = pthread_mutex_lock (&thread_Checkpoint_thread.lock);
      pthread_cond_timedwait (&thread_Checkpoint_thread.cond,
			      &thread_Checkpoint_thread.lock, &to);
      pthread_mutex_unlock (&thread_Checkpoint_thread.lock);
      if (tsd_ptr->shutdown)
	{
	  break;
	}

      logpb_checkpoint (tsd_ptr);
    }

  rv = pthread_mutex_lock (&thread_Checkpoint_thread.lock);
  thread_Checkpoint_thread.is_available = false;
  thread_Checkpoint_thread.is_running = false;
  pthread_mutex_unlock (&thread_Checkpoint_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/*
 * thread_wakeup_checkpoint_thread() -
 *   return:
 */
void
thread_wakeup_checkpoint_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Checkpoint_thread.lock);
  pthread_cond_signal (&thread_Checkpoint_thread.cond);
  pthread_mutex_unlock (&thread_Checkpoint_thread.lock);
}

/*
 * css_purge_archive_logs_thread() -
 *   return:
 *   arg_p(in):
 */

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_purge_archive_logs_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv;
  time_t cur_time, last_deleted_time = 0;
  struct timespec to = {
    0, 0
  };

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finish */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Purge_archive_logs_thread.is_available = true;
  thread_Purge_archive_logs_thread.is_running = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  /* during server is active */
  while (!tsd_ptr->shutdown)
    {
      er_clear ();

      if (prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL) > 0)
	{
	  to.tv_sec = time (NULL);
	  if (to.tv_sec >
	      last_deleted_time +
	      prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL))
	    {
	      to.tv_sec +=
		prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL);
	    }
	  else
	    {
	      to.tv_sec =
		last_deleted_time +
		prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL);
	    }
	}

      rv = pthread_mutex_lock (&thread_Purge_archive_logs_thread.lock);
      if (prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL) > 0)
	{
	  pthread_cond_timedwait (&thread_Purge_archive_logs_thread.cond,
				  &thread_Purge_archive_logs_thread.lock,
				  &to);
	}
      else
	{
	  pthread_cond_wait (&thread_Purge_archive_logs_thread.cond,
			     &thread_Purge_archive_logs_thread.lock);
	}
      pthread_mutex_unlock (&thread_Purge_archive_logs_thread.lock);
      if (tsd_ptr->shutdown)
	{
	  break;
	}

      if (prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL) > 0)
	{
	  cur_time = time (NULL);
	  if (cur_time - last_deleted_time <
	      prm_get_integer_value (PRM_ID_REMOVE_LOG_ARCHIVES_INTERVAL))
	    {
	      /* do not delete logs. wait more time */
	      continue;
	    }
	  /* remove a log */
	  if (logpb_remove_archive_logs_exceed_limit (tsd_ptr, 1) > 0)
	    {
	      /* A log was deleted */
	      last_deleted_time = time (NULL);
	    }
	}
      else
	{
	  /* remove all unnecessary logs */
	  logpb_remove_archive_logs_exceed_limit (tsd_ptr, 0);
	}

    }
  rv = pthread_mutex_lock (&thread_Purge_archive_logs_thread.lock);
  thread_Purge_archive_logs_thread.is_available = false;
  thread_Purge_archive_logs_thread.is_running = false;
  pthread_mutex_unlock (&thread_Purge_archive_logs_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/*
 * thread_wakeup_purge_archive_logs_thread() -
 *   return:
 */
void
thread_wakeup_purge_archive_logs_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Purge_archive_logs_thread.lock);
  pthread_cond_signal (&thread_Purge_archive_logs_thread.cond);
  pthread_mutex_unlock (&thread_Purge_archive_logs_thread.lock);
}

/*
 * thread_wakeup_oob_handler_thread() -
 *  return:
 */
void
thread_wakeup_oob_handler_thread (void)
{
#if !defined(WINDOWS)
  THREAD_ENTRY *thread_p;

  thread_p = &thread_Manager.thread_array[thread_Oob_thread.thread_index];
  pthread_kill (thread_p->tid, SIGURG);
#endif /* !WINDOWS */
}

/*
 * thread_check_ha_delay_info_thread() -
 *   return:
 *   arg_p(in):
 */

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_check_ha_delay_info_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  struct timeval cur_time = {
    0, 0
  };

  struct timespec wakeup_time = {
    0, 0
  };

  int rv;
  INT64 tmp_usec;
  int wakeup_interval = 1000;
#if !defined(WINDOWS)
  time_t log_record_time = 0;
  int error_code;
  int delay_limit_in_secs;
  int acceptable_delay_in_secs;
  int curr_delay_in_secs;
  HA_SERVER_STATE server_state;
  char buffer[LINE_MAX];
#endif

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finishes */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;	/* daemon thread */
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Check_ha_delay_info_thread.is_running = true;
  thread_Check_ha_delay_info_thread.is_available = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  while (!tsd_ptr->shutdown)
    {
      er_clear ();

      gettimeofday (&cur_time, NULL);
      wakeup_time.tv_sec = cur_time.tv_sec + (wakeup_interval / 1000);
      tmp_usec = cur_time.tv_usec + (wakeup_interval % 1000) * 1000;

      if (tmp_usec >= 1000000)
	{
	  wakeup_time.tv_sec += 1;
	  tmp_usec -= 1000000;
	}
      wakeup_time.tv_nsec = tmp_usec * 1000;

      rv = pthread_mutex_lock (&thread_Check_ha_delay_info_thread.lock);
      thread_Check_ha_delay_info_thread.is_running = false;

      do
	{
	  rv =
	    pthread_cond_timedwait (&thread_Check_ha_delay_info_thread.cond,
				    &thread_Check_ha_delay_info_thread.lock,
				    &wakeup_time);
	}
      while (rv == 0 && tsd_ptr->shutdown == false);

      thread_Check_ha_delay_info_thread.is_running = true;

      pthread_mutex_unlock (&thread_Check_ha_delay_info_thread.lock);

      if (tsd_ptr->shutdown == true)
	{
	  break;
	}

#if defined(WINDOWS)
      continue;
#else /* WINDOWS */

      /* do its job */
      csect_enter (tsd_ptr, CSECT_HA_SERVER_STATE, INF_WAIT);

      server_state = css_ha_server_state ();

      if (server_state == HA_SERVER_STATE_ACTIVE
	  || server_state == HA_SERVER_STATE_TO_BE_STANDBY)
	{
	  css_unset_ha_repl_delayed ();
	  mnt_x_ha_repl_delay (tsd_ptr, 0);

	  log_append_ha_server_state (tsd_ptr, server_state);
	  csect_exit (CSECT_HA_SERVER_STATE);
	}
      else
	{
	  csect_exit (CSECT_HA_SERVER_STATE);

	  delay_limit_in_secs =
	    prm_get_integer_value (PRM_ID_HA_DELAY_LIMIT_IN_SECS);
	  acceptable_delay_in_secs =
	    delay_limit_in_secs -
	    prm_get_integer_value (PRM_ID_HA_DELAY_LIMIT_DELTA_IN_SECS);

	  if (acceptable_delay_in_secs < 0)
	    {
	      acceptable_delay_in_secs = 0;
	    }

	  error_code =
	    catcls_get_apply_info_log_record_time (tsd_ptr, &log_record_time);

	  if (error_code == NO_ERROR && log_record_time > 0)
	    {
	      curr_delay_in_secs = time (NULL) - log_record_time;
	      if (curr_delay_in_secs > 0)
		{
		  curr_delay_in_secs -= HA_DELAY_ERR_CORRECTION;
		}

	      if (delay_limit_in_secs > 0)
		{
		  if (curr_delay_in_secs > delay_limit_in_secs)
		    {
		      if (!css_is_ha_repl_delayed ())
			{
			  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE,
				  ER_HA_REPL_DELAY_DETECTED, 2,
				  curr_delay_in_secs, delay_limit_in_secs);

			  css_set_ha_repl_delayed ();
			}
		    }
		  else if (curr_delay_in_secs <= acceptable_delay_in_secs)
		    {
		      if (css_is_ha_repl_delayed ())
			{
			  er_set (ER_NOTIFICATION_SEVERITY, ARG_FILE_LINE,
				  ER_HA_REPL_DELAY_RESOLVED, 2,
				  curr_delay_in_secs,
				  acceptable_delay_in_secs);

			  css_unset_ha_repl_delayed ();
			}
		    }
		}

	      mnt_x_ha_repl_delay (tsd_ptr, curr_delay_in_secs);
	    }
	}
#endif /* WINDOWS */
    }

  rv = pthread_mutex_lock (&thread_Check_ha_delay_info_thread.lock);
  thread_Check_ha_delay_info_thread.is_running = false;
  thread_Check_ha_delay_info_thread.is_available = false;
  pthread_mutex_unlock (&thread_Check_ha_delay_info_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/*
 * thread_page_flush_thread() -
 *   return:
 *   arg_p(in):
 */

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_page_flush_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv;
  struct timeval cur_time = {
    0, 0
  };

  struct timespec wakeup_time = {
    0, 0
  };
  int tmp_usec;
  int wakeup_interval;

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finishes */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;	/* daemon thread */
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Page_flush_thread.is_running = true;
  thread_Page_flush_thread.is_available = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  while (!tsd_ptr->shutdown)
    {
      er_clear ();

      wakeup_interval =
	prm_get_integer_value (PRM_ID_PAGE_BG_FLUSH_INTERVAL_MSEC);

      if (wakeup_interval > 0)
	{
	  gettimeofday (&cur_time, NULL);

	  wakeup_time.tv_sec = cur_time.tv_sec + (wakeup_interval / 1000);
	  tmp_usec = cur_time.tv_usec + (wakeup_interval % 1000) * 1000;
	  if (tmp_usec >= 1000000)
	    {
	      wakeup_time.tv_sec += 1;
	      tmp_usec -= 1000000;
	    }
	  wakeup_time.tv_nsec = tmp_usec * 1000;
	}

      rv = pthread_mutex_lock (&thread_Page_flush_thread.lock);
      thread_Page_flush_thread.is_running = false;

      if (wakeup_interval > 0)
	{
	  do
	    {
	      rv = pthread_cond_timedwait (&thread_Page_flush_thread.cond,
					   &thread_Page_flush_thread.lock,
					   &wakeup_time);
	    }
	  while (rv == 0);
	}
      else
	{
	  pthread_cond_wait (&thread_Page_flush_thread.cond,
			     &thread_Page_flush_thread.lock);
	}

      thread_Page_flush_thread.is_running = true;

      pthread_mutex_unlock (&thread_Page_flush_thread.lock);

      if (tsd_ptr->shutdown)
	{
	  break;
	}

      pgbuf_flush_victim_candidate (tsd_ptr,
				    prm_get_float_value
				    (PRM_ID_PB_BUFFER_FLUSH_RATIO));
    }

  rv = pthread_mutex_lock (&thread_Page_flush_thread.lock);
  thread_Page_flush_thread.is_running = false;
  thread_Page_flush_thread.is_available = false;
  pthread_mutex_unlock (&thread_Page_flush_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  thread_Page_flush_thread.is_running = false;

  return (THREAD_RET_T) 0;
}

/*
 * thread_wakeup_page_flush_thread() -
 *   return:
 */
void
thread_wakeup_page_flush_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Page_flush_thread.lock);
  if (!thread_Page_flush_thread.is_running)
    {
      pthread_cond_signal (&thread_Page_flush_thread.cond);
    }
  pthread_mutex_unlock (&thread_Page_flush_thread.lock);
}

/*
 * thread_is_page_flush_thread_available() -
 *   return:
 */
bool
thread_is_page_flush_thread_available (void)
{
  int rv;
  bool is_available;

  rv = pthread_mutex_lock (&thread_Page_flush_thread.lock);
  is_available = thread_Page_flush_thread.is_available;
  pthread_mutex_unlock (&thread_Page_flush_thread.lock);

  return is_available;
}

static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_flush_control_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv;

  struct timespec wakeup_time = {
    0, 0
  };

  struct timeval begin_tv, end_tv, diff_tv;
  INT64 diff_usec;
  int wakeup_interval_in_msec = 50;	/* 1 msec */

  int elapsed_usec = 0;
  int usec_consumed = 0;
  int usec_consumed_sum = 0;
  int token_gen = 0;
  int token_gen_sum = 0;
  int token_shared = 0;
  int token_consumed = 0;
  int token_consumed_sum = 0;

  tsd_ptr = (THREAD_ENTRY *) arg_p;

  /* wait until THREAD_CREATE() finishes */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;	/* daemon thread */
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Flush_control_thread.is_available = true;
  thread_Flush_control_thread.is_running = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  rv = fileio_flush_control_initialize ();
  if (rv != NO_ERROR)
    {
      goto error;
    }

  while (!tsd_ptr->shutdown)
    {
      INT64 tmp_usec;

      (void) gettimeofday (&begin_tv, NULL);
      er_clear ();

      wakeup_time.tv_sec =
	begin_tv.tv_sec + (wakeup_interval_in_msec / 1000LL);
      tmp_usec =
	begin_tv.tv_usec + (wakeup_interval_in_msec % 1000LL) * 1000LL;
      if (tmp_usec >= 1000000)
	{
	  wakeup_time.tv_sec += 1;
	  tmp_usec -= 1000000;
	}
      wakeup_time.tv_nsec = tmp_usec * 1000;

      rv = pthread_mutex_lock (&thread_Flush_control_thread.lock);
      thread_Flush_control_thread.is_running = false;

      pthread_cond_timedwait (&thread_Flush_control_thread.cond,
			      &thread_Flush_control_thread.lock,
			      &wakeup_time);

      thread_Flush_control_thread.is_running = true;

      pthread_mutex_unlock (&thread_Flush_control_thread.lock);

      if (tsd_ptr->shutdown)
	{
	  break;
	}

      (void) gettimeofday (&end_tv, NULL);
      DIFF_TIMEVAL (begin_tv, end_tv, diff_tv);
      diff_usec = diff_tv.tv_sec * 1000000LL + diff_tv.tv_usec;

      /* Do it's job */
      (void) fileio_flush_control_add_tokens (tsd_ptr, diff_usec, &token_gen,
					      &token_consumed);
    }
  rv = pthread_mutex_lock (&thread_Flush_control_thread.lock);
  thread_Flush_control_thread.is_available = false;
  thread_Flush_control_thread.is_running = false;
  pthread_mutex_unlock (&thread_Flush_control_thread.lock);

  fileio_flush_control_finalize ();
  er_final (false);

error:
  tsd_ptr->status = TS_DEAD;

  thread_Flush_control_thread.is_running = false;

  return (THREAD_RET_T) 0;
}

void
thread_wakeup_flush_control_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Flush_control_thread.lock);
  if (!thread_Flush_control_thread.is_running)
    {
      pthread_cond_signal (&thread_Flush_control_thread.cond);
    }
  pthread_mutex_unlock (&thread_Flush_control_thread.lock);
}

/*
 * thread_log_flush_thread() - flushed dirty log pages in background
 *   return:
 *   arg(in) : thread entry information
 *
 */
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_log_flush_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv, ret;

  struct timespec LFT_wakeup_time = { 0, 0 };
  struct timeval wakeup_time = { 0, 0 };
  struct timeval wait_time = { 0, 0 };
  struct timeval tmp_timeval = { 0, 0 };

  int working_time, remained_time, total_elapsed_time, param_refresh_remained;
  int gc_interval, wakeup_interval;
  int param_refresh_interval = 3000;
  int max_wait_time = 1000;

  LOG_GROUP_COMMIT_INFO *group_commit_info = &log_Gl.group_commit_info;

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finishes */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;	/* daemon thread */
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Log_flush_thread.is_available = true;
  thread_Log_flush_thread.is_running = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  gettimeofday (&wakeup_time, NULL);
  total_elapsed_time = 0;
  param_refresh_remained = param_refresh_interval;

  tsd_ptr->event_stats.trace_log_flush_time =
    prm_get_integer_value (PRM_ID_LOG_TRACE_FLUSH_TIME_MSECS);

  while (!tsd_ptr->shutdown)
    {
      er_clear ();

      gc_interval =
	prm_get_integer_value (PRM_ID_LOG_GROUP_COMMIT_INTERVAL_MSECS);

      wakeup_interval = max_wait_time;
      if (gc_interval > 0)
	{
	  wakeup_interval = MIN (gc_interval, wakeup_interval);
	}

      gettimeofday (&wait_time, NULL);
      working_time = (int) timeval_diff_in_msec (&wait_time, &wakeup_time);
      total_elapsed_time += working_time;

      remained_time = MAX ((int) (wakeup_interval - working_time), 0);
      (void) timeval_add_msec (&tmp_timeval, &wait_time, remained_time);
      (void) timeval_to_timespec (&LFT_wakeup_time, &tmp_timeval);

      rv = pthread_mutex_lock (&thread_Log_flush_thread.lock);

      ret = 0;
      if (thread_Log_flush_thread.nrequestors == 0 || gc_interval > 0)
	{
	  thread_Log_flush_thread.is_running = false;
	  ret = pthread_cond_timedwait (&thread_Log_flush_thread.cond,
					&thread_Log_flush_thread.lock,
					&LFT_wakeup_time);
	  thread_Log_flush_thread.is_running = true;
	}

      rv = pthread_mutex_unlock (&thread_Log_flush_thread.lock);

      gettimeofday (&wakeup_time, NULL);
      total_elapsed_time += timeval_diff_in_msec (&wakeup_time, &wait_time);

      if (tsd_ptr->shutdown)
	{
	  break;
	}

      if (ret == ETIMEDOUT)
	{
	  if (total_elapsed_time < gc_interval)
	    {
	      continue;
	    }
	}

      /* to prevent performance degradation */
      param_refresh_remained -= total_elapsed_time;
      if (param_refresh_remained < 0)
	{
	  tsd_ptr->event_stats.trace_log_flush_time
	    = prm_get_integer_value (PRM_ID_LOG_TRACE_FLUSH_TIME_MSECS);

	  param_refresh_remained = param_refresh_interval;
	}

      LOG_CS_ENTER (tsd_ptr);
      logpb_flush_pages_direct (tsd_ptr);
      LOG_CS_EXIT ();

      log_Stat.gc_flush_count++;
      total_elapsed_time = 0;

      rv = pthread_mutex_lock (&group_commit_info->gc_mutex);
      pthread_cond_broadcast (&group_commit_info->gc_cond);
      thread_reset_nrequestors_of_log_flush_thread ();
      pthread_mutex_unlock (&group_commit_info->gc_mutex);

#if defined(CUBRID_DEBUG)
      er_log_debug (ARG_FILE_LINE,
		    "css_log_flush_thread: "
		    "[%d]send signal - waiters\n", (int) THREAD_ID ());
#endif /* CUBRID_DEBUG */
    }

  rv = pthread_mutex_lock (&thread_Log_flush_thread.lock);
  thread_Log_flush_thread.is_available = false;
  thread_Log_flush_thread.is_running = false;
  pthread_mutex_unlock (&thread_Log_flush_thread.lock);

  er_final (false);
  tsd_ptr->status = TS_DEAD;

#if defined(CUBRID_DEBUG)
  er_log_debug (ARG_FILE_LINE,
		"css_log_flush_thread: " "[%d]end \n", (int) THREAD_ID ());
#endif /* CUBRID_DEBUG */

  return (THREAD_RET_T) 0;
}


/*
 * thread_wakeup_log_flush_thread() -
 *   return:
 */
void
thread_wakeup_log_flush_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Log_flush_thread.lock);
  pthread_cond_signal (&thread_Log_flush_thread.cond);
  thread_Log_flush_thread.nrequestors++;
  pthread_mutex_unlock (&thread_Log_flush_thread.lock);
}

/*
 * thread_reset_nrequestors_of_log_flush_thread() -
 *   return:
 */
static void
thread_reset_nrequestors_of_log_flush_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Log_flush_thread.lock);
  thread_Log_flush_thread.nrequestors = 0;
  pthread_mutex_unlock (&thread_Log_flush_thread.lock);
}

INT64
thread_get_log_clock_msec (void)
{
  struct timeval tv;
#if defined(HAVE_ATOMIC_BUILTINS)
  int rv;

  if (thread_Log_clock_thread.is_available == true)
    {
      return log_Clock_msec;
    }
#endif
  gettimeofday (&tv, NULL);

  return (tv.tv_sec * 1000LL) + (tv.tv_usec / 1000LL);
}

/*
 * thread_log_clock_thread() - set time for every 500 ms
 *   return:
 *   arg(in) : thread entry information
 *
 */
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_log_clock_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr = NULL;
#endif /* !HPUX */
  int rv = 0;
  struct timeval now;

#if defined(HAVE_ATOMIC_BUILTINS)
  assert (sizeof (log_Clock_msec) >= sizeof (now.tv_sec));
#endif /* HAVE_ATOMIC_BUILTINS */
  tsd_ptr = (THREAD_ENTRY *) arg_p;

  /* wait until THREAD_CREATE() finishes */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */
  thread_Log_clock_thread.is_available = true;
  thread_Log_clock_thread.is_running = true;

  while (!tsd_ptr->shutdown)
    {
#if defined(HAVE_ATOMIC_BUILTINS)
      INT64 clock_milli_sec;
      er_clear ();

      /* set time for every 200 ms */
      gettimeofday (&now, NULL);
      clock_milli_sec = (now.tv_sec * 1000LL) + (now.tv_usec / 1000LL);
      ATOMIC_TAS_64 (&log_Clock_msec, clock_milli_sec);
      thread_sleep (200);	/* 200 msec */
#else /* HAVE_ATOMIC_BUILTINS */
      int wakeup_interval = 1000;
      struct timespec wakeup_time;
      INT64 tmp_usec;

      er_clear ();
      gettimeofday (&now, NULL);
      wakeup_time.tv_sec = now.tv_sec + (wakeup_interval / 1000);
      tmp_usec = now.tv_usec + (wakeup_interval % 1000) * 1000;

      if (tmp_usec >= 1000000)
	{
	  wakeup_time.tv_sec += 1;
	  tmp_usec -= 1000000;
	}
      wakeup_time.tv_nsec = tmp_usec * 1000;

      rv = pthread_mutex_lock (&thread_Log_clock_thread.lock);
      thread_Log_clock_thread.is_running = false;

      do
	{
	  rv = pthread_cond_timedwait (&thread_Log_clock_thread.cond,
				       &thread_Log_clock_thread.lock,
				       &wakeup_time);
	}
      while (rv == 0 && tsd_ptr->shutdown == false);

      thread_Log_clock_thread.is_running = true;

      pthread_mutex_unlock (&thread_Log_clock_thread.lock);
#endif /* HAVE_ATOMIC_BUILTINS */
    }

  thread_Log_clock_thread.is_available = false;
  thread_Log_clock_thread.is_running = false;

  er_final (false);
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/*
 * thread_auto_volume_expansion_thread() -
 *   return:
 */
static THREAD_RET_T THREAD_CALLING_CONVENTION
thread_auto_volume_expansion_thread (void *arg_p)
{
#if !defined(HPUX)
  THREAD_ENTRY *tsd_ptr;
#endif /* !HPUX */
  int rv;
  short volid;
  int npages;

  tsd_ptr = (THREAD_ENTRY *) arg_p;
  /* wait until THREAD_CREATE() finish */
  rv = pthread_mutex_lock (&tsd_ptr->th_entry_lock);
  pthread_mutex_unlock (&tsd_ptr->th_entry_lock);

  thread_set_thread_entry_info (tsd_ptr);	/* save TSD */
  tsd_ptr->type = TT_DAEMON;
  tsd_ptr->status = TS_RUN;	/* set thread stat as RUN */

  thread_Auto_volume_expansion_thread.is_available = true;

  thread_set_current_tran_index (tsd_ptr, LOG_SYSTEM_TRAN_INDEX);

  pthread_mutex_init (&boot_Auto_addvol_job.lock, NULL);
  boot_Auto_addvol_job.ret_volid = NULL_VOLID;
  memset (&boot_Auto_addvol_job.ext_info, '\0', sizeof (DBDEF_VOL_EXT_INFO));

  rv = pthread_cond_init (&boot_Auto_addvol_job.cond, NULL);
  if (rv != 0)
    {
      er_set_with_oserror (ER_ERROR_SEVERITY, ARG_FILE_LINE,
			   ER_CSS_PTHREAD_COND_INIT, 0);
      tsd_ptr->status = TS_DEAD;

      return (THREAD_RET_T) 0;
    }

  while (!tsd_ptr->shutdown)
    {
      er_clear ();
      rv = pthread_mutex_lock (&thread_Auto_volume_expansion_thread.lock);
      thread_Auto_volume_expansion_thread.is_running = false;

      if (tsd_ptr->shutdown)
	{
	  pthread_mutex_unlock (&thread_Auto_volume_expansion_thread.lock);
	  break;
	}
      pthread_cond_wait (&thread_Auto_volume_expansion_thread.cond,
			 &thread_Auto_volume_expansion_thread.lock);

      thread_Auto_volume_expansion_thread.is_running = true;

      pthread_mutex_unlock (&thread_Auto_volume_expansion_thread.lock);

      rv = pthread_mutex_lock (&boot_Auto_addvol_job.lock);
      npages = boot_Auto_addvol_job.ext_info.extend_npages;
      if (npages <= 0)
	{
	  pthread_mutex_unlock (&boot_Auto_addvol_job.lock);
	  continue;
	}
      pthread_mutex_unlock (&boot_Auto_addvol_job.lock);

      volid = disk_cache_get_auto_extend_volid (tsd_ptr);

      if (volid != NULL_VOLID)
	{
	  if (csect_enter (tsd_ptr, CSECT_BOOT_SR_DBPARM, INF_WAIT) ==
	      NO_ERROR)
	    {
	      if (disk_expand_perm (tsd_ptr, volid, npages) <= 0)
		{
		  volid = NULL_VOLID;
		}

	      csect_exit (CSECT_BOOT_SR_DBPARM);
	    }
	  else
	    {
	      volid = NULL_VOLID;
	    }
	}

      pthread_mutex_lock (&boot_Auto_addvol_job.lock);
      boot_Auto_addvol_job.ret_volid = volid;
      (void) pthread_cond_broadcast (&boot_Auto_addvol_job.cond);
      memset (&boot_Auto_addvol_job.ext_info, '\0',
	      sizeof (DBDEF_VOL_EXT_INFO));
      pthread_mutex_unlock (&boot_Auto_addvol_job.lock);
    }

  (void) pthread_mutex_destroy (&boot_Auto_addvol_job.lock);
  (void) pthread_cond_destroy (&boot_Auto_addvol_job.cond);

  er_final (false);
  thread_Auto_volume_expansion_thread.is_available = false;
  tsd_ptr->status = TS_DEAD;

  return (THREAD_RET_T) 0;
}

/*
 * thread_auto_volume_expansion_thread_is_running () -
 *   return:
 */
bool
thread_auto_volume_expansion_thread_is_running (void)
{
  int rv;
  bool ret;

  rv = pthread_mutex_lock (&thread_Auto_volume_expansion_thread.lock);
  ret = thread_Auto_volume_expansion_thread.is_running;
  pthread_mutex_unlock (&thread_Auto_volume_expansion_thread.lock);

  return ret;
}

/*
 * thread_is_auto_volume_expansion_thread_available () -
 *   return:
 *
 *   NOTE: This is used in boot_add_auto_volume_extension()
 *         to tell whether the thread is working or not.
 *         When restart server, in log_recovery phase, the thread may be unavailable.
 */
bool
thread_is_auto_volume_expansion_thread_available (void)
{
  return thread_Auto_volume_expansion_thread.is_available;
}

/*
 *  thread_wakeup_auto_volume_expansion_thread() -
 *   return:
 */
void
thread_wakeup_auto_volume_expansion_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Auto_volume_expansion_thread.lock);
  if (!thread_Auto_volume_expansion_thread.is_running)
    {
      pthread_cond_signal (&thread_Auto_volume_expansion_thread.cond);
    }
  pthread_mutex_unlock (&thread_Auto_volume_expansion_thread.lock);
}

#if defined(ENABLE_UNUSED_FUNCTION)
/*
 * thread_wakeup_check_ha_delay_info_thread() -
 *   return:
 */
void
thread_wakeup_check_ha_delay_info_thread (void)
{
  int rv;

  rv = pthread_mutex_lock (&thread_Check_ha_delay_info_thread.lock);
  if (!thread_Check_ha_delay_info_thread.is_running)
    {
      pthread_cond_signal (&thread_Check_ha_delay_info_thread.cond);
    }
  pthread_mutex_unlock (&thread_Check_ha_delay_info_thread.lock);
}
#endif

/*
 * thread_slam_tran_index() -
 *   return:
 *   tran_index(in):
 */
void
thread_slam_tran_index (THREAD_ENTRY * thread_p, int tran_index)
{
  logtb_set_tran_index_interrupt (thread_p, tran_index, true);
  er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_CONN_SHUTDOWN, 0);
  css_shutdown_conn_by_tran_index (tran_index);
}

/*
 * xthread_kill_tran_index() - Kill given transaction.
 *   return:
 *   kill_tran_index(in):
 *   kill_user(in):
 *   kill_host(in):
 *   kill_pid(id):
 */
int
xthread_kill_tran_index (THREAD_ENTRY * thread_p, int kill_tran_index,
			 char *kill_user_p, char *kill_host_p, int kill_pid)
{
  char *slam_progname_p;	/* Client program name for tran */
  char *slam_user_p;		/* Client user name for tran    */
  char *slam_host_p;		/* Client host for tran         */
  int slam_pid;			/* Client process id for tran   */
  bool signaled = false;
  int error_code = NO_ERROR;
  bool killed = false;
  int i;

  if (kill_tran_index == NULL_TRAN_INDEX
      || kill_user_p == NULL
      || kill_host_p == NULL
      || strcmp (kill_user_p, "") == 0 || strcmp (kill_host_p, "") == 0)
    {
      /*
       * Not enough information to kill specific transaction..
       *
       * For now.. I am setting an er_set..since I have so many files out..and
       * I cannot compile more junk..
       */
      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE, ER_CSS_KILL_BAD_INTERFACE, 0);
      return ER_CSS_KILL_BAD_INTERFACE;
    }

  signaled = false;
  for (i = 0;
       i < THREAD_RETRY_MAX_SLAM_TIMES && error_code == NO_ERROR && !killed;
       i++)
    {
      if (logtb_find_client_name_host_pid (kill_tran_index, &slam_progname_p,
					   &slam_user_p, &slam_host_p,
					   &slam_pid) != NO_ERROR)
	{
	  if (signaled == false)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_CSS_KILL_UNKNOWN_TRANSACTION, 4,
		      kill_tran_index, kill_user_p, kill_host_p, kill_pid);
	      error_code = ER_CSS_KILL_UNKNOWN_TRANSACTION;
	    }
	  else
	    {
	      killed = true;
	    }
	  break;
	}

      if (kill_pid == slam_pid
	  && strcmp (kill_user_p, slam_user_p) == 0
	  && strcmp (kill_host_p, slam_host_p) == 0)
	{
	  thread_slam_tran_index (thread_p, kill_tran_index);
	  signaled = true;
	}
      else
	{
	  if (signaled == false)
	    {
	      er_set (ER_ERROR_SEVERITY, ARG_FILE_LINE,
		      ER_CSS_KILL_DOES_NOTMATCH, 8,
		      kill_tran_index, kill_user_p, kill_host_p, kill_pid,
		      kill_tran_index, slam_user_p, slam_host_p, slam_pid);
	      error_code = ER_CSS_KILL_DOES_NOTMATCH;
	    }
	  else
	    {
	      killed = true;
	    }
	  break;
	}
      thread_sleep (1000);	/* 1000 msec */
    }

  if (error_code == NO_ERROR && !killed)
    {
      error_code = ER_FAILED;	/* timeout */
    }

  return error_code;
}

/*
 * thread_find_first_lockwait_entry() -
 *   return:
 *   thread_index_p(in):
 */
THREAD_ENTRY *
thread_find_first_lockwait_entry (int *thread_index_p)
{
  THREAD_ENTRY *thread_p;
  int i;

  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &(thread_Manager.thread_array[i]);
      if (thread_p->status == TS_DEAD || thread_p->status == TS_FREE)
	{
	  continue;
	}
      if (thread_p->lockwait != NULL)
	{			/* found */
	  *thread_index_p = i;
	  return thread_p;
	}
    }

  return (THREAD_ENTRY *) NULL;
}

/*
 * thread_find_next_lockwait_entry() -
 *   return:
 *   thread_index_p(in):
 */
THREAD_ENTRY *
thread_find_next_lockwait_entry (int *thread_index_p)
{
  THREAD_ENTRY *thread_p;
  int i;

  for (i = (*thread_index_p + 1); i <= thread_Manager.num_workers; i++)
    {
      thread_p = &(thread_Manager.thread_array[i]);
      if (thread_p->status == TS_DEAD || thread_p->status == TS_FREE)
	{
	  continue;
	}
      if (thread_p->lockwait != NULL)
	{			/* found */
	  *thread_index_p = i;
	  return thread_p;
	}
    }

  return (THREAD_ENTRY *) NULL;
}

/*
 * thread_find_entry_by_index() -
 *   return:
 *   thread_index(in):
 */
THREAD_ENTRY *
thread_find_entry_by_index (int thread_index)
{
  return (&thread_Manager.thread_array[thread_index]);
}

/*
 * thread_get_lockwait_entry() -
 *   return:
 *   tran_index(in):
 *   thread_array_p(in):
 */
int
thread_get_lockwait_entry (int tran_index, THREAD_ENTRY ** thread_array_p)
{
  THREAD_ENTRY *thread_p;
  int i, thread_count;

  thread_count = 0;
  for (i = 1; i <= thread_Manager.num_workers; i++)
    {
      thread_p = &(thread_Manager.thread_array[i]);
      if (thread_p->status == TS_DEAD || thread_p->status == TS_FREE)
	{
	  continue;
	}
      if (thread_p->tran_index == tran_index && thread_p->lockwait != NULL)
	{
	  thread_array_p[thread_count] = thread_p;
	  thread_count++;
	  if (thread_count >= 10)
	    {
	      break;
	    }
	}
    }

  return thread_count;
}

/*
 * thread_set_info () -
 *   return:
 *   thread_p(out):
 *   client_id(in):
 *   rid(in):
 *   tran_index(in):
 */
void
thread_set_info (THREAD_ENTRY * thread_p, int client_id, int rid,
		 int tran_index)
{
  thread_p->client_id = client_id;
  thread_p->rid = rid;
  thread_p->tran_index = tran_index;
  thread_p->victim_request_fail = false;
  thread_p->next_wait_thrd = NULL;
  thread_p->lockwait = NULL;
  thread_p->lockwait_state = -1;
  thread_p->query_entry = NULL;
  thread_p->tran_next_wait = NULL;

  thread_clear_recursion_depth (thread_p);
}

/*
 * thread_trace_on () -
 *   return:
 *   thread_p(in):
 */
void
thread_trace_on (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  thread_p->on_trace = true;
}

/*
 * thread_set_trace_format () -
 *   return:
 *   thread_p(in):
 *   format(in):
 */
void
thread_set_trace_format (THREAD_ENTRY * thread_p, int format)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  thread_p->trace_format = format;
}

/*
 * thread_is_on_trace () -
 *   return:
 *   thread_p(in):
 */
bool
thread_is_on_trace (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  return thread_p->on_trace;
}

/*
 * thread_set_clear_trace () -
 *   return:
 *   thread_p(in):
 *   clear(in):
 */
void
thread_set_clear_trace (THREAD_ENTRY * thread_p, bool clear)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  thread_p->clear_trace = clear;
}

/*
 * thread_need_clear_trace() -
 *   return:
 *   thread_p(in):
 */
bool
thread_need_clear_trace (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  return thread_p->clear_trace;
}

/*
 * thread_set_sort_stats_active() -
 *   return:
 *   flag(in):
 */
bool
thread_set_sort_stats_active (THREAD_ENTRY * thread_p, bool flag)
{
  bool old_val = false;

  if (BO_IS_SERVER_RESTARTED ())
    {
      if (thread_p == NULL)
	{
	  thread_p = thread_get_thread_entry_info ();
	}

      old_val = thread_p->sort_stats_active;
      thread_p->sort_stats_active = flag;
    }

  return old_val;
}

/*
 * thread_get_sort_stats_active() -
 *   return:
 */
bool
thread_get_sort_stats_active (THREAD_ENTRY * thread_p)
{
  bool ret_val = false;

  if (BO_IS_SERVER_RESTARTED ())
    {
      if (thread_p == NULL)
	{
	  thread_p = thread_get_thread_entry_info ();
	}

      ret_val = thread_p->sort_stats_active;
    }

  return ret_val;
}

/*
 * thread_get_recursion_depth() -
 */
int
thread_get_recursion_depth (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  return thread_p->xasl_recursion_depth;
}

/*
 * thread_inc_recursion_depth() -
 */
void
thread_inc_recursion_depth (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  thread_p->xasl_recursion_depth++;
}

/*
 * thread_dec_recursion_depth() -
 */
void
thread_dec_recursion_depth (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  thread_p->xasl_recursion_depth--;
}

/*
 * thread_clear_recursion_depth() -
 */
void
thread_clear_recursion_depth (THREAD_ENTRY * thread_p)
{
  if (thread_p == NULL)
    {
      thread_p = thread_get_thread_entry_info ();
    }

  thread_p->xasl_recursion_depth = 0;
}
