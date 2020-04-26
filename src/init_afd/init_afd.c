/*
 *  init_afd.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 1995 - 2020 Deutscher Wetterdienst (DWD),
 *                            Holger Kiehl <Holger.Kiehl@dwd.de>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "afddefs.h"

DESCR__S_M1
/*
 ** NAME
 **   init_afd - starts all processes for the AFD and keeps them
 **              alive
 **
 ** SYNOPSIS
 **   init_afd [--version] [-w <work dir>] [-nd]
 **        --version      Prints current version and copyright
 **        -w <work dir>  Working directory of the AFD
 **        -nd            Do not start as daemon process
 **
 ** DESCRIPTION
 **   This program will start all programs used by the AFD in the
 **   correct order and will restart certain process that dies.
 **
 ** RETURN VALUES
 **   SUCCESS on normal exit and INCORRECT when an error has occurred.
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   25.10.1995 H.Kiehl Created
 **   11.02.1997 H.Kiehl Start and watch I/O-process.
 **   22.03.1997 H.Kiehl Added support for systems without mmap().
 **   10.06.1997 H.Kiehl When job counter reaches critical value stop
 **                      queue for jobs with lots of files waiting to
 **                      be send.
 **   03.09.1997 H.Kiehl Added process AFDD.
 **   23.02.1998 H.Kiehl Clear pool directory before starting AMG.
 **   16.10.1998 H.Kiehl Added parameter -nd so we do NOT start as
 **                      daemon.
 **   30.06.2001 H.Kiehl Starting afdd process is now only done when
 **                      this is configured in AFD_CONFIG file.
 **   06.04.2002 H.Kiehl Added saving of old core files.
 **   05.05.2002 H.Kiehl Monitor the number of files in full directories.
 **   16.05.2002 H.Kiehl Included a heartbeat counter in AFD_ACTIVE_FILE.
 **   25.08.2002 H.Kiehl Addition of process rename_log.
 **   12.08.2004 H.Kiehl Replace rec() with system_log().
 **   05.06.2007 H.Kiehl Systems like HPUX have problems when writting
 **                      the pid's to AFD_ACTIVE file with write(). Using
 **                      mmap() instead, seems to solve the problem.
 **   03.02.2009 H.Kiehl In case the AFD_ACTIVE file gets deleted while
 **                      AFD is still active and we do a a shutdown, take
 **                      the existing open file descriptor to AFD_ACTIVE
 **                      file instead of trying to open it again.
 **   20.12.2010 H.Kiehl Log some data to transfer log not system log.
 **   13.05.2012 H.Kiehl Do not exit() if we fail to stat() the files
 **                      directory.
 **   17.03.2017 H.Kiehl Remove some work to a new process init_afd_worker.
 **
 */
DESCR__E_M1

#include <stdio.h>               /* fprintf()                            */
#include <string.h>              /* strcpy(), strcat(), strerror()       */
#include <stdlib.h>              /* getenv(), atexit(), exit(), abort()  */
#include <time.h>                /* time(), ctime()                      */
#include <sys/types.h>
#ifdef HAVE_MMAP
# include <sys/mman.h>           /* mmap(), munmap()                     */
#endif
#include <sys/stat.h>
#include <sys/time.h>            /* struct timeval                       */
#ifdef HAVE_SETPRIORITY
# include <sys/resource.h>       /* setpriority()                        */
#endif
#include <sys/wait.h>            /* waitpid()                            */
#include <signal.h>              /* signal()                             */
#include <unistd.h>              /* select(), unlink(), lseek(), sleep() */
                                 /* gethostname(),  STDERR_FILENO        */
#include <fcntl.h>               /* O_RDWR, O_CREAT, O_WRONLY, etc       */
#include <dirent.h>              /* opendir(), readdir(), closedir(),    */
                                 /* DIR, struct dirent                   */
#include <errno.h>
#include "version.h"

#define BLOCK_SIGNALS
#define NO_OF_SAVED_CORE_FILES 10

/* Global definitions. */
int                        sys_log_fd = STDERR_FILENO,
                           event_log_fd = STDERR_FILENO,
                           afd_cmd_fd,
                           afd_resp_fd,
                           amg_cmd_fd,
                           fd_cmd_fd,
                           afd_active_fd = -1,
                           probe_only_fd,
#ifdef WITHOUT_FIFO_RW_SUPPORT
                           afd_cmd_writefd,
                           afd_resp_readfd,
                           amg_cmd_readfd,
                           fd_cmd_readfd,
                           probe_only_readfd,
#endif
                           probe_only = 1,
                           no_of_dirs = 0,
                           no_of_hosts = 0,
                           no_of_disabled_dirs = 0,
                           fra_fd = -1,
                           fra_id,
                           fsa_fd = -1,
                           fsa_id;
#ifdef HAVE_MMAP
off_t                      fra_size,
                           fsa_size;
#endif
char                       *pid_list,
                           *p_work_dir,
                           afd_status_file[MAX_PATH_LENGTH],
                           afd_active_file[MAX_PATH_LENGTH],
                           **disabled_dirs = NULL;
struct afd_status          *p_afd_status;
struct filetransfer_status *fsa;
struct fileretrieve_status *fra;
struct proc_table          proc_table[NO_OF_PROCESS + 1];
const char                 *sys_log_name = SYSTEM_LOG_FIFO;

/* Local function prototypes. */
static pid_t               make_process(char *, char *, sigset_t *);
static void                afd_exit(void),
                           check_dirs(char *),
                           delete_old_afd_status_files(void),
                           get_afd_config_value(int *, unsigned int *, int *),
                           sig_exit(int),
                           sig_bus(int),
                           sig_segv(int),
                           stuck_transfer_check(time_t),
                           zombie_check(void);


/*$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ main() $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$*/
int
main(int argc, char *argv[])
{
   int            afdd_port,
                  afd_status_fd,
                  current_month,
#ifdef AFDBENCH_CONFIG
                  pause_dir_scan,
#endif
                  status,
                  i,
                  in_global_filesystem,
                  auto_amg_stop = NO,
                  old_afd_stat;
   unsigned int   default_age_limit,
                  *heartbeat;
   long           link_max;
   off_t          afd_active_size;
   time_t         
                  month_check_time,
                  now,
                  disabled_dir_check_time;
   fd_set         rset;
   signed char    stop_typ = STARTUP_ID,
                  char_value;
   char           afd_file_dir[MAX_PATH_LENGTH],
                  *ptr = NULL,
                  *shared_shutdown,
                  work_dir[MAX_PATH_LENGTH];
   struct timeval timeout;
   struct tm      *bd_time;
   struct stat    stat_buf;

   CHECK_FOR_VERSION(argc, argv);

   /* First get working directory for the AFD. */
   if (get_afd_path(&argc, argv, work_dir) < 0)
   {
      exit(INCORRECT);
   }
#ifdef AFDBENCH_CONFIG
   if (get_arg(&argc, argv, "-A", NULL, 0) == SUCCESS)
   {
      pause_dir_scan = YES;
   }
   else
   {
      pause_dir_scan = NO;
   }
#endif
#ifdef WITH_SETUID_PROGS
   set_afd_euid(work_dir);
#endif
   (void)umask(0);

   /* Check if the working directory does exists and has */
   /* the correct permissions set. If not it is created. */
   if (check_dir(work_dir, R_OK | W_OK | X_OK) < 0)
   {
      exit(INCORRECT);
   }

   /* Initialise variables. */
   p_work_dir = work_dir;
   (void)strcpy(afd_status_file, work_dir);
   (void)strcat(afd_status_file, FIFO_DIR);
   i = strlen(afd_status_file);
   (void)strcpy(afd_active_file, afd_status_file);
   (void)strcpy(afd_active_file + i, AFD_ACTIVE_FILE);
   (void)sprintf(afd_status_file + i, "/%s.%x",
                 AFD_STATUS_FILE, get_afd_status_struct_size());

   (void)strcpy(afd_file_dir, work_dir);
   (void)strcat(afd_file_dir, AFD_FILE_DIR);

   /* Make sure that no other AFD is running in this directory. */
   if (check_afd_heartbeat(DEFAULT_HEARTBEAT_TIMEOUT, YES) == 1)
   {
      (void)fprintf(stderr, _("ERROR   : Another AFD is already active.\n"));
      exit(INCORRECT);
   }
   probe_only = 0;
   if ((afd_active_fd = coe_open(afd_active_file, (O_RDWR | O_CREAT | O_TRUNC),
#ifdef GROUP_CAN_WRITE
                                 (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP))) == -1)
#else
                                 (S_IRUSR | S_IWUSR))) == -1)
