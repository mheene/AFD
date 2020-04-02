/*
 *  gf_exec.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 2013 - 2020 Deutscher Wetterdienst (DWD),
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
 **   gf_exec - gets data via an external command
 **
 ** SYNOPSIS
 **   gf_exec <work dir> <job no.> <FSA id> <FSA pos> <dir alias> [options]
 **
 **   options
 **      --version        Version Number
 **      -d               Distributed helper job.
 **      -o <retries>     Old/Error message and number of retries.
 **      -t               Temp toggle.
 **
 ** DESCRIPTION
 **
 ** RETURN VALUES
 **   SUCCESS on normal exit and INCORRECT when an error has
 **   occurred.
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   23.01.2013 H.Kiehl Created
 **
 */
DESCR__E_M1

#include <stdio.h>                     /* fprintf(), snprintf()          */
#include <string.h>                    /* strcpy(), strcat(), strcmp(),  */
                                       /* strerror()                     */
#include <stdlib.h>                    /* malloc(), free(), abort()      */
#include <sys/types.h>
#include <dirent.h>                    /* opendir()                      */
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>                    /* signal()                       */
#include <unistd.h>                    /* close(), getpid()              */
#include <errno.h>
#include "fddefs.h"
#include "version.h"

int                        *current_no_of_listed_files = NULL,
                           event_log_fd = STDERR_FILENO,
                           exitflag = IS_FAULTY_VAR,
                           files_to_retrieve_shown = 0,
                           fra_fd = -1,
                           fra_id,
                           fsa_fd = -1,
                           fsa_id,
#ifdef HAVE_HW_CRC32
                           have_hw_crc32 = NO,
#endif
#ifdef _MAINTAINER_LOG
                           maintainer_log_fd = STDERR_FILENO,
#endif
                           no_of_dirs = 0,
                           no_of_hosts = 0,
                           *p_no_of_dirs = NULL,
                           *p_no_of_hosts = NULL,
                           no_of_listed_files,
                           rl_fd = -1,
                           trans_db_log_fd = STDERR_FILENO,
                           transfer_log_fd = STDERR_FILENO,
#ifdef WITHOUT_FIFO_RW_SUPPORT
                           trans_db_log_readfd,
                           transfer_log_readfd,
#endif
                           sys_log_fd = STDERR_FILENO,
                           timeout_flag;
off_t                      file_size_to_retrieve_shown = 0,
                           rl_size = 0;
#ifdef HAVE_MMAP
off_t                      fra_size,
                           fsa_size;
#endif
long                       transfer_timeout;
char                       msg_str[MAX_RET_MSG_LENGTH],
                           *p_work_dir = NULL,
                           tr_hostname[MAX_HOSTNAME_LENGTH + 2];
struct retrieve_list       *rl;
struct filetransfer_status *fsa;
struct fileretrieve_status *fra;
struct job                 db;
const char                 *sys_log_name = SYSTEM_LOG_FIFO;

/* Local function prototypes. */
static int                 exec_timeup(void);
static void                gf_exec_exit(void),
                           sig_bus(int),
                           sig_segv(int),
                           sig_kill(int),
                           sig_exit(int);