#endif
   {
      (void)fprintf(stderr, _("ERROR   : Failed to create `%s' : %s (%s %d)\n"),
                    afd_active_file, strerror(errno), __FILE__, __LINE__);
      exit(INCORRECT);
   }
   afd_active_size = ((NO_OF_PROCESS + 1) * sizeof(pid_t)) + sizeof(unsigned int) + 1 + 1;
   if (lseek(afd_active_fd, afd_active_size, SEEK_SET) == -1)
   {
      (void)fprintf(stderr, _("ERROR   : lseek() error in `%s' : %s (%s %d)\n"),
                    afd_active_file, strerror(errno), __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   char_value = EOF;
   if (write(afd_active_fd, &char_value, 1) != 1)
   {
      (void)fprintf(stderr, _("ERROR   : write() error in `%s' : %s (%s %d)\n"),
                    afd_active_file, strerror(errno), __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
#ifdef HAVE_MMAP
   if ((ptr = mmap(NULL, afd_active_size, (PROT_READ | PROT_WRITE),
                   MAP_SHARED, afd_active_fd, 0)) == (caddr_t) -1)
#else
   if ((ptr = mmap_emu(NULL, afd_active_size, (PROT_READ | PROT_WRITE),
                       MAP_SHARED, afd_active_file, 0)) == (caddr_t) -1)
#endif
   {
      (void)fprintf(stderr, _("ERROR   : mmap() error : %s (%s %d)\n"),
                    strerror(errno), __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   heartbeat = (unsigned int *)(ptr + afd_active_size - (sizeof(unsigned int) + 1 + 1));
   pid_list = ptr;
   shared_shutdown = ptr + afd_active_size - (sizeof(unsigned int) + 1 + 1) + sizeof(unsigned int);
   *shared_shutdown = 0;
   *heartbeat = 0;

   /* Open and create all fifos. */
   init_fifos_afd();

   if ((argc == 2) && (argv[1][0] == '-') && (argv[1][1] == 'n') &&
       (argv[1][2] == 'd'))
   {
      /* DO NOT START AS DAEMON!!! */;
   }
   else
   {
      daemon_init(AFD);
   }

   /* Now check if all directories needed are created. */
   check_dirs(work_dir);

   if (((i = stat(afd_status_file, &stat_buf)) == -1) ||
       (stat_buf.st_size != sizeof(struct afd_status)))
   {
      if ((i == -1) && (errno != ENOENT))
      {
         (void)fprintf(stderr, _("Failed to stat() `%s' : %s (%s %d)\n"),
                       afd_status_file, strerror(errno), __FILE__, __LINE__);
         (void)unlink(afd_active_file);
         exit(INCORRECT);
      }
      else
      {
         (void)fprintf(stderr, _("INFO: No old afd status file %s found. (%s %d)\n"),
                       afd_status_file, __FILE__, __LINE__);
      }
      if ((afd_status_fd = coe_open(afd_status_file, O_RDWR | O_CREAT | O_TRUNC,
#ifdef GROUP_CAN_WRITE
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP)) == -1)
#else
                                    S_IRUSR | S_IWUSR)) == -1)
#endif
      {
         (void)fprintf(stderr, _("Failed to create `%s' : %s (%s %d)\n"),
                       afd_status_file, strerror(errno), __FILE__, __LINE__);
         (void)unlink(afd_active_file);
         exit(INCORRECT);
      }
      if (lseek(afd_status_fd, sizeof(struct afd_status) - 1, SEEK_SET) == -1)
      {
         (void)fprintf(stderr, _("Could not seek() on `%s' : %s (%s %d)\n"),
                       afd_status_file, strerror(errno), __FILE__, __LINE__);
         (void)unlink(afd_active_file);
         exit(INCORRECT);
      }
      if (write(afd_status_fd, "", 1) != 1)
      {
         (void)fprintf(stderr, _("write() error : %s (%s %d)\n"),
                       strerror(errno), __FILE__, __LINE__);
         (void)unlink(afd_active_file);
         exit(INCORRECT);
      }
      old_afd_stat = NO;
      delete_old_afd_status_files();
   }
   else
   {
      if ((afd_status_fd = coe_open(afd_status_file, O_RDWR)) == -1)
      {
         (void)fprintf(stderr, _("Failed to create `%s' : %s (%s %d)\n"),
                       afd_status_file, strerror(errno), __FILE__, __LINE__);
         (void)unlink(afd_active_file);
         exit(INCORRECT);
      }
      old_afd_stat = YES;
   }
#ifdef HAVE_MMAP
   if ((ptr = mmap(NULL, sizeof(struct afd_status), (PROT_READ | PROT_WRITE),
                   MAP_SHARED, afd_status_fd, 0)) == (caddr_t) -1)
#else
   /* Start mapper process that emulates mmap(). */
   proc_table[MAPPER_NO].pid = make_process(MAPPER, work_dir, NULL);
   *(pid_t *)(pid_list + ((MAPPER_NO + 1) * sizeof(pid_t))) = proc_table[MAPPER_NO].pid;

   if ((ptr = mmap_emu(NULL, sizeof(struct afd_status),
                       (PROT_READ | PROT_WRITE),
                       MAP_SHARED, afd_status_file, 0)) == (caddr_t) -1)
#endif
   {
      (void)fprintf(stderr, _("mmap() error : %s (%s %d)\n"),
                    strerror(errno), __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   p_afd_status = (struct afd_status *)ptr;
   if (old_afd_stat == NO)
   {
      struct system_data sd;

      (void)memset(p_afd_status, 0, sizeof(struct afd_status));
      if (get_system_data(&sd) == SUCCESS)
      {
         p_afd_status->sys_log_ec = sd.sys_log_ec;
         (void)memcpy(p_afd_status->sys_log_fifo, sd.sys_log_fifo,
                      LOG_FIFO_SIZE);
         (void)memcpy(p_afd_status->sys_log_history, sd.sys_log_history,
                      MAX_LOG_HISTORY);
         p_afd_status->receive_log_ec = sd.receive_log_ec;
         (void)memcpy(p_afd_status->receive_log_fifo, sd.receive_log_fifo,
                      LOG_FIFO_SIZE);
         (void)memcpy(p_afd_status->receive_log_history, sd.receive_log_history,
                      MAX_LOG_HISTORY);
         p_afd_status->trans_log_ec = sd.trans_log_ec;
         (void)memcpy(p_afd_status->trans_log_fifo, sd.trans_log_fifo,
                      LOG_FIFO_SIZE);
         (void)memcpy(p_afd_status->trans_log_history, sd.trans_log_history,
                      MAX_LOG_HISTORY);
         p_afd_status->fd_fork_counter = sd.fd_fork_counter;
         p_afd_status->amg_fork_counter = sd.amg_fork_counter;
         p_afd_status->burst2_counter = sd.burst2_counter;
         p_afd_status->max_queue_length = sd.max_queue_length;
         p_afd_status->dir_scans = sd.dir_scans;
#ifdef WITH_INOTIFY
         p_afd_status->inotify_events = sd.inotify_events;
#endif
#ifdef HAVE_WAIT4
         p_afd_status->amg_child_utime.tv_sec = sd.amg_child_utime.tv_sec;
         p_afd_status->amg_child_utime.tv_usec = sd.amg_child_utime.tv_usec;
         p_afd_status->amg_child_stime.tv_sec = sd.amg_child_stime.tv_sec;
         p_afd_status->amg_child_stime.tv_usec = sd.amg_child_stime.tv_usec;
         p_afd_status->fd_child_utime.tv_sec = sd.fd_child_utime.tv_sec;
         p_afd_status->fd_child_utime.tv_usec = sd.fd_child_utime.tv_usec;
         p_afd_status->fd_child_stime.tv_sec = sd.fd_child_stime.tv_sec;
         p_afd_status->fd_child_stime.tv_usec = sd.fd_child_stime.tv_usec;
#endif
      }
      else
      {
         (void)memset(p_afd_status->receive_log_history, NO_INFORMATION,
                      MAX_LOG_HISTORY);
         (void)memset(p_afd_status->sys_log_history, NO_INFORMATION,
                      MAX_LOG_HISTORY);
         (void)memset(p_afd_status->trans_log_history, NO_INFORMATION,
                      MAX_LOG_HISTORY);
      }
   }
   else
   {
      p_afd_status->amg               = 0;
      p_afd_status->amg_jobs          = 0;
      p_afd_status->fd                = 0;
      p_afd_status->sys_log           = 0;
      p_afd_status->maintainer_log    = 0;
      p_afd_status->event_log         = 0;
      p_afd_status->receive_log       = 0;
      p_afd_status->trans_log         = 0;
      p_afd_status->trans_db_log      = 0;
      p_afd_status->archive_watch     = 0;
      p_afd_status->afd_stat          = 0;
      p_afd_status->afdd              = 0;
#ifndef HAVE_MMAP
      p_afd_status->mapper            = 0;
#endif
#ifdef _INPUT_LOG
      p_afd_status->input_log         = 0;
#endif
#ifdef _OUTPUT_LOG
      p_afd_status->output_log        = 0;
#endif
#ifdef _CONFIRMATION_LOG
      p_afd_status->confirmation_log  = 0;
#endif
#ifdef _DELETE_LOG
      p_afd_status->delete_log        = 0;
#endif
#ifdef _PRODUCTION_LOG
      p_afd_status->production_log    = 0;
#endif
#ifdef _DISTRIBUTION_LOG
      p_afd_status->distribution_log  = 0;
#endif
#ifdef _TRANSFER_RATE_LOG
      p_afd_status->transfer_rate_log = 0;
#endif
      p_afd_status->afd_worker        = 0;
      p_afd_status->no_of_transfers   = 0;
      p_afd_status->start_time        = 0L;
   }
   (void)strcpy(p_afd_status->work_dir, work_dir);
   p_afd_status->user_id = geteuid();
   if (gethostname(p_afd_status->hostname, MAX_REAL_HOSTNAME_LENGTH) == -1)
   {
      p_afd_status->hostname[0] = '\0';
   }
   for (i = 0; i < NO_OF_PROCESS; i++)
   {
      proc_table[i].pid = 0;
      switch (i)
      {
         case AMG_NO :
            proc_table[i].status = &p_afd_status->amg;
            (void)strcpy(proc_table[i].proc_name, AMG);
            break;

         case FD_NO :
            proc_table[i].status = &p_afd_status->fd;
            (void)strcpy(proc_table[i].proc_name, FD);
            break;

         case SLOG_NO :
            proc_table[i].status = &p_afd_status->sys_log;
            (void)strcpy(proc_table[i].proc_name, SLOG);
            break;

         case MAINTAINER_LOG_NO :
            proc_table[i].status = &p_afd_status->maintainer_log;
            (void)strcpy(proc_table[i].proc_name, MLOG);
            break;

         case ELOG_NO :
            proc_table[i].status = &p_afd_status->event_log;
            (void)strcpy(proc_table[i].proc_name, ELOG);
            break;

         case RLOG_NO :
            proc_table[i].status = &p_afd_status->receive_log;
            (void)strcpy(proc_table[i].proc_name, RLOG);
            break;

         case TLOG_NO :
            proc_table[i].status = &p_afd_status->trans_log;
            (void)strcpy(proc_table[i].proc_name, TLOG);
            break;

         case TDBLOG_NO :
            proc_table[i].status = &p_afd_status->trans_db_log;
            (void)strcpy(proc_table[i].proc_name, TDBLOG);
            break;

         case AW_NO :
            proc_table[i].status = &p_afd_status->archive_watch;
            (void)strcpy(proc_table[i].proc_name, ARCHIVE_WATCH);
            break;

         case STAT_NO :
            proc_table[i].status = &p_afd_status->afd_stat;
            (void)strcpy(proc_table[i].proc_name, AFD_STAT);
            break;

         case DC_NO : /* dir_check */
            *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = 0;
            break;

         case AFDD_NO :
            proc_table[i].status = &p_afd_status->afdd;
            (void)strcpy(proc_table[i].proc_name, AFDD);
            break;

#ifdef _WITH_ATPD_SUPPORT
         case ATPD_NO :
            proc_table[i].status = &p_afd_status->atpd;
            (void)strcpy(proc_table[i].proc_name, ATPD);
            break;
#endif
#ifdef _WITH_WMOD_SUPPORT
         case WMOD_NO :
            proc_table[i].status = &p_afd_status->wmod;
            (void)strcpy(proc_table[i].proc_name, WMOD);
            break;
#endif
#ifdef _WITH_DE_MAIL_SUPPORT
         case DEMCD_NO :
            proc_table[i].status = &p_afd_status->demcd;
            (void)strcpy(proc_table[i].proc_name, DEMCD);
            break;
#endif
#ifndef HAVE_MMAP
         case MAPPER_NO :
            proc_table[i].status = &p_afd_status->mapper;
            (void)strcpy(proc_table[i].proc_name, MAPPER);
            *proc_table[MAPPER_NO].status = ON;
            break;
#endif
#ifdef _INPUT_LOG
         case INPUT_LOG_NO :
            proc_table[i].status = &p_afd_status->input_log;
            (void)strcpy(proc_table[i].proc_name, INPUT_LOG_PROCESS);
            break;
#endif
#ifdef _OUTPUT_LOG
         case OUTPUT_LOG_NO :
            proc_table[i].status = &p_afd_status->output_log;
            (void)strcpy(proc_table[i].proc_name, OUTPUT_LOG_PROCESS);
            break;
#endif
#ifdef _CONFIRMATION_LOG
         case CONFIRMATION_LOG_NO :
            proc_table[i].status = &p_afd_status->confirmation_log;
            (void)strcpy(proc_table[i].proc_name, CONFIRMATION_LOG_PROCESS);
            break;
#endif
#ifdef _DELETE_LOG
         case DELETE_LOG_NO :
            proc_table[i].status = &p_afd_status->delete_log;
            (void)strcpy(proc_table[i].proc_name, DELETE_LOG_PROCESS);
            break;
#endif
#ifdef _PRODUCTION_LOG
         case PRODUCTION_LOG_NO :
            proc_table[i].status = &p_afd_status->production_log;
            (void)strcpy(proc_table[i].proc_name, PRODUCTION_LOG_PROCESS);
            break;
#endif
#ifdef _DISTRIBUTION_LOG
         case DISTRIBUTION_LOG_NO :
            proc_table[i].status = &p_afd_status->distribution_log;
            (void)strcpy(proc_table[i].proc_name, DISTRIBUTION_LOG_PROCESS);
            break;
#endif
#ifdef _TRANSFER_RATE_LOG
         case TRANSFER_RATE_LOG_NO :
            proc_table[i].status = &p_afd_status->transfer_rate_log;
            (void)strcpy(proc_table[i].proc_name, TRLOG);
            break;
#endif
         case AFD_WORKER_NO :
            proc_table[i].status = &p_afd_status->afd_worker;
            (void)strcpy(proc_table[i].proc_name, AFD_WORKER);
            break;

#if ALDAD_OFFSET != 0
         case ALDAD_NO :
            proc_table[i].status = &p_afd_status->aldad;
            (void)strcpy(proc_table[i].proc_name, ALDAD);
            break;
#endif
         default : /* Impossible */
            (void)fprintf(stderr,
                          _("Unknown process number %d. Giving up! (%s %d)\n"),
                          i, __FILE__, __LINE__);
            exit(INCORRECT);
      }
   }
   get_afd_config_value(&afdd_port, &default_age_limit, &in_global_filesystem);

   /* Do some cleanups when we exit */
   if (atexit(afd_exit) != 0)
   {
      (void)fprintf(stderr,
                    _("Could not register exit function : %s (%s %d)\n"),
                    strerror(errno), __FILE__, __LINE__);
      exit(INCORRECT);
   }

   /* Activate some signal handlers, so we know what happened. */
   if ((signal(SIGINT, sig_exit) == SIG_ERR) ||
       (signal(SIGTERM, SIG_IGN) == SIG_ERR) ||
       (signal(SIGSEGV, sig_segv) == SIG_ERR) ||
       (signal(SIGBUS, sig_bus) == SIG_ERR) ||
       (signal(SIGHUP, SIG_IGN) == SIG_ERR))
   {
      (void)fprintf(stderr, _("signal() error : %s (%s %d)\n"),
                    strerror(errno), __FILE__, __LINE__);
      exit(INCORRECT);
   }

   /*
    * Check all file permission if they are correct.
    */
   check_permissions();

   /*
    * Determine current month.
    */
   now = time(NULL);

   if ((bd_time = localtime(&now)) == NULL)
   {
      system_log(WARN_SIGN, __FILE__, __LINE__,
                 "localtime() error : %s", strerror(errno));
      current_month = 0;
   }
   else
   {
      current_month = bd_time->tm_mon;
   }
   month_check_time = ((now / 86400) * 86400) + 86400;
   disabled_dir_check_time = 0;

   /* Initialise communication flag FD <-> AMG. */
#ifdef AFDBENCH_CONFIG
   if (pause_dir_scan == YES)
   {
      p_afd_status->amg_jobs = PAUSE_DISTRIBUTION;
   }
   else
   {
      p_afd_status->amg_jobs = 0;
   }
#else
   p_afd_status->amg_jobs = 0;
#endif

   /* Start all log process. */
   proc_table[SLOG_NO].pid = make_process(SLOG, work_dir, NULL);
   *(pid_t *)(pid_list + ((SLOG_NO + 1) * sizeof(pid_t))) = proc_table[SLOG_NO].pid;
   *proc_table[SLOG_NO].status = ON;
   proc_table[ELOG_NO].pid = make_process(ELOG, work_dir, NULL);
   *(pid_t *)(pid_list + ((ELOG_NO + 1) * sizeof(pid_t))) = proc_table[ELOG_NO].pid;
   *proc_table[ELOG_NO].status = ON;
   proc_table[RLOG_NO].pid = make_process(RLOG, work_dir, NULL);
   *(pid_t *)(pid_list + ((RLOG_NO + 1) * sizeof(pid_t))) = proc_table[RLOG_NO].pid;
   *proc_table[RLOG_NO].status = ON;
   proc_table[TLOG_NO].pid = make_process(TLOG, work_dir, NULL);
   *(pid_t *)(pid_list + ((TLOG_NO + 1) * sizeof(pid_t))) = proc_table[TLOG_NO].pid;
   *proc_table[TLOG_NO].status = ON;
   proc_table[TDBLOG_NO].pid = make_process(TDBLOG, work_dir, NULL);
   *(pid_t *)(pid_list + ((TDBLOG_NO + 1) * sizeof(pid_t))) = proc_table[TDBLOG_NO].pid;
   *proc_table[TDBLOG_NO].status = ON;

   /* Start process cleaning archive directory. */
   proc_table[AW_NO].pid = make_process(ARCHIVE_WATCH, work_dir, NULL);
   *(pid_t *)(pid_list + ((AW_NO + 1) * sizeof(pid_t))) = proc_table[AW_NO].pid;
   *proc_table[AW_NO].status = ON;

   /* Start process doing the I/O logging. */
#ifdef _INPUT_LOG
   proc_table[INPUT_LOG_NO].pid = make_process(INPUT_LOG_PROCESS, work_dir, NULL);
   *(pid_t *)(pid_list + ((INPUT_LOG_NO + 1) * sizeof(pid_t))) = proc_table[INPUT_LOG_NO].pid;
   *proc_table[INPUT_LOG_NO].status = ON;
#endif
#ifdef _OUTPUT_LOG
   proc_table[OUTPUT_LOG_NO].pid = make_process(OUTPUT_LOG_PROCESS, work_dir, NULL);
   *(pid_t *)(pid_list + ((OUTPUT_LOG_NO + 1) * sizeof(pid_t))) = proc_table[OUTPUT_LOG_NO].pid;
   *proc_table[OUTPUT_LOG_NO].status = ON;
#endif
#ifdef _CONFIRMATION_LOG
   proc_table[CONFIRMATION_LOG_NO].pid = make_process(CONFIRMATION_LOG_PROCESS, work_dir, NULL);
   *(pid_t *)(pid_list + ((CONFIRMATION_LOG_NO + 1) * sizeof(pid_t))) = proc_table[CONFIRMATION_LOG_NO].pid;
   *proc_table[CONFIRMATION_LOG_NO].status = ON;
#endif
#ifdef _DELETE_LOG
   proc_table[DELETE_LOG_NO].pid = make_process(DELETE_LOG_PROCESS, work_dir, NULL);
   *(pid_t *)(pid_list + ((DELETE_LOG_NO + 1) * sizeof(pid_t))) = proc_table[DELETE_LOG_NO].pid;
   *proc_table[DELETE_LOG_NO].status = ON;
#endif
#ifdef _PRODUCTION_LOG
   proc_table[PRODUCTION_LOG_NO].pid = make_process(PRODUCTION_LOG_PROCESS, work_dir, NULL);
   *(pid_t *)(pid_list + ((PRODUCTION_LOG_NO + 1) * sizeof(pid_t))) = proc_table[PRODUCTION_LOG_NO].pid;
   *proc_table[PRODUCTION_LOG_NO].status = ON;
#endif
#ifdef _DISTRIBUTION_LOG
   proc_table[DISTRIBUTION_LOG_NO].pid = make_process(DISTRIBUTION_LOG_PROCESS, work_dir, NULL);
   *(pid_t *)(pid_list + ((DISTRIBUTION_LOG_NO + 1) * sizeof(pid_t))) = proc_table[DISTRIBUTION_LOG_NO].pid;
   *proc_table[DISTRIBUTION_LOG_NO].status = ON;
#endif
#ifdef _MAINTAINER_LOG
   proc_table[MAINTAINER_LOG_NO].pid = make_process(MLOG, work_dir, NULL);
   *(pid_t *)(pid_list + ((MAINTAINER_LOG_NO + 1) * sizeof(pid_t))) = proc_table[MAINTAINER_LOG_NO].pid;
   *proc_table[MAINTAINER_LOG_NO].status = ON;
#else
   proc_table[MAINTAINER_LOG_NO].pid = -1;
   *proc_table[MAINTAINER_LOG_NO].status = NEITHER;
#endif

   /* Tell user at what time the AFD was started. */
   *(pid_t *)(pid_list) = getpid();
   system_log(CONFIG_SIGN, NULL, 0,
              "=================> STARTUP <=================");
   if (p_afd_status->hostname[0] != '\0')
   {
      char dstr[26];

      strftime(dstr, 26, "%a %h %d %H:%M:%S %Y", localtime(&now));
      system_log(CONFIG_SIGN, NULL, 0, _("Starting on <%s> %s"),
                 p_afd_status->hostname, dstr);
   }
   system_log(INFO_SIGN, NULL, 0, _("Starting %s (%s)"), AFD, PACKAGE_VERSION);
   system_log(DEBUG_SIGN, NULL, 0,
              _("AFD configuration: Default age limit         %d (sec)"),
              default_age_limit);

   /* Start the process AMG. */
   proc_table[AMG_NO].pid = make_process(AMG, work_dir, NULL);
   *(pid_t *)(pid_list + ((AMG_NO + 1) * sizeof(pid_t))) = proc_table[AMG_NO].pid;
   *proc_table[AMG_NO].status = ON;

   /* Start TCP info daemon of AFD. */
   if (afdd_port > 0)
   {
      proc_table[AFDD_NO].pid = make_process(AFDD, work_dir, NULL);
      *(pid_t *)(pid_list + ((AFDD_NO + 1) * sizeof(pid_t))) = proc_table[AFDD_NO].pid;
      *proc_table[AFDD_NO].status = ON;
   }
   else
   {
      proc_table[AFDD_NO].pid = -1;
      *proc_table[AFDD_NO].status = NEITHER;
   }

#ifdef _WITH_ATPD_SUPPORT
   /* Start AFD Transfer Protocol daemon. */
   proc_table[ATPD_NO].pid = make_process(ATPD, work_dir, NULL);
   *(pid_t *)(pid_list + ((ATPD_NO + 1) * sizeof(pid_t))) = proc_table[ATPD_NO].pid;
   *proc_table[ATPD_NO].status = ON;
#endif

#ifdef _WITH_WMOD_SUPPORT
   /* Start WMO Protocol daemon. */
   proc_table[WMOD_NO].pid = make_process(WMOD, work_dir, NULL);
   *(pid_t *)(pid_list + ((WMOD_NO + 1) * sizeof(pid_t))) = proc_table[WMOD_NO].pid;
   *proc_table[WMOD_NO].status = ON;
#endif

#ifdef _WITH_DE_MAIL_SUPPORT
   /* Start De Mail Confirmation daemon. */
   proc_table[DEMCD_NO].pid = make_process(DEMCD, work_dir, NULL);
   *(pid_t *)(pid_list + ((DEMCD_NO + 1) * sizeof(pid_t))) = proc_table[DEMCD_NO].pid;
   *proc_table[DEMCD_NO].status = ON;
#endif

#if ALDAD_OFFSET != 0
   /* Start ALDA daemon of AFD. */
   proc_table[ALDAD_NO].pid = make_process(ALDAD, work_dir, NULL);
   *(pid_t *)(pid_list + ((ALDAD_NO + 1) * sizeof(pid_t))) = proc_table[ALDAD_NO].pid;
   *proc_table[ALDAD_NO].status = ON;
#endif

   /*
    * Before starting the FD lets initialise all critical values
    * for this process.
    */
   p_afd_status->no_of_transfers = 0;
   if ((i = fsa_attach(AFD)) != SUCCESS)
   {
      if (i != INCORRECT_VERSION)
      {
         system_log(ERROR_SIGN, __FILE__, __LINE__,
                    _("Failed to attach to FSA."));
      }
      else
      {
         system_log(INFO_SIGN, __FILE__, __LINE__,
                    _("You can ignore the last warning about incorrect version."));
      }
   }
   else
   {
      int j;

      for (i = 0; i < no_of_hosts; i++)
      {
#ifdef WITH_IP_DB
         fsa[i].host_status |= STORE_IP;
#endif
         fsa[i].active_transfers = 0;
         for (j = 0; j < MAX_NO_PARALLEL_JOBS; j++)
         {
            fsa[i].job_status[j].no_of_files = 0;
            fsa[i].job_status[j].proc_id = -1;
            fsa[i].job_status[j].job_id = NO_ID;
            fsa[i].job_status[j].connect_status = DISCONNECT;
            fsa[i].job_status[j].file_name_in_use[0] = '\0';
         }
      }
      (void)fsa_detach(YES);
   }

#ifdef _LINK_MAX_TEST
   link_max = LINKY_MAX;
#else
# ifdef REDUCED_LINK_MAX
   link_max = REDUCED_LINK_MAX;
# else
   if ((link_max = pathconf(afd_file_dir, _PC_LINK_MAX)) == -1)
   {
      system_log(DEBUG_SIGN, __FILE__, __LINE__,
                 _("pathconf() _PC_LINK_MAX error, setting to %d : %s"),
                 _POSIX_LINK_MAX, strerror(errno));
      link_max = _POSIX_LINK_MAX;
   }
# endif
#endif

#ifdef HAVE_FDATASYNC
   if (fdatasync(afd_status_fd) == -1)
#else
   if (fsync(afd_status_fd) == -1)
#endif
   {
      system_log(WARN_SIGN, __FILE__, __LINE__,
                 _("Failed to sync `%s' file : %s"),
                 afd_status_file, strerror(errno));
   }
#ifndef MMAP_KILLER
   /*
    * In Irix 5.x there is a process fsr that always tries to
    * optimise the file system. This process however does not care
    * about memory mapped files! So, lets hope that when we leave
    * the memory mapped file open, it will leave this file
    * untouched.
    */
   if (close(afd_status_fd) == -1)
   {
      (void)fprintf(stderr, _("Failed to close() `%s' : %s (%s %d)\n"),
                    afd_status_file, strerror(errno), __FILE__, __LINE__);
   }
#endif

#ifdef HAVE_FDATASYNC
   if (fdatasync(afd_active_fd) == -1)
#else
   if (fsync(afd_active_fd) == -1)
#endif
   {
      system_log(WARN_SIGN, __FILE__, __LINE__,
                 _("Failed to sync AFD_ACTIVE file : %s"), strerror(errno));
   }

   /* Start init_afd_worker to help this process. */
   proc_table[AFD_WORKER_NO].pid = make_process(AFD_WORKER, work_dir, NULL);
   *(pid_t *)(pid_list + ((AFD_WORKER_NO + 1) * sizeof(pid_t))) = proc_table[AFD_WORKER_NO].pid;
   *proc_table[AFD_WORKER_NO].status = ON;

   /*
    * Watch if any of the two process (AMG, FD) dies.
    * While doing this wait and see if any commands or replies
    * are received via fifos.
    */
   FD_ZERO(&rset);
   for (;;)
   {
      (*heartbeat)++;
#ifdef HAVE_MMAP
      if (msync(pid_list, afd_active_size, MS_ASYNC) == -1)
#else
      if (msync_emu(pid_list) == -1)
#endif
      {
         system_log(WARN_SIGN, __FILE__, __LINE__,
                    _("msync() error : %s"), strerror(errno));
      }

      if (*shared_shutdown == SHUTDOWN)
      {
         system_log(INFO_SIGN, NULL, 0,
                    _("Shutdown bit is set, shutting down."));
         exit(SUCCESS);
      }

      /*
       * Lets write the month into the SYSTEM_LOG once only. So check
       * every day if a new month has been reached.
       */
      if (time(&now) > month_check_time)
      {
         system_log(DEBUG_SIGN, NULL, 0,
                    _("fork() syscalls AMG       : %18u FD : %18u => %u"),
                    p_afd_status->amg_fork_counter,
                    p_afd_status->fd_fork_counter,
                    p_afd_status->amg_fork_counter +
                    p_afd_status->fd_fork_counter);
         p_afd_status->amg_fork_counter = 0;
         p_afd_status->fd_fork_counter = 0;
#ifdef HAVE_WAIT4
         system_log(DEBUG_SIGN, NULL, 0,
                    _("child CPU user time AMG   : %11ld.%06ld FD : %11ld.%06ld"),
                    (long int)p_afd_status->amg_child_utime.tv_sec,
                    (long int)p_afd_status->amg_child_utime.tv_usec,
                    (long int)p_afd_status->fd_child_utime.tv_sec,
                    (long int)p_afd_status->fd_child_utime.tv_usec);
         p_afd_status->amg_child_utime.tv_sec = 0L;
         p_afd_status->amg_child_utime.tv_usec = 0L;
         p_afd_status->fd_child_utime.tv_sec = 0L;
         p_afd_status->fd_child_utime.tv_usec = 0L;
         system_log(DEBUG_SIGN, NULL, 0,
                    _("child CPU system time AMG : %11ld.%06ld FD : %11ld.%06ld"),
                    (long int)p_afd_status->amg_child_stime.tv_sec,
                    (long int)p_afd_status->amg_child_stime.tv_usec,
                    (long int)p_afd_status->fd_child_stime.tv_sec,
                    (long int)p_afd_status->fd_child_stime.tv_usec);
         p_afd_status->amg_child_stime.tv_sec = 0L;
         p_afd_status->amg_child_stime.tv_usec = 0L;
         p_afd_status->fd_child_stime.tv_sec = 0L;
         p_afd_status->fd_child_stime.tv_usec = 0L;
#endif
         system_log(DEBUG_SIGN, NULL, 0, _("Burst2 counter            : %u"),
                    p_afd_status->burst2_counter);
         p_afd_status->burst2_counter = 0;
         system_log(DEBUG_SIGN, NULL, 0, _("Max FD queue length       : %u"),
                    p_afd_status->max_queue_length);
         p_afd_status->max_queue_length = 0;
         system_log(DEBUG_SIGN, NULL, 0, _("Directories scanned       : %u"),
                    p_afd_status->dir_scans);
         p_afd_status->dir_scans = 0;
#ifdef WITH_INOTIFY
         system_log(DEBUG_SIGN, NULL, 0, _("Inotify events handled    : %u"),
                    p_afd_status->inotify_events);
         p_afd_status->inotify_events = 0;
#endif
         if ((bd_time = localtime(&now)) == NULL)
         {
            system_log(WARN_SIGN, __FILE__, __LINE__,
                       "localtime() error : %s", strerror(errno));
         }
         else
         {
            if (bd_time->tm_mon != current_month)
            {
               char date[20];

               (void)strftime(date, 20, "%B %Y", bd_time);
               system_log(DUMMY_SIGN, NULL, 0,
                          "=================> %s <=================", date);
               current_month = bd_time->tm_mon;
            }
         }
         month_check_time = ((now / 86400) * 86400) + 86400;
      }

      if (now > disabled_dir_check_time)
      {
         if ((check_disabled_dirs() == YES) && (stop_typ != STARTUP_ID))
         {
            if (fra_attach() == SUCCESS)
            {
               int    gotcha,
                      i, j;
               time_t current_time = time(NULL);

               /* First lets unset any dirs that have been removed. */
               for (i = 0; i < no_of_dirs; i++)
               {
                  if (fra[i].dir_flag & DIR_DISABLED_STATIC)
                  {
                     gotcha = NO;
                     for (j = 0; j < no_of_disabled_dirs; j++)
                     {
                        if (strcmp(fra[i].dir_alias, disabled_dirs[j]) == 0)
                        {
                           /* It is still set. */
                           gotcha = YES;
                           break;
                        }
                     }
                     if (gotcha == NO)
                     {
                        if (fra[i].dir_flag & DIR_DISABLED)
                        {
                           event_log(0L, EC_DIR, ET_AUTO, EA_ENABLE_DIRECTORY,
                                     "%s%cfrom config file %s",
                                     fra[i].dir_alias, SEPARATOR_CHAR,
                                     DISABLED_DIR_FILE);
                           fra[i].dir_flag &= ~DIR_DISABLED;
                           SET_DIR_STATUS(fra[i].dir_flag, current_time,
                                          fra[i].start_event_handle,
                                          fra[i].end_event_handle,
                                          fra[i].dir_status);
                        }
                        fra[i].dir_flag &= ~DIR_DISABLED_STATIC;
                     }
                  }
               }

               /* Set all host in list. */
               for (i = 0; i < no_of_disabled_dirs; i++)
               {
                  for (j = 0; j < no_of_dirs; j++)
                  {
                     if (strcmp(fra[j].dir_alias, disabled_dirs[i]) == 0)
                     {
                        if ((fra[j].dir_flag & DIR_DISABLED) == 0)
                        {
                           event_log(0L, EC_DIR, ET_AUTO, EA_DISABLE_DIRECTORY,
                                     "%s%cfrom config file %s",
                                     fra[j].dir_alias, SEPARATOR_CHAR,
                                     DISABLED_DIR_FILE);
                           fra[j].dir_flag |= DIR_DISABLED;
                           fra[j].dir_flag |= DIR_DISABLED_STATIC;
                           SET_DIR_STATUS(fra[j].dir_flag, current_time,
                                          fra[j].start_event_handle,
                                          fra[j].end_event_handle,
                                          fra[j].dir_status);

                           if (fra[j].host_alias[0] != '\0')
                           {
                              int  fd;
#ifdef WITHOUT_FIFO_RW_SUPPORT
                              int  readfd;
#endif
                              char fd_delete_fifo[MAX_PATH_LENGTH];

                              (void)sprintf(fd_delete_fifo, "%s%s%s",
                                            p_work_dir, FIFO_DIR,
                                            FD_DELETE_FIFO);
#ifdef WITHOUT_FIFO_RW_SUPPORT
                              if (open_fifo_rw(fd_delete_fifo, &readfd, &fd) == -1)
#else
                              if ((fd = open(fd_delete_fifo, O_RDWR)) == -1)
#endif
                              {
                                 (void)fprintf(stderr,
                                               _("Failed to open() %s : %s (%s %d)\n"),
                                               FD_DELETE_FIFO, strerror(errno),
                                               __FILE__, __LINE__);
                              }
                              else
                              {
                                 size_t length;
                                 char   wbuf[MAX_DIR_ALIAS_LENGTH + 2];

                                 wbuf[0] = DELETE_RETRIEVES_FROM_DIR;
                                 (void)strcpy(&wbuf[1], fra[j].dir_alias);
                                 length = 1 + strlen(fra[j].dir_alias) + 1;
                                 if (write(fd, wbuf, length) != length)
                                 {
                                    (void)fprintf(stderr,
                                                  _("Failed to write() to %s : %s (%s %d)\n"),
                                                  FD_DELETE_FIFO,
                                                  strerror(errno),
                                                  __FILE__, __LINE__);
                                 }
#ifdef WITHOUT_FIFO_RW_SUPPORT
                                 if (close(readfd) == -1)
                                 {
                                    system_log(DEBUG_SIGN, __FILE__, __LINE__,
                                               _("Failed to close() `%s' : %s"),
                                               FD_DELETE_FIFO, strerror(errno));
                                 }
#endif
                                 if (close(fd) == -1)
                                 {
                                    system_log(DEBUG_SIGN, __FILE__, __LINE__,
                                               _("Failed to close() `%s' : %s"),
                                               FD_DELETE_FIFO, strerror(errno));
                                 }
                              }
                           }
                        }
                        fra[j].dir_flag |= DIR_DISABLED_STATIC;
                        break;
                     }
                  }
               }
               (void)fra_detach();
            }
         }
         disabled_dir_check_time = ((now / 5) * 5) + 5;
      }

      /* Initialise descriptor set and timeout. */
      FD_SET(afd_cmd_fd, &rset);
      timeout.tv_usec = 0;
      timeout.tv_sec = AFD_RESCAN_TIME;

      /* Wait for message x seconds and then continue. */
      status = select(afd_cmd_fd + 1, &rset, NULL, NULL, &timeout);

      /* Did we get a timeout? */
      if (status == 0)
      {
         (*heartbeat)++;
#ifdef HAVE_MMAP
         if (msync(pid_list, afd_active_size, MS_ASYNC) == -1)
#else
         if (msync_emu(pid_list) == -1)
#endif
         {
            system_log(WARN_SIGN, __FILE__, __LINE__,
                       _("msync() error : %s"), strerror(errno));
         }
#ifdef HAVE_FDATASYNC
         if (in_global_filesystem)
         {
            if (fdatasync(afd_status_fd) == -1)
            {
               system_log(WARN_SIGN, __FILE__, __LINE__,
                          _("Failed to fdatasync() `%s' file : %s"),
                          afd_status_file, strerror(errno));
            }
            if (fdatasync(afd_active_fd) == -1)
            {
               system_log(WARN_SIGN, __FILE__, __LINE__,
                          _("Failed to fdatasync() `%s' file : %s"),
                          afd_active_file, strerror(errno));
            }
            if (fdatasync(fsa_fd) == -1)
            {
               system_log(WARN_SIGN, __FILE__, __LINE__,
                          _("Failed to fdatasync() FSA : %s"), strerror(errno));
            }
         }
#endif

         /* Check if all jobs are still running. */
         zombie_check();

         /* Check if there are any stuck transfer jobs. */
         stuck_transfer_check(time(&now));

         /*
          * See how many directories there are in file directory,
          * so we can show user the number of jobs in queue.
          */
         if (stat(afd_file_dir, &stat_buf) < 0)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       _("Failed to stat() %s : %s"),
                       afd_file_dir, strerror(errno));

            /*
             * Do not exit here. With network attached storage this
             * might be a temporary status.
             */
         }
         else
         {
            /*
             * If there are more then LINK_MAX directories stop the AMG.
             * Or else files will be lost!
             */
            if ((stat_buf.st_nlink > (link_max - STOP_AMG_THRESHOLD - DIRS_IN_FILE_DIR)) && (proc_table[AMG_NO].pid != 0))
            {
               /* Tell user that AMG is stopped. */
               system_log(ERROR_SIGN, __FILE__, __LINE__,
                          _("Have stopped AMG, due to to many jobs in system!"));
               system_log(INFO_SIGN, NULL, 0,
                          _("Will start AMG again when job counter is less then %d"),
                          (link_max - START_AMG_THRESHOLD + 1));
               event_log(0L, EC_GLOB, ET_AUTO, EA_AMG_STOP,
#if SIZEOF_NLINK_T == 4
                         _("To many jobs (%d) in system."),
#else
                         _("To many jobs (%lld) in system."),
#endif
                         (pri_nlink_t)stat_buf.st_nlink);
               auto_amg_stop = YES;

               if (send_cmd(STOP, amg_cmd_fd) < 0)
               {
                  system_log(WARN_SIGN, __FILE__, __LINE__,
                             _("Was not able to stop %s."), AMG);
               }
            }
            else
            {
               if ((auto_amg_stop == YES) &&
                   (stat_buf.st_nlink < (link_max - START_AMG_THRESHOLD)))
               {
                  if (proc_table[AMG_NO].pid < 1)
                  {
                     /* Restart the AMG */
                     proc_table[AMG_NO].pid = make_process(AMG, work_dir, NULL);
                     *(pid_t *)(pid_list + ((AMG_NO + 1) * sizeof(pid_t))) = proc_table[AMG_NO].pid;
                     *proc_table[AMG_NO].status = ON;
                     system_log(ERROR_SIGN, __FILE__, __LINE__,
                                _("Have started AMG, that was stopped due to too many jobs in the system!"));
                     event_log(0L, EC_GLOB, ET_AUTO, EA_AMG_STOP, NULL);
                  }
                  auto_amg_stop = NO;
               }
            }
         }
      }
           /* Did we get a message from the process */
           /* that controls the AFD?                */
      else if (FD_ISSET(afd_cmd_fd, &rset))
           {
              int  n;
              char buffer[DEFAULT_BUFFER_SIZE];

              if ((n = read(afd_cmd_fd, buffer, DEFAULT_BUFFER_SIZE)) > 0)
              {
                 int    count = 0;
                 time_t current_time;

                 /* Now evaluate all data read from fifo, byte after byte */
                 while (count < n)
                 {
                    (*heartbeat)++;
                    switch (buffer[count])
                    {
                       case SHUTDOWN  : /* Shutdown AFD */

                          /* Tell 'afd' that we received shutdown message */
                          (*heartbeat)++;
                          if (send_cmd(ACKN, afd_resp_fd) < 0)
                          {
                             system_log(ERROR_SIGN, __FILE__, __LINE__,
                                        _("Failed to send ACKN : %s"),
                                        strerror(errno));
                          }

                          if (proc_table[AFD_WORKER_NO].pid > 0)
                          {
                             int  fd;
#ifdef WITHOUT_FIFO_RW_SUPPORT
                             int  writefd;
#endif
                             char afd_worker_cmd[MAX_PATH_LENGTH];

                             (void)snprintf(afd_worker_cmd, MAX_PATH_LENGTH,
                                            "%s%s%s", p_work_dir, FIFO_DIR, AFD_WORKER_CMD_FIFO);
#ifdef WITHOUT_FIFO_RW_SUPPORT
                             if (open_fifo_rw(afd_worker_cmd, &fd, &write_fd) == -1)
#else
                             if ((fd = coe_open(afd_worker_cmd, O_RDWR)) == -1)
#endif
                             {
                                system_log(ERROR_SIGN, __FILE__, __LINE__,
                                           "Failed to open %s to send %s SHUTDOWN command : %s",
                                           afd_worker_cmd, AFD_WORKER, strerror(errno));
                             }
                             else
                             {
                                int   j;
                                pid_t pid;

                                p_afd_status->afd_worker = SHUTDOWN;
                                if (send_cmd(SHUTDOWN, fd) < 0)
                                {
                                   system_log(ERROR_SIGN, __FILE__, __LINE__,
                                              _("Failed to send SHUTDOWN to %s : %s"),
                                              AFD_WORKER, strerror(errno));
                                }
                                for (j = 0; j < MAX_SHUTDOWN_TIME; j++)
                                {
                                   if ((pid = waitpid(proc_table[AFD_WORKER_NO].pid, NULL, WNOHANG)) > 0)
                                   {
                                      if (pid == proc_table[AFD_WORKER_NO].pid)
                                      {
                                         proc_table[AFD_WORKER_NO].pid = 0;
                                         p_afd_status->afd_worker = STOPPED;
                                      }
                                      else
                                      {
                                         int gotcha = NO,
                                             k;

                                         for (k = 0; k < NO_OF_PROCESS; k++)
                                         {
                                            (*heartbeat)++;
                                            if (proc_table[k].pid == pid)
                                            {
                                               system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                                          _("Premature end of process %s (PID=%d), while waiting for %s."),
#else
                                                          _("Premature end of process %s (PID=%lld), while waiting for %s."),
#endif
                                                          proc_table[k].proc_name,
                                                          (pri_pid_t)proc_table[k].pid),
                                               proc_table[k].pid = 0;
                                               gotcha = YES;
                                               break;
                                            }
                                         }
                                         if (gotcha == NO)
                                         {
                                            system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                                       _("Caught some unknown zombie with PID %d while waiting for FD."),
#else
                                                       _("Caught some unknown zombie with PID ll%d while waiting for FD."),
#endif
                                                       (pri_pid_t)pid);
                                         }
                                      }
                                   }
                                   else
                                   {
                                      my_usleep(100000L);
                                   }
                                   if (proc_table[AFD_WORKER_NO].pid == 0)
                                   {
                                      break;
                                   }
                                }
                             }
                          }

                          if (proc_table[AMG_NO].pid > 0)
                          {
                             int   j;
                             pid_t pid;

                             /*
                              * Killing the AMG by sending it an SIGINT, is
                              * not a good idea. Lets wait for it to shutdown
                              * properly so it has time to finish writing
                              * its messages.
                              */
                             p_afd_status->amg = SHUTDOWN;
                             if (proc_table[FD_NO].pid > 0)
                             {
                                p_afd_status->fd = SHUTDOWN;
                             }
                             if (send_cmd(STOP, amg_cmd_fd) < 0)
                             {
                                system_log(WARN_SIGN, __FILE__, __LINE__,
                                           _("Was not able to stop %s."), AMG);
                             }
                             if (send_cmd(STOP, fd_cmd_fd) < 0)
                             {
                                system_log(WARN_SIGN, __FILE__, __LINE__,
                                          _("Was not able to stop %s."), FD);
                             }
                             for (j = 0; j < MAX_SHUTDOWN_TIME;  j++)
                             {
                                (*heartbeat)++;
                                if ((pid = waitpid(0, NULL, WNOHANG)) > 0)
                                {
                                   if (pid == proc_table[FD_NO].pid)
                                   {
                                      proc_table[FD_NO].pid = 0;
                                      p_afd_status->fd = STOPPED;
                                   }
                                   else if (pid == proc_table[AMG_NO].pid)
                                        {
                                           proc_table[AMG_NO].pid = 0;
                                           p_afd_status->amg = STOPPED;
                                        }
                                        else
                                        {
                                           int gotcha = NO,
                                               k;

                                           for (k = 0; k < NO_OF_PROCESS; k++)
                                           {
                                              if (proc_table[k].pid == pid)
                                              {
                                                 system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                                            _("Premature end of process %s (PID=%d), while waiting for %s."),
#else
                                                            _("Premature end of process %s (PID=%lld), while waiting for %s."),
#endif
                                                            proc_table[k].proc_name,
                                                            (pri_pid_t)proc_table[k].pid,
                                                            AMG);
                                                 proc_table[k].pid = 0;
                                                 gotcha = YES;
                                                 break;
                                              }
                                           }
                                           if (gotcha == NO)
                                           {
                                              system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                                         _("Caught some unknown zombie with PID %d while waiting for AMG."),
#else
                                                         _("Caught some unknown zombie with PID %lld while waiting for AMG."),
#endif
                                                         (pri_pid_t)pid);
                                           }
                                        }
                                }
                                else
                                {
                                   my_usleep(100000L);
                                }
                                if ((proc_table[FD_NO].pid == 0) &&
                                    (proc_table[AMG_NO].pid == 0))
                                {
                                   break;
                                }
                             }
                          }
                          else if (proc_table[FD_NO].pid > 0)
                               {
                                  int   j;
                                  pid_t pid;

                                  if (proc_table[FD_NO].pid > 0)
                                  {
                                     p_afd_status->fd = SHUTDOWN;
                                  }
                                  if (send_cmd(STOP, fd_cmd_fd) < 0)
                                  {
                                     system_log(WARN_SIGN, __FILE__, __LINE__,
                                                _("Was not able to stop %s."),
                                                FD);
                                  }
                                  for (j = 0; j < MAX_SHUTDOWN_TIME; j++)
                                  {
                                     (*heartbeat)++;
                                     if ((pid = waitpid(proc_table[FD_NO].pid, NULL, WNOHANG)) > 0)
                                     {
                                        if (pid == proc_table[FD_NO].pid)
                                        {
                                           proc_table[FD_NO].pid = 0;
                                           p_afd_status->fd = STOPPED;
                                        }
                                        else
                                        {
                                           int gotcha = NO,
                                               k;

                                           for (k = 0; k < NO_OF_PROCESS; k++)
                                           {
                                              if (proc_table[k].pid == pid)
                                              {
                                                 system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                                            _("Premature end of process %s (PID=%d), while waiting for %s."),
#else
                                                            _("Premature end of process %s (PID=%lld), while waiting for %s."),
#endif
                                                            proc_table[k].proc_name,
                                                            (pri_pid_t)proc_table[k].pid),
                                                 proc_table[k].pid = 0;
                                                 gotcha = YES;
                                                 break;
                                              }
                                           }
                                           if (gotcha == NO)
                                           {
                                              system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                                         _("Caught some unknown zombie with PID %d while waiting for FD."),
#else
                                                         _("Caught some unknown zombie with PID ll%d while waiting for FD."),
#endif
                                                         (pri_pid_t)pid);
                                           }
                                        }
                                     }
                                     else
                                     {
                                        my_usleep(100000L);
                                     }
                                     if (proc_table[FD_NO].pid == 0)
                                     {
                                        break;
                                     }
                                  }
                               }

                          current_time = time(NULL);
                          (void)memset(buffer, '-', 35 + AFD_LENGTH);
                          buffer[35 + AFD_LENGTH] = '\0';
                          (void)fprintf(stderr, _("%.24s : Stopped %s\n%s\n"),
                                        ctime(&current_time), AFD, buffer);

                          /* Shutdown of other process is handled by */
                          /* the exit handler.                       */
                          exit(SUCCESS);

                       case STOP      : /* Stop all process of the AFD */

                          stop_typ = ALL_ID;

                          /* Send all process the stop signal (via fifo) */
                          if (p_afd_status->amg == ON)
                          {
                             p_afd_status->amg = SHUTDOWN;
                          }
                          if (p_afd_status->fd == ON)
                          {
                             p_afd_status->fd = SHUTDOWN;
                          }
                          if (send_cmd(STOP, amg_cmd_fd) < 0)
                          {
                             system_log(WARN_SIGN, __FILE__, __LINE__,
                                        _("Was not able to stop %s."), AMG);
                          }
                          if (send_cmd(STOP, fd_cmd_fd) < 0)
                          {
                             system_log(WARN_SIGN, __FILE__, __LINE__,
                                        _("Was not able to stop %s."), FD);
                          }

                          break;

                       case STOP_AMG  : /* Only stop the AMG */

                          stop_typ = AMG_ID;
                          if (p_afd_status->amg == ON)
                          {
                             p_afd_status->amg = SHUTDOWN;
                          }
                          if (send_cmd(STOP, amg_cmd_fd) < 0)
                          {
                             system_log(WARN_SIGN, __FILE__, __LINE__,
                                        _("Was not able to stop %s."), AMG);
                          }
                          break;

                       case STOP_FD   : /* Only stop the FD */

                          stop_typ = FD_ID;
                          if (p_afd_status->fd == ON)
                          {
                             p_afd_status->fd = SHUTDOWN;
                          }
                          if (send_cmd(QUICK_STOP, fd_cmd_fd) < 0)
                          {
                             system_log(WARN_SIGN, __FILE__, __LINE__,
                                        _("Was not able to stop %s."), FD);
                          }
                          break;

                       case START_AMG : /* Start the AMG */

                          /* First check if it is running */
                          if (proc_table[AMG_NO].pid > 0)
                          {
                             system_log(INFO_SIGN, __FILE__, __LINE__,
                                        _("%s is already running."), AMG);
                          }
                          else
                          {
                             proc_table[AMG_NO].pid = make_process(AMG,
                                                                   work_dir,
                                                                   NULL);
                             *(pid_t *)(pid_list + ((AMG_NO + 1) * sizeof(pid_t))) = proc_table[AMG_NO].pid;
                             *proc_table[AMG_NO].status = ON;
                             stop_typ = NONE_ID;
                          }
                          break;
                          
                       case START_FD  : /* Start the FD */

                          /* First check if it is running */
                          if (proc_table[FD_NO].pid > 0)
                          {
                             system_log(INFO_SIGN, __FILE__, __LINE__,
                                        _("%s is already running."), FD);
                          }
                          else
                          {
                             proc_table[FD_NO].pid = make_process(FD, work_dir,
                                                                  NULL);
                             *(pid_t *)(pid_list + ((FD_NO + 1) * sizeof(pid_t))) = proc_table[FD_NO].pid;
                             *proc_table[FD_NO].status = ON;
                             stop_typ = NONE_ID;
                          }
                          break;
                          
                       case AMG_READY : /* AMG has successfully executed the command */

                          /* Tell afd startup procedure that it may */
                          /* start afd_ctrl.                        */
                          (*heartbeat)++;
                          if (send_cmd(ACKN, probe_only_fd) < 0)
                          {
                             system_log(WARN_SIGN, __FILE__, __LINE__,
                                        _("Was not able to send acknowledge via fifo."));
                             exit(INCORRECT);
                          }

                          if (stop_typ == ALL_ID)
                          {
                             proc_table[AMG_NO].pid = 0;
                          }
                          else if (stop_typ == AMG_ID)
                               {
                                  proc_table[AMG_NO].pid = 0;
                                  stop_typ = NONE_ID;
                               }
                               /* We did not send any stop message. The */
                               /* AMG was started the first time and we */
                               /* may now start the next process.       */
                          else if (stop_typ == STARTUP_ID)
                               {
                                  /* Start the AFD_STAT */
                                  proc_table[STAT_NO].pid = make_process(AFD_STAT, work_dir, NULL);
                                  *(pid_t *)(pid_list + ((STAT_NO + 1) * sizeof(pid_t))) = proc_table[STAT_NO].pid;
                                  *proc_table[STAT_NO].status = ON;

#ifdef _TRANSFER_RATE_LOG
                                  proc_table[TRANSFER_RATE_LOG_NO].pid = make_process(TRLOG, work_dir, NULL);
                                  *(pid_t *)(pid_list + ((TRANSFER_RATE_LOG_NO + 1) * sizeof(pid_t))) = proc_table[TRANSFER_RATE_LOG_NO].pid;
                                  *proc_table[TRANSFER_RATE_LOG_NO].status = ON;
#endif
                                  if (fra_attach() == SUCCESS)
                                  {
                                     int    gotcha,
                                            i, j;
                                     time_t current_time = time(NULL);

                                     /* First lets unset any dirs that have been removed. */
                                     for (i = 0; i < no_of_dirs; i++)
                                     {
                                        if (fra[i].dir_flag & DIR_DISABLED_STATIC)
                                        {
                                           gotcha = NO;
                                           for (j = 0; j < no_of_disabled_dirs; j++)
                                           {
                                              if (strcmp(fra[i].dir_alias,
                                                         disabled_dirs[j]) == 0)
                                              {
                                                 /* It is still set. */
                                                 gotcha = YES;
                                                 break;
                                              }
                                           }
                                           if (gotcha == NO)
                                           {
                                              if (fra[i].dir_flag & DIR_DISABLED)
                                              {
                                                 event_log(0L, EC_DIR, ET_AUTO, EA_ENABLE_DIRECTORY,
                                                           "%s%cfrom config file %s",
                                                           fra[i].dir_alias, SEPARATOR_CHAR,
                                                           DISABLED_DIR_FILE);
                                                 fra[i].dir_flag &= ~DIR_DISABLED;
                                                 SET_DIR_STATUS(fra[i].dir_flag,
                                                                current_time,
                                                                fra[i].start_event_handle,
                                                                fra[i].end_event_handle,
                                                                fra[i].dir_status);
                                              }
                                              fra[i].dir_flag &= ~DIR_DISABLED_STATIC;
                                           }
                                        }
                                     }

                                     /* Set all host in list. */
                                     for (i = 0; i < no_of_disabled_dirs; i++)
                                     {
                                        for (j = 0; j < no_of_dirs; j++)
                                        {
                                           if (strcmp(fra[j].dir_alias,
                                                      disabled_dirs[i]) == 0)
                                           {
                                              if ((fra[j].dir_flag & DIR_DISABLED) == 0)
                                              {
                                                 event_log(0L, EC_DIR, ET_AUTO, EA_DISABLE_DIRECTORY,
                                                           "%s%cfrom config file %s",
                                                           fra[j].dir_alias,
                                                           SEPARATOR_CHAR,
                                                           DISABLED_DIR_FILE);
                                                 fra[j].dir_flag |= DIR_DISABLED;
                                                 fra[j].dir_flag |= DIR_DISABLED_STATIC;
                                                 SET_DIR_STATUS(fra[j].dir_flag,
                                                                current_time,
                                                                fra[j].start_event_handle,
                                                                fra[j].end_event_handle,
                                                                fra[j].dir_status);
                                              }
                                              fra[j].dir_flag |= DIR_DISABLED_STATIC;
                                              break;
                                           }
                                        }
                                     }
                                     (void)fra_detach();
                                  }

                                  /* Attach to the FSA */
                                  if (fsa_attach(AFD) != SUCCESS)
                                  {
                                     system_log(ERROR_SIGN, __FILE__, __LINE__,
                                               _("Failed to attach to FSA."));
                                  }

                                  /* Start the FD */
                                  proc_table[FD_NO].pid  = make_process(FD,
                                                                        work_dir,
                                                                        NULL);
                                  *(pid_t *)(pid_list + ((FD_NO + 1) * sizeof(pid_t))) = proc_table[FD_NO].pid;
                                  *proc_table[FD_NO].status = ON;
                                  stop_typ = NONE_ID;

                                  check_permissions();
                               }
                          else if (stop_typ != NONE_ID)
                               {
                                  system_log(WARN_SIGN, __FILE__, __LINE__,
                                             _("Unknown stop_typ (%d)"),
                                             stop_typ);
                               }
                          break;

                       case IS_ALIVE  : /* Somebody wants to know whether an AFD */
                                        /* is running in this directory. Quickly */
                                        /* lets answer or we will be killed!     */
                          (*heartbeat)++;
                          if (send_cmd(ACKN, probe_only_fd) < 0)
                          {
                             system_log(WARN_SIGN, __FILE__, __LINE__,
                                        _("Was not able to send acknowledge via fifo."));
                             exit(INCORRECT);
                          }
                          break;

                       default        : /* Reading garbage from fifo */

                          system_log(ERROR_SIGN, __FILE__, __LINE__,
                                     _("Reading garbage on AFD command fifo [%d]. Ignoring."),
                                     (int)buffer[count]);
                          break;
                    }
                    count++;
                 } /* while (count < n) */
              } /* if (n > 0) */
           }
                /* An error has occurred */
           else if (status < 0)
                {
                   system_log(FATAL_SIGN, __FILE__, __LINE__,
                              _("select() error : %s"), strerror(errno));
                   exit(INCORRECT);
                }
                else
                {
                   system_log(FATAL_SIGN, __FILE__, __LINE__,
                              _("Unknown condition."));
                   exit(INCORRECT);
                }
   } /* for (;;) */
   
   exit(SUCCESS);
}


/*+++++++++++++++++++++++++ get_afd_config_value() ++++++++++++++++++++++*/
static void
get_afd_config_value(int          *afdd_port,
                     unsigned int *default_age_limit,
                     int          *in_global_filesystem)
{
   char *buffer,
        config_file[MAX_PATH_LENGTH];

   (void)snprintf(config_file, MAX_PATH_LENGTH, "%s%s%s",
                  p_work_dir, ETC_DIR, AFD_CONFIG_FILE);
   if ((eaccess(config_file, F_OK) == 0) &&
       (read_file_no_cr(config_file, &buffer, YES, __FILE__, __LINE__) != INCORRECT))
   {
      char value[MAX_INT_LENGTH];

#ifdef HAVE_SETPRIORITY
      if (get_definition(buffer, INIT_AFD_PRIORITY_DEF,
                         value, MAX_INT_LENGTH) != NULL)
      {
         if (setpriority(PRIO_PROCESS, 0, atoi(value)) == -1)
         {
            system_log(WARN_SIGN, __FILE__, __LINE__,
                       "Failed to set priority to %d : %s",
                       atoi(value), strerror(errno));
         }
      }
#endif
      if (get_definition(buffer, AFD_TCP_PORT_DEF,
                         value, MAX_INT_LENGTH) != NULL)
      {
         *afdd_port = atoi(value);

         /* Note: Port range is checked by afdd process. */
      }
      else
      {
         *afdd_port = -1;
      }
      if (get_definition(buffer, DEFAULT_AGE_LIMIT_DEF,
                         value, MAX_INT_LENGTH) != NULL)
      {
         *default_age_limit = (unsigned int)atoi(value);
      }
      else
      {
         *default_age_limit = DEFAULT_AGE_LIMIT;
      }
      if (get_definition(buffer, IN_GLOBAL_FILESYSTEM_DEF,
                         value, MAX_INT_LENGTH) != NULL)
      {
         if ((value[0] == '\0') ||
             (((value[0] == 'Y') || (value[0] == 'y')) &&
              ((value[1] == 'E') || (value[1] == 'e')) &&
              ((value[2] == 'S') || (value[2] == 's')) &&
              (value[3] == '\0')))
         {
            *in_global_filesystem = YES;
         }
         else
         {
            *in_global_filesystem = NO;
         }
      }
      else
      {
         *in_global_filesystem = NO;
      }
      free(buffer);
   }
   else
   {
      *afdd_port = -1;
      *default_age_limit = DEFAULT_AGE_LIMIT;
      *in_global_filesystem = NO;
   }

   return;
}