/*$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ main() $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$*/
int
main(int argc, char *argv[])
{
   int              files_retrieved = 0,
                    files_to_retrieve = 0,
                    local_file_length,
                    more_files_in_list,
                    ret;
#ifdef HAVE_SETPRIORITY
   int              sched_priority;
#endif
   unsigned int     crc_val;
   off_t            file_size_retrieved = 0,
                    file_size_to_retrieve = 0;
   char             command_str[MAX_PATH_LENGTH + MAX_RECIPIENT_LENGTH],
                    job_str[4],
                    local_file[MAX_PATH_LENGTH],
                    local_tmp_file[MAX_PATH_LENGTH],
                    *p_command,
                    *p_current_real_hostname,
                    *p_local_file,
                    *p_local_tmp_file,
                    *return_str = NULL,
                    str_crc_val[MAX_INT_HEX_LENGTH];
   DIR              *dp;
   struct dirent    *p_dir;
   struct stat      stat_buf;
#ifdef SA_FULLDUMP
   struct sigaction sact;
#endif

   CHECK_FOR_VERSION(argc, argv);

#ifdef SA_FULLDUMP
   /*
    * When dumping core sure we do a FULL core dump!
    */
   sact.sa_handler = SIG_DFL;
   sact.sa_flags = SA_FULLDUMP;
   sigemptyset(&sact.sa_mask);
   if (sigaction(SIGSEGV, &sact, NULL) == -1)
   {
      system_log(FATAL_SIGN, __FILE__, __LINE__,
                 "sigaction() error : %s", strerror(errno));
      exit(INCORRECT);
   }
#endif

   /* Do some cleanups when we exit. */
   if (atexit(gf_exec_exit) != 0)
   {
      system_log(FATAL_SIGN, __FILE__, __LINE__,
                 "Could not register exit function : %s", strerror(errno));
      exit(INCORRECT);
   }

   /* Initialise variables. */
   init_gf(argc, argv, EXEC_FLAG);
   msg_str[0] = '\0';

   if ((signal(SIGINT, sig_kill) == SIG_ERR) ||
       (signal(SIGQUIT, sig_exit) == SIG_ERR) ||
       (signal(SIGTERM, SIG_IGN) == SIG_ERR) ||
       (signal(SIGSEGV, sig_segv) == SIG_ERR) ||
       (signal(SIGBUS, sig_bus) == SIG_ERR) ||
       (signal(SIGHUP, SIG_IGN) == SIG_ERR) ||
       (signal(SIGPIPE, SIG_IGN) == SIG_ERR))
   {
      system_log(FATAL_SIGN, __FILE__, __LINE__,
                 "signal() error : %s", strerror(errno));
      exit(INCORRECT);
   }

   /* Now determine the real hostname. */
   if (db.toggle_host == YES)
   {
      if (fsa->host_toggle == HOST_ONE)
      {
         (void)strcpy(db.hostname, fsa->real_hostname[HOST_TWO - 1]);
      }
      else
      {
         (void)strcpy(db.hostname, fsa->real_hostname[HOST_ONE - 1]);
      }
   }
   else
   {
      (void)strcpy(db.hostname,
                   fsa->real_hostname[(int)(fsa->host_toggle - 1)]);
   }

   fsa->job_status[(int)db.job_no].connect_status = EXEC_RETRIEVE_ACTIVE;

   /* Get directory where files are to be stored and */
   /* prepare some pointers for the file names.      */
#ifdef HAVE_HW_CRC32
   crc_val = get_str_checksum_crc32c(db.exec_cmd, have_hw_crc32);
#else
   crc_val = get_str_checksum_crc32c(db.exec_cmd);
#endif
   (void)snprintf(str_crc_val, MAX_INT_HEX_LENGTH, "%x", crc_val);
   if (create_remote_dir(NULL, fra->retrieve_work_dir, db.user, db.hostname,
                         str_crc_val, local_file, &local_file_length) == INCORRECT)
   {
      system_log(ERROR_SIGN, __FILE__, __LINE__,
                 "Failed to determine local incoming directory for <%s>.",
                 fra->dir_alias);
      exit(INCORRECT);
   }
   else
   {
      p_local_tmp_file = local_tmp_file +
                         snprintf(local_tmp_file, MAX_PATH_LENGTH, "%s/.%x/",
                                  local_file, (int)db.job_no);
      if ((mkdir(local_tmp_file, DIR_MODE) == -1) && (errno != EEXIST))
      {
         trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                   "Failed to create directory `%s' : %s",
                   local_tmp_file, strerror(errno));
         exit(MKDIR_ERROR);
      }
      local_file[local_file_length - 1] = '/';
      local_file[local_file_length] = '\0';
      p_local_file = local_file + local_file_length;
   }

   /* Prepare command string that we want to execute. */
   p_command = db.exec_cmd;
   while ((*p_command == ' ') || (*p_command == '\t'))
   {
      p_command++;
   }
   (void)snprintf(command_str, MAX_PATH_LENGTH + MAX_RECIPIENT_LENGTH,
                  "cd %s && %s", local_tmp_file, p_command);

   /* Init job_str for exec_cmd(). */
   job_str[0] = '[';
   job_str[1] = db.job_no + '0';
   job_str[2] = ']';
   job_str[3] = '\0';

   more_files_in_list = NO;
   do
   {
      /* Check if real_hostname has changed. */
      if (db.toggle_host == YES)
      {
         if (fsa->host_toggle == HOST_ONE)
         {
            p_current_real_hostname = fsa->real_hostname[HOST_TWO - 1];
         }
         else
         {
            p_current_real_hostname = fsa->real_hostname[HOST_ONE - 1];
         }
      }
      else
      {
         p_current_real_hostname = fsa->real_hostname[(int)(fsa->host_toggle - 1)];
      }
      if (strcmp(db.hostname, p_current_real_hostname) != 0)
      {
         trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                   "hostname changed (%s -> %s), exiting.",
                   db.hostname, p_current_real_hostname);
         reset_values(files_retrieved, file_size_retrieved,
                      files_to_retrieve, file_size_to_retrieve,
                      (struct job *)&db);
         exitflag = 0;
         exit(TRANSFER_SUCCESS);
      }

      if (db.fsa_pos != INCORRECT)
      {
         fsa->job_status[(int)db.job_no].no_of_files += files_to_retrieve;
         fsa->job_status[(int)db.job_no].file_size += file_size_to_retrieve;

         /* Number of connections. */
         fsa->connections += 1;

         files_to_retrieve_shown += files_to_retrieve;
         file_size_to_retrieve_shown += file_size_to_retrieve;
      }

      (void)gsf_check_fra((struct job *)&db);
      if (db.fra_pos == INCORRECT)
      {
         /* Looks as if this source is no longer in our database. */
         reset_values(files_retrieved, file_size_retrieved,
                      files_to_retrieve, file_size_to_retrieve,
                      (struct job *)&db);
         exitflag = 0;
         exit(TRANSFER_SUCCESS);
      }

#ifdef HAVE_SETPRIORITY
      if (db.exec_base_priority != NO_PRIORITY)
      {
         sched_priority = db.exec_base_priority;
         if (db.add_afd_priority == YES)        
         {
            sched_priority += (int)(fsa->job_status[(int)db.job_no].unique_name[MAX_MSG_NAME_LENGTH - 1]);
            if (sched_priority > db.min_sched_priority)
            {
               sched_priority = db.min_sched_priority;
            }
            else if (sched_priority < db.max_sched_priority)
                 {
                    sched_priority = db.max_sched_priority;
                 }
         }
         if ((sched_priority == db.current_priority) ||
             ((db.current_priority > sched_priority) && (geteuid() != 0)))
         {
            sched_priority = NO_PRIORITY;
         }
      }
      else
      {
         sched_priority = NO_PRIORITY;
      }                               
#endif
      if ((ret = exec_cmd(command_str, &return_str, transfer_log_fd,
                          fsa->host_dsp_name, MAX_HOSTNAME_LENGTH,
#ifdef HAVE_SETPRIORITY
                          sched_priority,
#endif
                          job_str, NULL, NULL, 0,
                          (fsa->protocol_options & TIMEOUT_TRANSFER) ? (time_t)transfer_timeout : 0L,
                          YES, YES)) != 0)
      {
         trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                   "Failed to execute command %s [Return code = %d]",
                   command_str, ret);
         if ((return_str != NULL) && (return_str[0] != '\0'))
         {
            char *end_ptr = return_str,
                 *start_ptr;

            do
            {
               start_ptr = end_ptr;
               while ((*end_ptr != '\n') && (*end_ptr != '\0'))
               {
                  end_ptr++;
               }
               if (*end_ptr == '\n')
               {
                  *end_ptr = '\0';
                  end_ptr++;
               }
               trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                         "%s", start_ptr);
            } while (*end_ptr != '\0');
         }
         exit(EXEC_ERROR);
      }
      free(return_str);
      return_str = NULL;

      /* Now lets see what the command got for us and move this to  */
      /* a place where AMG can pick them up for further processing. */
      if ((dp = opendir(local_tmp_file)) == NULL)
      {
         trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                   _("Failed to opendir() `%s' : %s"),
                   local_tmp_file, strerror(errno));
         exit(OPEN_FILE_DIR_ERROR);
      }
      while ((p_dir = readdir(dp)) != NULL)
      {
         if (p_dir->d_name[0] == '.')
         {
            errno = 0;
            continue;
         }
         (void)strcpy(p_local_tmp_file, p_dir->d_name);
         if (stat(local_tmp_file, &stat_buf) < 0)
         {
            if (errno != ENOENT)
            {
               trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                         _("Failed to stat() file `%s' : %s"),
                         local_tmp_file, strerror(errno));
            }
            continue;
         }

         /* Sure it is a normal file? */
         if (S_ISREG(stat_buf.st_mode))
         {
            /* Generate name for the new file. */
            (void)strcpy(p_local_file, p_dir->d_name);

            if (rename(local_tmp_file, local_file) == -1)
            {
               trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                         _("Failed to rename() `%s' to `%s' : %s"),
                         local_tmp_file, local_file, strerror(errno));
            }
            else
            {
               if (db.fsa_pos != INCORRECT)
               {
                  fsa->job_status[(int)db.job_no].file_size_done += stat_buf.st_size;
                  fsa->job_status[(int)db.job_no].no_of_files_done += 1;
               }
               files_retrieved++;
               file_size_retrieved += stat_buf.st_size;
               if (fsa->debug > NORMAL_MODE)
               {
                  trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                               "Renamed local file `%s' to `%s'.",
                               local_tmp_file, local_file);
               }
            }
         }
      }
      if (errno == EBADF)
      {
         trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                   _("Failed to readdir() `%s' : %s"),
                   local_tmp_file, strerror(errno));
      }
      if (db.fsa_pos != INCORRECT)
      {
         fsa->job_status[(int)db.job_no].no_of_files = 0;
         fsa->job_status[(int)db.job_no].file_size = 0;
      }
   } while (((*(unsigned char *)((char *)p_no_of_hosts + AFD_FEATURE_FLAG_OFFSET_START) & DISABLE_RETRIEVE) == 0) &&
            ((more_files_in_list == YES) ||
             ((db.keep_connected > 0) && (exec_timeup() == SUCCESS))));

   if (db.fsa_pos != INCORRECT)
   {
      fsa->job_status[(int)db.job_no].connect_status = CLOSING_CONNECTION;
   }

   exitflag = 0;
   exit(TRANSFER_SUCCESS);
}