/*+++++++++++++++++++++++++++++ check_dirs() +++++++++++++++++++++++++++*/
static void
check_dirs(char *work_dir)
{
   int                    tmp_sys_log_fd = sys_log_fd;
   char                   new_dir[MAX_PATH_LENGTH],
                          *ptr2,
                          *ptr;
   struct stat            stat_buf;
#ifdef MULTI_FS_SUPPORT
   int                    no_of_extra_work_dirs;
   struct extra_work_dirs *ewl;
#endif

   /* First check that the working directory does exist */
   /* and make sure that it is a directory.             */
   sys_log_fd = STDOUT_FILENO;
   if (stat(work_dir, &stat_buf) < 0)
   {
      (void)fprintf(stderr, _("Could not stat() `%s' : %s (%s %d)\n"),
                    work_dir, strerror(errno), __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   if (!S_ISDIR(stat_buf.st_mode))
   {
      (void)fprintf(stderr, _("`%s' is not a directory. (%s %d)\n"),
                    work_dir, __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Now lets check if the fifo directory is there. */
   (void)strcpy(new_dir, work_dir);
   ptr = new_dir + strlen(new_dir);
   (void)strcpy(ptr, FIFO_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is messages directory there? */
   (void)strcpy(ptr, AFD_MSG_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is log directory there? */
   (void)strcpy(ptr, LOG_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is archive directory there? */
   (void)strcpy(ptr, AFD_ARCHIVE_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

#ifdef WITH_ONETIME
   /* Is messages directory there? */
   (void)strcpy(ptr, AFD_ONETIME_DIR);
   ptr2 = ptr;
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is onetime/log directory there? */
   ptr += AFD_ONETIME_DIR_LENGTH;
   (void)strcpy(ptr, LOG_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is onetime/etc directory there? */
   (void)strcpy(ptr, ETC_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is onetime/etc/list directory there? */
   ptr += ETC_DIR_LENGTH;
   (void)strcpy(ptr, AFD_LIST_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is onetime/etc/config directory there? */
   (void)strcpy(ptr, AFD_CONFIG_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr = ptr2;
#endif

   /* Is etc/groups directory there? */
   ptr2 = ptr;
   (void)strcpy(ptr, ETC_DIR);
   ptr += ETC_DIR_LENGTH;
   (void)strcpy(ptr, GROUP_NAME_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr += GROUP_NAME_DIR_LENGTH;
   (void)strcpy(ptr, SOURCE_GROUP_NAME);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, RECIPIENT_GROUP_NAME);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, FILE_GROUP_NAME);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is etc/info directory there? */
   ptr = ptr2 + ETC_DIR_LENGTH;
   (void)strcpy(ptr, INFO_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is etc/action directory there? */
   ptr = ptr2 + ETC_DIR_LENGTH;
   (void)strcpy(ptr, ACTION_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr += ACTION_DIR_LENGTH;
   (void)strcpy(ptr, ACTION_TARGET_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr += ACTION_TARGET_DIR_LENGTH;
   (void)strcpy(ptr, ACTION_ERROR_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, ACTION_WARN_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, ACTION_INFO_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, ACTION_SUCCESS_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr = ptr2 + ETC_DIR_LENGTH + ACTION_DIR_LENGTH;
   (void)strcpy(ptr, ACTION_SOURCE_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr += ACTION_SOURCE_DIR_LENGTH;
   (void)strcpy(ptr, ACTION_ERROR_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, ACTION_WARN_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, ACTION_INFO_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   (void)strcpy(ptr, ACTION_SUCCESS_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   ptr = ptr2;

   /* Is file directory there? */
   (void)strcpy(ptr, AFD_FILE_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is outgoing file directory there? */
   ptr += AFD_FILE_DIR_LENGTH;
   (void)strcpy(ptr, OUTGOING_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

#ifdef WITH_DUP_CHECK
   /* Is store directory there? */
   (void)strcpy(ptr, STORE_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is CRC directory there? */
   (void)strcpy(ptr, CRC_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
#endif

   /* Is the temp pool file directory there? */
   (void)strcpy(ptr, AFD_TMP_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is the time pool file directory there? */
   (void)strcpy(ptr, AFD_TIME_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is the incoming file directory there? */
   (void)strcpy(ptr, INCOMING_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is the file mask directory there? */
   ptr += INCOMING_DIR_LENGTH;
   (void)strcpy(ptr, FILE_MASK_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

   /* Is the ls data from remote hosts directory there? */
   (void)strcpy(ptr, LS_DATA_DIR);
   if (check_dir(new_dir, R_OK | W_OK | X_OK) < 0)
   {
      (void)fprintf(stderr, "Failed to check directory %s\n", new_dir);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }

#ifdef MULTI_FS_SUPPORT
   get_extra_work_dirs(NULL, &no_of_extra_work_dirs, &ewl, YES);
   if (no_of_extra_work_dirs > 0)
   {
      int i;

      for (i = 0; i < no_of_extra_work_dirs; i++)
      {
         if (stat(ewl[i].dir_name, &stat_buf) < 0)
         {
            (void)fprintf(stderr, _("Could not stat() `%s' : %s (%s %d)\n"),
                          ewl[i].dir_name, strerror(errno), __FILE__, __LINE__);
            if (i == 0)
            {
               (void)unlink(afd_active_file);
               exit(INCORRECT);
            }
         }
         if (!S_ISDIR(stat_buf.st_mode))
         {
            (void)fprintf(stderr, _("`%s' is not a directory. (%s %d)\n"),
                          ewl[i].dir_name, __FILE__, __LINE__);
            if (i == 0)
            {
               (void)unlink(afd_active_file);
               exit(INCORRECT);
            }
         }
      }
   }
   else
   {
      (void)fprintf(stderr,
                    _("Failed to locate any valid working directories. (%s %d)\n"),
                    __FILE__, __LINE__);
      (void)unlink(afd_active_file);
      exit(INCORRECT);
   }
   delete_stale_extra_work_dir_links(no_of_extra_work_dirs, ewl);
   free_extra_work_dirs(no_of_extra_work_dirs, &ewl);
#endif
   sys_log_fd = tmp_sys_log_fd;

   return;
}


/*+++++++++++++++++++ delete_old_afd_status_files() ++++++++++++++++++++*/
static void
delete_old_afd_status_files(void)
{
   char fifo_dir[MAX_PATH_LENGTH],
        *p_fifo_dir;
   DIR  *dp;

   p_fifo_dir = fifo_dir + snprintf(fifo_dir, MAX_PATH_LENGTH, "%s%s",
                                    p_work_dir, FIFO_DIR);
   if ((dp = opendir(fifo_dir)) == NULL)
   {
      (void)fprintf(stderr, "Could not opendir() `%s' : %s (%s %d)\n",
                    fifo_dir, strerror(errno), __FILE__, __LINE__);
   }
   else
   {
      int           current_afd_status_file_length;
      char          current_afd_status_file[AFD_STATUS_FILE_LENGTH + 1 + MAX_INT_HEX_LENGTH + 1];
      struct dirent *p_dir;

      current_afd_status_file_length = snprintf(current_afd_status_file,
                                                AFD_STATUS_FILE_LENGTH + 1 + MAX_INT_HEX_LENGTH + 1,
                                                "%s.%x",
                                                AFD_STATUS_FILE,
                                                get_afd_status_struct_size());
      *p_fifo_dir = '/';
      errno = 0;
      while ((p_dir = readdir(dp)) != NULL)
      {
         if (p_dir->d_name[0] == '.')
         {
            continue;
         }
         if ((strncmp(p_dir->d_name, AFD_STATUS_FILE,
                      AFD_STATUS_FILE_LENGTH - 1) == 0) &&
             (memcmp(p_dir->d_name, current_afd_status_file,
                     current_afd_status_file_length + 1) != 0))
         {
            (void)strcpy(p_fifo_dir + 1, p_dir->d_name);
            if (unlink(fifo_dir) == -1)
            {
               (void)fprintf(stderr, "Could not unlink() `%s' : %s (%s %d)\n",
                             fifo_dir, strerror(errno), __FILE__, __LINE__);
            }
            else
            {
               (void)fprintf(stderr, "INFO: Removed %s (%s %d)\n",
                             fifo_dir, __FILE__, __LINE__);
            }
         }
         errno = 0;
      } /*  while (readdir(dp) != NULL) */

      if (errno)
      {
         *p_fifo_dir = '\0';
         (void)fprintf(stderr, "readdir() error `%s' : %s (%s %d)\n",
                       fifo_dir, strerror(errno), __FILE__, __LINE__);
      }
      if (closedir(dp) == -1)
      {
         *p_fifo_dir = '\0';
         (void)fprintf(stderr, "Could not closedir() `%s' : %s (%s %d)\n",
                       fifo_dir, strerror(errno), __FILE__, __LINE__);
      }
   }

   return;
}


/*++++++++++++++++++++++++++++ make_process() ++++++++++++++++++++++++++*/
static pid_t
make_process(char *progname, char *directory, sigset_t *oldmask)
{
   pid_t pid;

   switch (pid = fork())
   {
      case -1: /* Could not generate process */

         system_log(FATAL_SIGN, __FILE__, __LINE__,
                    _("Could not create a new process : %s"), strerror(errno));
         exit(INCORRECT);

      case  0: /* Child process */

         if (oldmask != NULL)
         {
            if (sigprocmask(SIG_SETMASK, oldmask, NULL) < 0)
            {
               system_log(ERROR_SIGN, __FILE__, __LINE__,
                          _("sigprocmask() error : %s"),
                          strerror(errno));
            }
         }
         if (execlp(progname, progname, WORK_DIR_ID, directory, (char *)0) < 0)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       _("Failed to start process %s : %s"),
                       progname, strerror(errno));
            _exit(INCORRECT);
         }
         exit(SUCCESS);

      default: /* Parent process */

         return(pid);
   }
}


/*+++++++++++++++++++++++++++++ zombie_check() +++++++++++++++++++++++++*/
/*
 * Description : Checks if any process is finished (zombie), if this
 *               is the case it is killed with waitpid().
 */
static void
zombie_check(void)
{
   int status,
       i;

   for (i = 0; i < NO_OF_PROCESS; i++)
   {
      if ((proc_table[i].pid > 0) &&
          (waitpid(proc_table[i].pid, &status, WNOHANG) > 0))
      {
         if (WIFEXITED(status))
         {
            switch (WEXITSTATUS(status))
            {
               case 0 : /* Ordinary end of process. */
                  system_log(INFO_SIGN, __FILE__, __LINE__,
                             _("<INIT> Normal termination of process %s"),
                             proc_table[i].proc_name);
                  proc_table[i].pid = 0;
                  *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = 0;
                  *proc_table[i].status = STOPPED;
                  break;

               case 1 : /* Process has been stopped by user */
                  break;

               case 2 : /* Process has received SIGHUP */
                  proc_table[i].pid = make_process(proc_table[i].proc_name,
                                                   p_work_dir, NULL);
                  *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = proc_table[i].pid;
                  *proc_table[i].status = ON;
                  system_log(INFO_SIGN, __FILE__, __LINE__,
                             _("<INIT> Have restarted %s. SIGHUP received!"),
                             proc_table[i].proc_name);
                  break;

               case 3 : /* Shared memory region gone. Restart. */
                  proc_table[i].pid = make_process(proc_table[i].proc_name,
                                                   p_work_dir, NULL);
                  *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = proc_table[i].pid;
                  *proc_table[i].status = ON;
                  system_log(INFO_SIGN, __FILE__, __LINE__,
                             _("<INIT> Have restarted %s, due to missing shared memory area."),
                             proc_table[i].proc_name);
                  break;

               case PROCESS_NEEDS_RESTART : /* Process requests a restart. */
                  proc_table[i].pid = make_process(proc_table[i].proc_name,
                                                   p_work_dir, NULL);
                  *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = proc_table[i].pid;
                  *proc_table[i].status = ON;
                  system_log(INFO_SIGN, __FILE__, __LINE__,
                             _("<INIT> Have restarted %s, due to process requesting a restart."),
                             proc_table[i].proc_name);
                  break;

               default: /* Unknown error. Assume process has died. */
                  {
#ifdef BLOCK_SIGNALS
                     sigset_t         newmask,
                                      oldmask;
                     struct sigaction newact,
                                      oldact_sig_exit;

                     newact.sa_handler = sig_exit;
                     sigemptyset(&newact.sa_mask);
                     newact.sa_flags = 0;
                     if ((sigaction(SIGINT, &newact, &oldact_sig_exit) < 0) ||
                         (sigaction(SIGTERM, &newact, &oldact_sig_exit) < 0))
                     {
                     }
                     sigemptyset(&newmask);
                     sigaddset(&newmask, SIGINT);
                     sigaddset(&newmask, SIGTERM);
                     if (sigprocmask(SIG_BLOCK, &newmask, &oldmask) < 0)
                     {
                        system_log(ERROR_SIGN, __FILE__, __LINE__,
                                   _("sigprocmask() error : %s"),
                                   strerror(errno));
                     }
#endif
                     proc_table[i].pid = 0;
                     *proc_table[i].status = OFF;
                     system_log(ERROR_SIGN, __FILE__, __LINE__,
                                _("<INIT> Process %s has died!"),
                                proc_table[i].proc_name);

                     /* The log and archive process may never die! */
                     if ((i == SLOG_NO) || (i == ELOG_NO) || (i == TLOG_NO) ||
                         (i == RLOG_NO) || (i == FD_NO) ||
#ifndef HAVE_MMAP
                         (i == MAPPER_NO) ||
#endif
#ifdef _WITH_ATPD_SUPPORT
                         (i == ATPD_NO) ||
#endif
#ifdef _WITH_WMOD_SUPPORT
                         (i == WMOD_NO) ||
#endif
#ifdef _WITH_DE_MAIL_SUPPORT
                         (i == DEMCD_NO) ||
#endif
#if ALDAD_OFFSET != 0
                         (i == ALDAD_NO) ||
#endif
                         (i == TDBLOG_NO) || (i == AW_NO) ||
                         (i == AFDD_NO) || (i == STAT_NO) ||
                         (i == AFD_WORKER_NO))
                     {
                        proc_table[i].pid = make_process(proc_table[i].proc_name,
#ifdef BLOCK_SIGNALS
                                                         p_work_dir, &oldmask);
#else
                                                         p_work_dir, NULL);
#endif
                        *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = proc_table[i].pid;
                        *proc_table[i].status = ON;
                        system_log(INFO_SIGN, __FILE__, __LINE__,
                                   _("<INIT> Have restarted %s"),
                                   proc_table[i].proc_name);
                     }
#ifdef BLOCK_SIGNALS
                     if ((sigaction(SIGINT, &oldact_sig_exit, NULL) < 0) ||
                         (sigaction(SIGTERM, &oldact_sig_exit, NULL) < 0))
                     {
                        system_log(WARN_SIGN, __FILE__, __LINE__,
                                   "Failed to restablish a signal handler for SIGINT and/or SIGTERM : %s",
                                   strerror(errno));
                     }
                     if (sigprocmask(SIG_SETMASK, &oldmask, NULL) < 0)
                     {
                        system_log(ERROR_SIGN, __FILE__, __LINE__,
                                   _("sigprocmask() error : %s"),
                                   strerror(errno));
                     }
#endif
                  }
                  break;
            }
         }
         else if (WIFSIGNALED(status))
              {
#ifdef NO_OF_SAVED_CORE_FILES
                 static int no_of_saved_cores = 0;
#endif

                 /* abnormal termination */
                 proc_table[i].pid = 0;
                 *proc_table[i].status = OFF;
                 system_log(ERROR_SIGN, __FILE__, __LINE__,
                            _("<INIT> Abnormal termination of %s, caused by signal %d!"),
                            proc_table[i].proc_name, WTERMSIG(status));
#ifdef NO_OF_SAVED_CORE_FILES
                 if (no_of_saved_cores < NO_OF_SAVED_CORE_FILES)
                 {
                    char        core_file[MAX_PATH_LENGTH];
                    struct stat stat_buf;

                    (void)snprintf(core_file, MAX_PATH_LENGTH,
                                   "%s/core", p_work_dir);
                    if (stat(core_file, &stat_buf) != -1)
                    {
                       char new_core_file[MAX_PATH_LENGTH + 51];

                       (void)snprintf(new_core_file, MAX_PATH_LENGTH + 51,
# if SIZEOF_TIME_T == 4
                                      "%s.%s.%ld.%d",
# else
                                      "%s.%s.%lld.%d",
# endif
                                      core_file, proc_table[i].proc_name,
                                      (pri_time_t)time(NULL), no_of_saved_cores);
                       if (rename(core_file, new_core_file) == -1)
                       {
                          system_log(DEBUG_SIGN, __FILE__, __LINE__,
                                     _("Failed to rename() `%s' to `%s' : %s"),
                                     core_file, new_core_file, strerror(errno));
                       }
                       else
                       {
                          no_of_saved_cores++;
                       }
                    }
                 }
#endif /* NO_OF_SAVED_CORE_FILES */

                 /* No process may end abnormally! */
                 proc_table[i].pid = make_process(proc_table[i].proc_name,
                                                  p_work_dir, NULL);
                 *(pid_t *)(pid_list + ((i + 1) * sizeof(pid_t))) = proc_table[i].pid;
                 *proc_table[i].status = ON;
                 system_log(INFO_SIGN, __FILE__, __LINE__,
                            _("<INIT> Have restarted %s"),
                            proc_table[i].proc_name);
              }
              else if (WIFSTOPPED(status))
                   {
                      /* Child stopped */
                      system_log(ERROR_SIGN, __FILE__, __LINE__,
                                 _("<INIT> Process %s has been put to sleep!"),
                                 proc_table[i].proc_name);
                   }
      }
   }

   return;
}


/*+++++++++++++++++++++++++ stuck_transfer_check() ++++++++++++++++++++++*/
/*
 * Description : Checks if any transfer process is stuck. If such a
 *               process is found a kill signal is send.
 */
static void
stuck_transfer_check(time_t current_time)
{
#ifdef THIS_CODE_IS_BROKEN /* We need to remember byte activity! */
   if ((fsa != NULL) && (current_time > p_afd_status->start_time) &&
       ((current_time - p_afd_status->start_time) > 600))
   {
      int i, j;

      (void)check_fsa(NO, AFD);

      for (i = 0; i < no_of_hosts; i++)
      {
         if ((fsa[i].active_transfers > 0) && (fsa[i].error_counter > 0) &&
             ((fsa[i].host_status & STOP_TRANSFER_STAT) == 0) &&
             ((current_time - (fsa[i].last_retry_time + (fsa[i].retry_interval + fsa[i].transfer_timeout + 660))) >= 0))
         {
            for (j = 0; j < fsa[i].allowed_transfers; j++)
            {
               if (fsa[i].job_status[j].proc_id > 0)
               {
                  if (kill(fsa[i].job_status[j].proc_id, SIGINT) < 0)
                  {
                     if (errno != ESRCH)
                     {
                        system_log(WARN_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                   _("<INIT> Failed to send kill signal to possible stuck transfer job to %s (%d) : %s"),
#else
                                   _("<INIT> Failed to send kill signal to possible stuck transfer job to %s (%lld) : %s"),
#endif
                                   fsa[i].host_alias,
                                   (pri_pid_t)fsa[i].job_status[j].proc_id,
                                   strerror(errno));
                     }
                  }
                  else
                  {
                     system_log(WARN_SIGN, __FILE__, __LINE__,
#if SIZEOF_PID_T == 4
                                _("<INIT> Send kill signal to possible stuck transfer job to %s (%d)"),
#else
                                _("<INIT> Send kill signal to possible stuck transfer job to %s (%lld)"),
#endif
                                fsa[i].host_alias,
                                (pri_pid_t)fsa[i].job_status[j].proc_id);

                      /* Process fd will take care of the zombie. */
                  }
               }
            }
         }
      }
   }
#endif

   return;
}


/*++++++++++++++++++++++++++++++ afd_exit() +++++++++++++++++++++++++++++*/
static void
afd_exit(void)
{
   char        *buffer,
               *ptr;
   struct stat stat_buf;

   if (probe_only != 1)
   {
      int   i;
      pid_t syslog = 0;

      system_log(INFO_SIGN, NULL, 0,
                 _("Stopped %s. (%s)"), AFD, PACKAGE_VERSION);

      if (afd_active_fd == -1)
      {
         int read_fd;

         if ((read_fd = open(afd_active_file, O_RDONLY)) == -1)
         {
            system_log(FATAL_SIGN, __FILE__, __LINE__,
                       _("Failed to open `%s' : %s"),
                       afd_active_file, strerror(errno));
            _exit(INCORRECT);
         }
         if (fstat(read_fd, &stat_buf) == -1)
         {
            system_log(FATAL_SIGN, __FILE__, __LINE__,
                       _("Failed to fstat() `%s' : %s"),
                       afd_active_file, strerror(errno));
            _exit(INCORRECT);
         }
         if ((buffer = malloc(stat_buf.st_size)) == NULL)
         {
            system_log(FATAL_SIGN, __FILE__, __LINE__,
                       _("malloc() error : %s"), strerror(errno));
            _exit(INCORRECT);
         }
         if (read(read_fd, buffer, stat_buf.st_size) != stat_buf.st_size)
         {
            system_log(FATAL_SIGN, __FILE__, __LINE__,
                       _("read() error : %s"), strerror(errno));
            _exit(INCORRECT);
         }
         (void)close(read_fd);
         ptr = buffer;
      }
      else
      {
         ptr = pid_list;
      }

#ifndef HAVE_MMAP
      /*
       * If some afd_ctrl dialog is still active, let the
       * LED's show the user that all process have been stopped.
       */
      for (i = 0; i < NO_OF_PROCESS; i++)
      {
         if ((i != DC_NO) &&
             ((i != AFDD_NO) || (*proc_table[i].status != NEITHER)))
         {
            *proc_table[i].status = STOPPED;
         }
      }

      /* We have to unmap now because when we kill the mapper */
      /* this will no longer be possible and the job will     */
      /* hang forever.                                        */
      (void)munmap_emu((void *)p_afd_status);
      p_afd_status = NULL;
#endif

      /* Try to send kill signal to all running process */
      ptr += sizeof(pid_t);
      for (i = 1; i <= NO_OF_PROCESS; i++)
      {
         if (i == (SLOG_NO + 1))
         {
            syslog = *(pid_t *)ptr;
         }
         else
         {
            if (*(pid_t *)ptr > 0)
            {
#ifdef _DEBUG
               system_log(DEBUG_SIGN, __FILE__, __LINE__, _("Killing %d - %s"),
                          *(pid_t *)ptr, proc_table[i - 1].proc_name);
#endif
               if (kill(*(pid_t *)ptr, SIGINT) == -1)
               {
                  if (errno != ESRCH)
                  {
                     system_log(WARN_SIGN, __FILE__, __LINE__,
                                _("Failed to kill() %d %s : %s"),
                                *(pid_t *)ptr, proc_table[i - 1].proc_name,
                                strerror(errno));
                  }
               }
               else
               {
                  if (((i - 1) != DC_NO) && (proc_table[i - 1].status != NULL) &&
                      (((i - 1) != AFDD_NO) || (*proc_table[i - 1].status != NEITHER)))
                  {
                     *proc_table[i - 1].status = STOPPED;
                  }
               }
            }
            else
            {
               if (((i - 1) != DC_NO) && (proc_table[i - 1].status != NULL) &&
                   (((i - 1) != AFDD_NO) || (*proc_table[i - 1].status != NEITHER)))
               {
                  *proc_table[i - 1].status = STOPPED;
               }
            }
         }
         ptr += sizeof(pid_t);
      }
      *proc_table[SLOG_NO].status = STOPPED;

      if (p_afd_status->hostname[0] != '\0')
      {
         char   date_str[26];
         time_t now;

         now = time(NULL);
         strftime(date_str, 26, "%a %h %d %H:%M:%S %Y", localtime(&now));
         system_log(CONFIG_SIGN, NULL, 0,
                    _("Shutdown on <%s> %s"), p_afd_status->hostname, date_str);
      }

      /* Unset hostname so no one thinks AFD is still up. */
      p_afd_status->hostname[0] = '\0';

      /*
       * As the last step store all important values in a machine
       * indepandant format.
       */
      (void)check_fsa(NO, AFD);
      if (fsa != NULL)
      {
         (void)fra_attach_passive();
         write_system_data(p_afd_status,
                           (int)*((char *)fsa - AFD_FEATURE_FLAG_OFFSET_END),
                           (int)*((char *)fra - AFD_FEATURE_FLAG_OFFSET_END));
         fra_detach();
      }

#ifdef HAVE_MMAP
      if (msync((void *)p_afd_status, sizeof(struct afd_status), MS_SYNC) == -1)
      {
         system_log(ERROR_SIGN, __FILE__, __LINE__,
                    _("msync() error : %s"), strerror(errno));
      }
      if (munmap((void *)p_afd_status, sizeof(struct afd_status)) == -1)
      {
         system_log(ERROR_SIGN, __FILE__, __LINE__,
                    _("munmap() error : %s"), strerror(errno));
      }
      p_afd_status = NULL;
#endif

      system_log(CONFIG_SIGN, NULL, 0,
                 "=================> SHUTDOWN <=================");

      if (unlink(afd_active_file) == -1)
      {
         system_log(ERROR_SIGN, __FILE__, __LINE__,
                    _("Failed to unlink() `%s' : %s"),
                    afd_active_file, strerror(errno));
      }

      /* As the last process kill the system log process. */
      if (syslog > 0)
      {
         int            counter = 0;
         fd_set         rset;
         struct timeval timeout;

         /* Give system log some time to tell whether AMG, FD, etc */
         /* have been stopped.                                     */
         FD_ZERO(&rset);
         do
         {
            (void)my_usleep(5000L);
            FD_SET(sys_log_fd, &rset);
            timeout.tv_usec = 10000L;
            timeout.tv_sec = 0L;
            counter++;
         } while ((select(sys_log_fd + 1, &rset, NULL, NULL, &timeout) > 0) &&
                  (counter < 1000));
         (void)my_usleep(10000L);
         (void)kill(syslog, SIGINT);
      }
   }

   return;
}


/*++++++++++++++++++++++++++++++ sig_segv() +++++++++++++++++++++++++++++*/
static void
sig_segv(int signo)
{
   system_log(FATAL_SIGN, __FILE__, __LINE__,
              _("Aaarrrggh! Received SIGSEGV."));
   abort();
}


/*++++++++++++++++++++++++++++++ sig_bus() ++++++++++++++++++++++++++++++*/
static void
sig_bus(int signo)
{
   system_log(FATAL_SIGN, __FILE__, __LINE__, _("Uuurrrggh! Received SIGBUS."));
   abort();
}


/*++++++++++++++++++++++++++++++ sig_exit() +++++++++++++++++++++++++++++*/
static void
sig_exit(int signo)
{
   if (signo == SIGINT)
   {
      system_log(DEBUG_SIGN, __FILE__, __LINE__, _("Received SIGINT!"));
   }
   else
   {
      system_log(DEBUG_SIGN, __FILE__, __LINE__, _("Received %d!"), signo);
   }

   exit(INCORRECT);
}