/*++++++++++++++++++++++++++++ gf_exec_exit() +++++++++++++++++++++++++++*/
static void
gf_exec_exit(void)
{
   if ((fsa != NULL) && (db.fsa_pos >= 0))
   {
      int  length = MAX_INT_LENGTH + 16 + MAX_OFF_T_LENGTH + 16 + MAX_INT_LENGTH + 8 + 1;
      char buffer[MAX_INT_LENGTH + 16 + MAX_OFF_T_LENGTH + 16 + MAX_INT_LENGTH + 8 + 1];

      WHAT_DONE_BUFFER(length, buffer, "exec retrieved",
                       fsa->job_status[(int)db.job_no].file_size_done,
                       fsa->job_status[(int)db.job_no].no_of_files_done);
      trans_log(INFO_SIGN, NULL, 0, NULL, NULL, "%s @%x",
                buffer, db.id.dir);
      reset_fsa((struct job *)&db, exitflag, 0, 0);
   }

   send_proc_fin(NO);
   if (sys_log_fd != STDERR_FILENO)
   {
      (void)close(sys_log_fd);
   }

   return;
}


/*+++++++++++++++++++++++++++ exec_timeup() +++++++++++++++++++++++++++++*/
static int
exec_timeup(void)
{
   time_t now,
          timeup;

   (void)gsf_check_fra((struct job *)&db);
   if (db.fra_pos == INCORRECT)                 
   {
      return(INCORRECT);
   }
   if (fra->keep_connected > 0)
   {
      db.keep_connected = fra->keep_connected;
   }
   else if ((fsa->keep_connected > 0) &&
            ((fsa->special_flag & KEEP_CON_NO_FETCH) == 0))
        {
           db.keep_connected = fsa->keep_connected;
        }
        else
        {
           db.keep_connected = 0;
           return(INCORRECT);
        }
   now = time(NULL);
   timeup = now + db.keep_connected;
   if (db.no_of_time_entries == 0)
   {
      fra->next_check_time = now + db.remote_file_check_interval;
   }
   else
   {
      fra->next_check_time = calc_next_time_array(db.no_of_time_entries,
                                                  db.te,
#ifdef WITH_TIMEZONE
                                                  db.timezone,
#endif
                                                  now,
                                                  __FILE__, __LINE__);
   }
   if (fra->next_check_time > timeup)
   {
      return(INCORRECT);
   }
   else
   {
      if (fra->next_check_time < now)
      {
         system_log(DEBUG_SIGN, __FILE__, __LINE__,
#if SIZEOF_TIME_T == 4
                    "BUG in calc_next_time(): next_check_time (%ld) < now (%ld)",
#else
                    "BUG in calc_next_time(): next_check_time (%lld) < now (%lld)",
#endif
                    (pri_time_t)fra->next_check_time,
                    (pri_time_t)now);
         return(INCORRECT);
      }
      else
      {
         timeup = fra->next_check_time;
      }
   }
   if (gsf_check_fsa((struct job *)&db) != NEITHER)
   {
      time_t sleeptime = 0;

      if (fsa->protocol_options & STAT_KEEPALIVE)
      {
         sleeptime = fsa->transfer_timeout - 5;
      }
      if (sleeptime < 1)
      {
         sleeptime = DEFAULT_NOOP_INTERVAL;
      }
      if ((now + sleeptime) > timeup)
      {
         sleeptime = timeup - now;
      }
      fsa->job_status[(int)db.job_no].unique_name[2] = 5;
      do
      {
         (void)sleep(sleeptime);
         (void)gsf_check_fra((struct job *)&db);
         if ((db.fra_pos == INCORRECT) || (db.fsa_pos == INCORRECT))
         {
            return(INCORRECT);
         }
         if (gsf_check_fsa((struct job *)&db) == NEITHER)
         {
            if (db.fsa_pos == INCORRECT)
            {
               return(INCORRECT);
            }
            break;
         }
         if (fsa->job_status[(int)db.job_no].unique_name[2] == 6)
         {
            fsa->job_status[(int)db.job_no].unique_name[2] = '\0';
            return(INCORRECT);
         }
         now = time(NULL);
         if ((now + sleeptime) > timeup)
         {
            sleeptime = timeup - now;
         }
      } while (timeup > now);
   }

   return(SUCCESS);
}


/*++++++++++++++++++++++++++++++ sig_segv() +++++++++++++++++++++++++++++*/
static void
sig_segv(int signo)
{
   reset_fsa((struct job *)&db, IS_FAULTY_VAR, 0, 0);
   system_log(DEBUG_SIGN, __FILE__, __LINE__,
              "Aaarrrggh! Received SIGSEGV. Remove the programmer who wrote this!");
   abort();
}


/*++++++++++++++++++++++++++++++ sig_bus() ++++++++++++++++++++++++++++++*/
static void
sig_bus(int signo)
{
   reset_fsa((struct job *)&db, IS_FAULTY_VAR, 0, 0);
   system_log(DEBUG_SIGN, __FILE__, __LINE__, "Uuurrrggh! Received SIGBUS.");
   abort();
}


/*++++++++++++++++++++++++++++++ sig_kill() +++++++++++++++++++++++++++++*/
static void
sig_kill(int signo)
{
   exitflag = 0;
   exit(GOT_KILLED);
}


/*++++++++++++++++++++++++++++++ sig_exit() +++++++++++++++++++++++++++++*/
static void
sig_exit(int signo)
{
   exit(INCORRECT);
}
