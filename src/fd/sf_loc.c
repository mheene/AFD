/*
 *  sf_loc.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 1996 - 2019 Deutscher Wetterdienst (DWD),
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
 **   sf_loc - copies files from one directory to another
 **
 ** SYNOPSIS
 **   sf_loc <work dir> <job no.> <FSA id> <FSA pos> <msg name> [options]
 **
 **   options
 **       --version        Version Number
 **       -a <age limit>   The age limit for the files being send.
 **       -A               Disable archiving of files.
 **       -o <retries>     Old/Error message and number of retries.
 **       -r               Resend from archive (job from show_olog).
 **       -t               Temp toggle.
 **
 ** DESCRIPTION
 **   sf_loc is very similar to sf_ftp only that it sends files
 **   locally (i.e. moves/copies files from one directory to another).
 **
 ** RETURN VALUES
 **   SUCCESS on normal exit and INCORRECT when an error has
 **   occurred.
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   11.03.1996 H.Kiehl Created
 **   27.01.1997 H.Kiehl Include support for output logging.
 **   18.04.1997 H.Kiehl Do a hard link when we are in the same file
 **                      system.
 **   08.05.1997 H.Kiehl Logging archive directory.
 **   12.06.1999 H.Kiehl Added option to change user and group ID.
 **   07.10.1999 H.Kiehl Added option to force a copy.
 **   09.07.2000 H.Kiehl Cleaned up log output to reduce code size.
 **   03.03.2004 H.Kiehl Create target directory if it does not exist.
 **   02.09.2007 H.Kiehl Added copying via splice().
 **   23.01.2010 H.Kiehl Added support for mirroring source.
 **   25.11.2011 H.Kiehl When calling trans_exec() use the source file
 **                      and NOT the destination file!
 **   28.03.2012 H.Kiehl Handle cross link errors in case we use
 **                      mount with bind option in linux.
 **   15.09.2014 H.Kiehl Added simulation mode.
 **   06.07.2019 H.Kiehl Added trans_srename support.
 **
 */
DESCR__E_M1

#include <stdio.h>                     /* fprintf(), snprintf()          */
#include <string.h>                    /* strcpy(), strcat(), strerror() */
#include <stdlib.h>                    /* getenv(), abort()              */
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>                     /* isalpha()                      */
#include <utime.h>                     /* utime()                        */
#include <sys/time.h>                  /* struct timeval                 */
#ifdef _OUTPUT_LOG
# include <sys/times.h>                /* times(), struct tms            */
#endif
#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif
#include <signal.h>                    /* signal()                       */
#include <unistd.h>                    /* unlink(), alarm()              */
#include <errno.h>
#include "fddefs.h"
#include "version.h"

#ifdef WITH_SPLICE_SUPPORT
# ifndef SPLICE_F_MOVE
#  define SPLICE_F_MOVE 0x01
# endif
# ifndef SPLICE_F_MORE
#  define SPLICE_F_MORE 0x04
# endif
#endif

/* Global variables. */
int                        amg_flag = NO,
                           counter_fd = -1,
                           *current_no_of_listed_files,
                           event_log_fd = STDERR_FILENO,
                           exitflag = IS_FAULTY_VAR,
                           files_to_delete,
#ifdef HAVE_HW_CRC32
                           have_hw_crc32 = NO,
#endif
#ifdef _MAINTAINER_LOG
                           maintainer_log_fd = STDERR_FILENO,
#endif
                           no_of_dirs = 0,
                           no_of_hosts,    /* This variable is not used */
                                           /* in this module.           */
                           no_of_listed_files,
                           *p_no_of_hosts = NULL,
                           fra_fd = -1,
                           fra_id,
                           fsa_fd = -1,
                           fsa_id,
                           prev_no_of_files_done = 0,
                           move_flag,
                           rl_fd = -1,
                           simulation_mode = NO,
                           sys_log_fd = STDERR_FILENO,
                           timeout_flag = OFF,
                           transfer_log_fd = STDERR_FILENO,
                           trans_db_log_fd = STDERR_FILENO,
#ifdef WITHOUT_FIFO_RW_SUPPORT
                           trans_db_log_readfd,
                           transfer_log_readfd,
#endif
                           trans_rename_blocked = NO,
                           *unique_counter;
#ifdef _OUTPUT_LOG
int                        ol_fd = -2;
# ifdef WITHOUT_FIFO_RW_SUPPORT
int                        ol_readfd = -2;
# endif
unsigned int               *ol_job_number,
                           *ol_retries;
char                       *ol_data = NULL,
                           *ol_file_name,
                           *ol_output_type;
unsigned short             *ol_archive_name_length,
                           *ol_file_name_length,
                           *ol_unl;
off_t                      *ol_file_size;
size_t                     ol_size,
                           ol_real_size;
clock_t                    *ol_transfer_time;
#endif
#ifdef _WITH_BURST_2
unsigned int               burst_2_counter = 0;
#endif
long                       transfer_timeout; /* Not used [init_sf()]    */
#ifdef HAVE_MMAP
off_t                      fra_size,
                           fsa_size;
#endif
off_t                      *file_size_buffer = NULL,
                           rl_size = 0;
time_t                     *file_mtime_buffer = NULL;
u_off_t                    prev_file_size_done = 0;
char                       *p_work_dir = NULL,
                           tr_hostname[MAX_HOSTNAME_LENGTH + 2],
                           *del_file_name_buffer = NULL,
                           *file_name_buffer = NULL;
struct fileretrieve_status *fra = NULL;
struct filetransfer_status *fsa = NULL;
struct retrieve_list       *rl;
struct job                 db;
struct rule                *rule;
#ifdef _DELETE_LOG
struct delete_log          dl;
#endif
const char                 *sys_log_name = SYSTEM_LOG_FIFO;

/* Local global variables. */
static int                 files_send,
                           files_to_send,
                           local_file_counter;
static off_t               local_file_size,
                           *p_file_size_buffer;

/* Local function prototypes. */
static int                 copy_file_mkdir(char *, char *, char *, int *);
static void                sf_loc_exit(void),
                           sig_bus(int),
                           sig_segv(int),
                           sig_kill(int),
                           sig_exit(int);


/*$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ main() $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$*/
int
main(int argc, char *argv[])
{
   int              additional_length,
#ifdef _WITH_BURST_2
                    cb2_ret = NO,
#endif
                    exit_status = TRANSFER_SUCCESS,
                    fd,
                    lfs,                    /* Local file system. */
                    ret;
#ifdef WITH_ARCHIVE_COPY_INFO
   unsigned int     archived_copied = 0;
#endif
   time_t           connected,
#ifdef _WITH_BURST_2
                    diff_time,
#endif
                    last_update_time,
                    now,
                    *p_file_mtime_buffer;
   char             *ptr,
                    *p_if_name,
                    *p_ff_name,
                    *p_source_file,
                    *p_to_name,
                    *p_file_name_buffer,
                    file_name[MAX_FILENAME_LENGTH],
                    if_name[MAX_PATH_LENGTH],
                    ff_name[MAX_PATH_LENGTH],
                    file_path[MAX_PATH_LENGTH],
                    source_file[MAX_PATH_LENGTH];
   clock_t          clktck;
   struct job       *p_db;
#ifdef SA_FULLDUMP
   struct sigaction sact;
#endif
#ifdef WITH_FAST_MOVE
   nlink_t          nlink;
#endif
#ifdef _OUTPUT_LOG
   clock_t          end_time = 0,
                    start_time = 0;
   struct tms       tmsdummy;
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
      system_log(ERROR_SIGN, __FILE__, __LINE__,
                 "sigaction() error : %s", strerror(errno));
      exit(INCORRECT);
   }
#endif

   /* Do some cleanups when we exit. */
   if (atexit(sf_loc_exit) != 0)
   {
      system_log(ERROR_SIGN, __FILE__, __LINE__,
                 "Could not register exit function : %s", strerror(errno));
      exit(INCORRECT);
   }

   /* Initialise variables. */
   local_file_counter = 0;
   files_to_send = init_sf(argc, argv, file_path, LOC_FLAG);
   p_db = &db;
   if ((clktck = sysconf(_SC_CLK_TCK)) <= 0)
   {
      system_log(ERROR_SIGN, __FILE__, __LINE__,
                 "Could not get clock ticks per second : %s", strerror(errno));
      exit(INCORRECT);
   }

   if ((signal(SIGINT, sig_kill) == SIG_ERR) ||
       (signal(SIGQUIT, sig_exit) == SIG_ERR) ||
       (signal(SIGTERM, SIG_IGN) == SIG_ERR) ||
       (signal(SIGSEGV, sig_segv) == SIG_ERR) ||
       (signal(SIGBUS, sig_bus) == SIG_ERR) ||
       (signal(SIGHUP, SIG_IGN) == SIG_ERR))
   {
      system_log(ERROR_SIGN, __FILE__, __LINE__,
                 "Failed to set signal handlers : %s", strerror(errno));
      exit(INCORRECT);
   }

   /* Inform FSA that we have are ready to copy the files. */
   if (gsf_check_fsa(p_db) != NEITHER)
   {
      fsa->job_status[(int)db.job_no].connect_status = LOC_ACTIVE;
      fsa->job_status[(int)db.job_no].no_of_files = files_to_send;
   }
   connected = time(NULL);

#ifdef _WITH_BURST_2
   do
   {
      if (burst_2_counter > 0)
      {
         if (fsa->debug > NORMAL_MODE)
         {
            trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL, "Bursting.");
         }
      }
#endif /* _WITH_BURST_2 */
      /* If we send a lockfile, do it now. */
      if (db.lock == LOCKFILE)
      {
         /* Create lock file in directory. */
         if ((fd = open(db.lock_file_name, (O_WRONLY | O_CREAT | O_TRUNC),
                        (S_IRUSR | S_IWUSR))) == -1)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                      "Failed to create lock file `%s' : %s",
                      db.lock_file_name, strerror(errno));
            exit(WRITE_LOCK_ERROR);
         }
         else
         {
            if (fsa->debug > NORMAL_MODE)
            {
               trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                            "Created lockfile to `%s'.", db.lock_file_name);
            }
         }
         if (close(fd) == -1)
         {
            trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                      "Failed to close() `%s' : %s",
                      db.lock_file_name, strerror(errno));
         }
      }

      /*
       * Since we do not know if the directory where the file
       * will be moved is in the same file system, lets determine
       * this by comparing the device numbers.
       */
      if (((db.special_flag & FORCE_COPY) == 0) &&
          ((db.special_flag & FILE_NAME_IS_HEADER) == 0))
      {
         struct stat stat_buf;

         if (stat(file_path, &stat_buf) == 0)
         {
            dev_t ldv;               /* Local device number (file system). */

            ldv = stat_buf.st_dev;
#ifdef WITH_FAST_MOVE
            nlink = stat_buf.st_nlink;
#endif
            if (stat(db.target_dir, &stat_buf) == 0)
            {
               if (stat_buf.st_dev == ldv)
               {
                  lfs = YES;
               }
               else
               {
                  lfs = NO;
               }
            }
            else if ((errno == ENOENT) && (db.special_flag & CREATE_TARGET_DIR))
                 {
                    char created_path[MAX_PATH_LENGTH],
                         *error_ptr;

                    created_path[0] = '\0';
                    if (((ret = check_create_path(db.target_dir, db.dir_mode,
                                                  &error_ptr, YES, YES,
                                                  created_path)) == CREATED_DIR) ||
                        (ret == CHOWN_ERROR))
                    {
                       if (CHECK_STRCMP(db.target_dir, created_path) == 0)
                       {
                          trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                    "Created directory `%s'", db.target_dir);
                       }
                       else
                       {
                          trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                    "Created directory part `%s' for `%s'",
                                    created_path, db.target_dir);
                       }
                       if (ret == CHOWN_ERROR)
                       {
                          trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                                    "Failed to chown() of directory `%s' : %s",
                                    db.target_dir, strerror(errno));
                       }
                       if (stat(db.target_dir, &stat_buf) == 0)
                       {
                          if (stat_buf.st_dev == ldv)
                          {
                             lfs = YES;
                          }
                          else
                          {
                             lfs = NO;
                          }
                       }
                       else
                       {
                          trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                    "Failed to stat() `%s' : %s",
                                    db.target_dir, strerror(errno));
                          exit(STAT_TARGET_ERROR);
                       }
                    }
                    else if (ret == MKDIR_ERROR)
                         {
                            if (error_ptr != NULL)
                            {
                               *error_ptr = '\0';
                            }
                            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                      "Failed to mkdir() `%s' error : %s",
                                      db.target_dir, strerror(errno));
                            ret = MOVE_ERROR;
                         }
                    else if (ret == STAT_ERROR)
                         {
                            if (error_ptr != NULL)
                            {
                               *error_ptr = '\0';
                            }
                            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                      "Failed to stat() `%s' error : %s",
                                      db.target_dir, strerror(errno));
                            ret = MOVE_ERROR;
                         }
                    else if (ret == NO_ACCESS)
                         {
                            if (error_ptr != NULL)
                            {
                               *error_ptr = '\0';
                            }
                            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                      "Cannot access directory `%s' : %s",
                                      db.target_dir, strerror(errno));
                            ret = MOVE_ERROR;
                         }
                    else if (ret == ALLOC_ERROR)
                         {
                            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                      "Failed to allocate memory : %s",
                                      strerror(errno));
                         }
                    else if (ret == SUCCESS)
                         {
                            trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
                                      "Hmmm, directory does seem to be ok, so why can we not open the file!?");
                            ret = MOVE_ERROR;
                         }
                    if (ret != CREATED_DIR)
                    {
                       exit(ret);
                    }
                 }
                 else
                 {
                    trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                              "Failed to stat() %s : %s [%d]",
                              db.target_dir, strerror(errno),
                              (*(unsigned char *)((char *)p_no_of_hosts + 5)));
                    exit(STAT_TARGET_ERROR);
                 }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                      "Failed to stat() %s : %s", file_path, strerror(errno));
            exit(STAT_ERROR);
         }
      }
      else
      {
         lfs = NO;
      }

      /* Prepare pointers and directory name. */
      (void)strcpy(source_file, file_path);
      p_source_file = source_file + strlen(source_file);
      *p_source_file++ = '/';
      (void)strcpy(if_name, db.target_dir);
      p_if_name = if_name + strlen(if_name);
      *p_if_name++ = '/';
      *p_if_name = '\0';
      (void)strcpy(ff_name, db.target_dir);
      p_ff_name = ff_name + strlen(ff_name);
      *p_ff_name++ = '/';
      *p_ff_name = '\0';

      if ((db.lock == DOT) || (db.lock == DOT_VMS) ||
          (db.special_flag & UNIQUE_LOCKING))
      {
         p_to_name = if_name;
      }
      else
      {
         p_to_name = ff_name;
      }
      move_flag = 0;

#ifdef WITH_FAST_MOVE
      /*
       * When source + destination are in same filesystem and no
       * locking is requested, lets try and move all the files with
       * one single rename() call.
       */
      if ((lfs == YES) && (p_to_name == ff_name) &&
          ((db.special_flag & TRANS_EXEC) == 0) && (nlink == 2) &&
          (db.trans_rename_rule[0] == '\0') && (db.archive_time == 0) &&
          (access(db.target_dir, W_OK) == 0) &&
          (rename(file_path, db.target_dir) == 0))
      {
         p_file_size_buffer = file_size_buffer;

         /* Tell FSA we have copied a file. */
         if (gsf_check_fsa(p_db) != NEITHER)
         {
            fsa->job_status[(int)db.job_no].file_name_in_use[0] = '\0';
            fsa->job_status[(int)db.job_no].no_of_files_done += files_to_send;
            fsa->job_status[(int)db.job_no].file_size_in_use = 0;
            fsa->job_status[(int)db.job_no].file_size_in_use_done = 0;
            for (files_send = 0; files_send < files_to_send; files_send++)
            {
               fsa->job_status[(int)db.job_no].file_size_done += *p_file_size_buffer;
               fsa->job_status[(int)db.job_no].bytes_send += *p_file_size_buffer;
               local_file_size += *p_file_size_buffer;
               p_file_size_buffer++;
            }
            local_file_counter += files_to_send;

            now = time(NULL);
            if (now >= (last_update_time + LOCK_INTERVAL_TIME))
            {
               last_update_time = now;
               update_tfc(local_file_counter, local_file_size,
                          p_file_size_buffer, files_to_send,
                          files_send, now);
               local_file_size = 0;
               local_file_counter = 0;
            }
         }
      }
      else
      {
#endif
         /* Copy all files. */
         p_file_name_buffer = file_name_buffer;
         p_file_size_buffer = file_size_buffer;
         p_file_mtime_buffer = file_mtime_buffer;
         last_update_time = time(NULL);
         local_file_size = 0;
         for (files_send = 0; files_send < files_to_send; files_send++)
         {
            additional_length = 0;

            /* Get the the name of the file we want to send next. */
            *p_ff_name = '\0';
            (void)strcat(ff_name, p_file_name_buffer);
            (void)strcpy(file_name, p_file_name_buffer);
            if ((db.lock == DOT) || (db.lock == DOT_VMS))
            {
               *p_if_name = '\0';
               (void)strcat(if_name, db.lock_notation);
               (void)strcat(if_name, p_file_name_buffer);
            }
            else if (db.lock == POSTFIX)
                 {
                    *p_if_name = '\0';
                    (void)strcat(if_name, p_file_name_buffer);
                    (void)strcat(if_name, db.lock_notation);
                 }

            if (db.special_flag & UNIQUE_LOCKING)
            {
               char *p_end;

               p_end = if_name + strlen(if_name);
               (void)snprintf(p_end, MAX_PATH_LENGTH - (p_end - if_name),
                              ".%u", (unsigned int)db.unique_number);
            }
            (void)strcpy(p_source_file, p_file_name_buffer);

            /* Write status to FSA? */
            if (gsf_check_fsa(p_db) != NEITHER)
            {
               fsa->job_status[(int)db.job_no].file_size_in_use = *p_file_size_buffer;
               (void)my_strncpy(fsa->job_status[(int)db.job_no].file_name_in_use,
                                p_file_name_buffer, MAX_FILENAME_LENGTH);
            }

            if (db.trans_rename_rule[0] != '\0')
            {
               register int k;
   
               for (k = 0; k < rule[db.trans_rule_pos].no_of_rules; k++)
               {
                  if (pmatch(rule[db.trans_rule_pos].filter[k],
                             p_file_name_buffer, NULL) == 0)
                  {
                     change_name(p_file_name_buffer,
                                 rule[db.trans_rule_pos].filter[k],
                                 rule[db.trans_rule_pos].rename_to[k],
                                 p_ff_name,
                                 MAX_PATH_LENGTH - (p_ff_name - ff_name),
                                 &counter_fd, &unique_counter, db.id.job);
                     break;
                  }
               }
            }
            else if (db.cn_filter != NULL)
                 {
                    if (pmatch(db.cn_filter, p_file_name_buffer, NULL) == 0)
                    {
                       change_name(p_file_name_buffer, db.cn_filter,
                                   db.cn_rename_to, p_ff_name,
                                   MAX_PATH_LENGTH - (p_ff_name - ff_name),
                                   &counter_fd, &unique_counter, db.id.job);
                    }
                 }

#ifdef _OUTPUT_LOG
            if (db.output_log == YES)
            {
               start_time = times(&tmsdummy);
            }
#endif

            /* Here comes the BIG move .... */
            if (lfs == YES)
            {
               if (simulation_mode == YES)
               {
                  if (fsa->debug > NORMAL_MODE)
                  {
                     trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                  "Linked file `%s' to `%s'.",
                                  source_file, p_to_name);
                  }
                  move_flag |= FILES_MOVED;
               }
               else
               {
try_link_again:
                  if (link(source_file, p_to_name) == -1)
                  {
                     if (errno == EEXIST)
                     {
                        if ((unlink(p_to_name) == -1) && (errno != ENOENT))
                        {
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Failed to unlink() `%s' : %s",
                                     p_to_name, strerror(errno));
                           exit(MOVE_ERROR);
                        }
                        else
                        {
#ifndef DO_NOT_INFORM_ABOUT_OVERWRITE
                           if (errno != ENOENT)
                           {
                              trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                        "File `%s' did already exist, removed it and linked again.",
                                        p_to_name);
                           }
#endif
                           goto try_link_again;
                        }
                     }
                     else if ((errno == ENOENT) &&
                              (db.special_flag & CREATE_TARGET_DIR))
                          {
                             char *p_file = p_to_name;

                             p_file += strlen(p_to_name);
                             while ((*p_file != '/') && (p_file != p_to_name))
                             {
                                p_file--;
                             }
                             if (*p_file == '/')
                             {
                                char created_path[MAX_PATH_LENGTH],
                                     *error_ptr;

                                *p_file = '\0';
                                created_path[0] = '\0';
                                if (((ret = check_create_path(p_to_name, db.dir_mode,
                                                              &error_ptr,
                                                              YES, YES,
                                                              created_path)) == CREATED_DIR) ||
                                    (ret == CHOWN_ERROR))
                                {
                                   if (CHECK_STRCMP(p_to_name, created_path) == 0)
                                   {
                                      trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                "Created directory `%s'",
                                                p_to_name);
                                   }
                                   else
                                   {
                                      trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                "Created directory part `%s' for `%s'",
                                                created_path, p_to_name);
                                   }
                                   if (ret == CHOWN_ERROR)
                                   {
                                      trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                "Failed to chown() of directory `%s' : %s",
                                                p_to_name, strerror(errno));
                                   }
                                   *p_file = '/';
                                   if (link(source_file, p_to_name) == -1)
                                   {
                                      if (errno == EEXIST)
                                      {
                                         if ((unlink(p_to_name) == -1) && (errno != ENOENT))
                                         {
                                            trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                      "Failed to unlink() `%s' : %s",
                                                      p_to_name, strerror(errno));
                                            exit(MOVE_ERROR);
                                         }
                                         else
                                         {
#ifndef DO_NOT_INFORM_ABOUT_OVERWRITE
                                            if (errno != ENOENT)
                                            {
                                               trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                         "File `%s' did already exist, removed it and linked again.",
                                                         p_to_name);
                                            }
#endif

                                            if (link(source_file, p_to_name) == -1)
                                            {
                                               if (errno == EXDEV)
                                               {
                                                  lfs = NO;
                                                  goto cross_link_error;
                                               }
                                               else
                                               {
                                                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                            "Failed to link file `%s' to `%s' : %s",
                                                            source_file, p_to_name, strerror(errno));
                                                  exit(MOVE_ERROR);
                                               }
                                            }
                                            else
                                            {
                                               move_flag |= FILES_MOVED;
                                            }
                                         }
                                      }
                                      else if (errno == EXDEV)
                                           {
                                              lfs = NO;
                                              goto cross_link_error;
                                           }
                                           else
                                           {
                                              trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                        "Failed to link file `%s' to `%s' : %s",
                                                        source_file, p_to_name, strerror(errno));
                                              exit(MOVE_ERROR);
                                           }
                                   }
                                }
                                else if (ret == MKDIR_ERROR)
                                     {
                                        if (error_ptr != NULL)
                                        {
                                           *error_ptr = '\0';
                                        }
                                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                  "Failed to mkdir() `%s' error : %s",
                                                  p_to_name, strerror(errno));
                                     }
                                else if (ret == STAT_ERROR)
                                     {
                                        if (error_ptr != NULL)
                                        {
                                           *error_ptr = '\0';
                                        }
                                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                  "Failed to stat() `%s' error : %s",
                                                  p_to_name, strerror(errno));
                                     }
                                else if (ret == NO_ACCESS)
                                     {
                                        if (error_ptr != NULL)
                                        {
                                           *error_ptr = '\0';
                                        }
                                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                  "Cannot access directory `%s' : %s",
                                                  p_to_name, strerror(errno));
                                        ret = MOVE_ERROR;
                                     }
                                else if (ret == ALLOC_ERROR)
                                     {
                                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                  "Failed to allocate memory : %s",
                                                  strerror(errno));
                                     }
                                else if (ret == SUCCESS)
                                     {
                                        trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                  "Hmmm, directory does seem to be ok, so why can we not open the file!?");
                                        ret = MOVE_ERROR;
                                     }
                                if (ret != CREATED_DIR)
                                {
                                   exit(ret);
                                }
                             }
                             else
                             {
                                *p_file = '/';
                                trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                          "Failed to link file `%s' to `%s' : %s",
                                          source_file, p_to_name, strerror(errno));
                                exit(MOVE_ERROR);
                             }
                          }
                     else if (errno == EXDEV)
                          {
                             lfs = NO;
                             goto cross_link_error;
                          }
                          else
                          {
                             trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                       "Failed to link file `%s' to `%s' : %s",
                                       source_file, p_to_name, strerror(errno));
                             exit(MOVE_ERROR);
                          }
                  }
                  else
                  {
                     if (fsa->debug > NORMAL_MODE)
                     {
                        trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                     "Linked file `%s' to `%s'.",
                                     source_file, p_to_name);
                     }
                     move_flag |= FILES_MOVED;
                  }
               }
            }
            else
            {
cross_link_error:
               if ((ret = copy_file_mkdir(source_file, p_to_name,
                                          p_file_name_buffer,
                                          &additional_length)) != SUCCESS)
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Failed to copy file `%s' to `%s'",
                            source_file, p_to_name);
                  exit(ret);
               }
               else
               {
                  move_flag |= FILES_COPIED;
                  if ((fsa->protocol_options & KEEP_TIME_STAMP) &&
                      (file_mtime_buffer != NULL) &&
                      (simulation_mode != YES))
                  {
                     struct utimbuf old_time;

                     old_time.actime = time(NULL);
                     old_time.modtime = *p_file_mtime_buffer;
                     if (utime(p_to_name, &old_time) == -1)
                     {
                        trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                                  "Failed to set time of file %s : %s",
                                  p_to_name, strerror(errno));
                     }
                  }
                  if (fsa->debug > NORMAL_MODE)
                  {
                     trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                  "Copied file `%s' to `%s'.",
                                  source_file, p_to_name);
                  }
               }
            }

            if (db.special_flag & CHANGE_PERMISSION)
            {
               if ((db.lock == DOT) || (db.lock == DOT_VMS) ||
                   (db.special_flag & UNIQUE_LOCKING))
               {
                  ptr = if_name;
               }
               else
               {
                  ptr = ff_name;
               }

               if (simulation_mode == YES)
               {
                  if (fsa->debug > NORMAL_MODE)
                  {
                     trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                  "Changed permission of file `%s' to %d",
                                  ptr, db.chmod);
                  }
               }
               else
               {
                  if (chmod(ptr, db.chmod) == -1)
                  {
                     trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                               "Failed to chmod() file `%s' : %s",
                               ptr, strerror(errno));
                  }
                  else
                  {
                     if (fsa->debug > NORMAL_MODE)
                     {
                        trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                     "Changed permission of file `%s' to %d",
                                     ptr, db.chmod);
                     }
                  }
               }
            } /* if (db.special_flag & CHANGE_PERMISSION) */

            if ((db.lock == DOT) || (db.lock == DOT_VMS) ||
                (db.special_flag & UNIQUE_LOCKING))
            {
               if (db.lock == DOT_VMS)
               {
                  (void)strcat(ff_name, DOT_NOTATION);
               }
               if (simulation_mode == YES)
               {
                  if (fsa->debug > NORMAL_MODE)
                  {
                     trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                  "Renamed file `%s' to `%s'.",
                                  if_name, ff_name);
                  }
               }
               else
               {
                  if (rename(if_name, ff_name) == -1)
                  {
                     if ((errno == ENOENT) &&
                         (db.special_flag & CREATE_TARGET_DIR))
                     {
                        char *p_file = ff_name;

                        p_file += strlen(ff_name);
                        while ((*p_file != '/') && (p_file != ff_name))
                        {
                           p_file--;
                        }
                        if (*p_file == '/')
                        {
                           char created_path[MAX_PATH_LENGTH],
                                *error_ptr;

                           *p_file = '\0';
                           created_path[0] = '\0';
                           if (((ret = check_create_path(ff_name, db.dir_mode,
                                                         &error_ptr, YES, YES,
                                                         created_path)) == CREATED_DIR) ||
                               (ret == CHOWN_ERROR))
                           {
                              if (CHECK_STRCMP(ff_name, created_path) == 0)
                              {
                                 trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                           "Created directory `%s'", ff_name);
                              }
                              else
                              {
                                 trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                           "Created directory part `%s' for `%s'",
                                           created_path, ff_name);
                              }
                              if (ret == CHOWN_ERROR)
                              {
                                 trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                                           "Failed to chown() of directory `%s' : %s",
                                           ff_name, strerror(errno));
                              }
                              *p_file = '/';
                              if (rename(if_name, ff_name) == -1)
                              {
                                 trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                           "Failed to rename() file `%s' to `%s' : %s",
                                           if_name, ff_name, strerror(errno));
                                 exit(RENAME_ERROR);
                              }
                           }
                           else if (ret == MKDIR_ERROR)
                                {
                                   if (error_ptr != NULL)
                                   {
                                      *error_ptr = '\0';
                                   }
                                   trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                             "Failed to mkdir() `%s' error : %s",
                                             ff_name, strerror(errno));
                                }
                           else if (ret == STAT_ERROR)
                                {
                                   if (error_ptr != NULL)
                                   {
                                      *error_ptr = '\0';
                                   }
                                   trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                             "Failed to stat() `%s' error : %s",
                                             ff_name, strerror(errno));
                                }
                           else if (ret == NO_ACCESS)
                                {
                                   if (error_ptr != NULL)
                                   {
                                      *error_ptr = '\0';
                                   }
                                   trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                             "Cannot access directory `%s' : %s",
                                             ff_name, strerror(errno));
                                   ret = MOVE_ERROR;
                                }
                           else if (ret == ALLOC_ERROR)
                                {
                                   trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                             "Failed to allocate memory : %s",
                                             strerror(errno));
                                }
                           else if (ret == SUCCESS)
                                {
                                   trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
                                             "Hmmm, directory does seem to be ok, someone else created it.");
                                   *p_file = '/';
                                   if (rename(if_name, ff_name) == -1)
                                   {
                                      trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                                "Failed to rename() file `%s' to `%s' : %s",
                                                if_name, ff_name, strerror(errno));
                                      exit(RENAME_ERROR);
                                   }
                                }
                           if ((ret != CREATED_DIR) && (ret != CHOWN_ERROR) &&
                               (ret != SUCCESS))
                           {
                              exit(ret);
                           }
                        }
                        else
                        {
                           *p_file = '/';
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Failed to rename() file `%s' to `%s' : %s",
                                     if_name, ff_name, strerror(errno));
                           exit(RENAME_ERROR);
                        }
                     }
                     else
                     {
                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                  "Failed to rename() file `%s' to `%s' : %s",
                                  if_name, ff_name, strerror(errno));
                        exit(RENAME_ERROR);
                     }
                  }
                  else
                  {
                     if (fsa->debug > NORMAL_MODE)
                     {
                        trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                     "Renamed file `%s' to `%s'.", if_name, ff_name);
                     }
                  }
               }
               if (db.lock == DOT_VMS)
               {
                  /* Take away the dot at the end. */
                  ptr = ff_name + strlen(ff_name) - 1;
                  *ptr = '\0';
               }
            }

#ifdef _OUTPUT_LOG
            if (db.output_log == YES)
            {
               end_time = times(&tmsdummy);
            }
#endif

            if (db.special_flag & CHANGE_UID_GID)
            {
               if (simulation_mode == YES)
               {
                  if (fsa->debug > NORMAL_MODE)
                  {
                     trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                  "Changed owner of file `%s' to %d:%d.",
                                  ff_name, db.user_id, db.group_id);
                  }
               }
               else
               {
                  if (chown(ff_name, db.user_id, db.group_id) == -1)
                  {
                     trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                               "Failed to chown() of file `%s' : %s",
                               ff_name, strerror(errno));
                  }
                  else
                  {
                     if (fsa->debug > NORMAL_MODE)
                     {
                        trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                     "Changed owner of file `%s' to %d:%d.",
                                     ff_name, db.user_id, db.group_id);
                     }
                  }
               }
            } /* if (db.special_flag & CHANGE_UID_GID) */

            /* Tell FSA we have copied a file. */
            if (gsf_check_fsa(p_db) != NEITHER)
            {
               fsa->job_status[(int)db.job_no].file_name_in_use[0] = '\0';
               fsa->job_status[(int)db.job_no].no_of_files_done++;
               fsa->job_status[(int)db.job_no].file_size_in_use = 0;
               fsa->job_status[(int)db.job_no].file_size_in_use_done = 0;
               fsa->job_status[(int)db.job_no].file_size_done += *p_file_size_buffer;
               fsa->job_status[(int)db.job_no].bytes_send += *p_file_size_buffer;
               local_file_size += *p_file_size_buffer;
               local_file_counter += 1;

               now = time(NULL);
               if (now >= (last_update_time + LOCK_INTERVAL_TIME))
               {
                  last_update_time = now;
                  update_tfc(local_file_counter, local_file_size,
                             p_file_size_buffer, files_to_send,
                             files_send, now);
                  local_file_size = 0;
                  local_file_counter = 0;
               }
            }

#ifdef _WITH_TRANS_EXEC
            if (db.special_flag & TRANS_EXEC)
            {
               if (db.special_flag & EXECUTE_IN_TARGET_DIR)
               {
                  trans_exec(db.target_dir, ff_name, p_file_name_buffer, clktck);
               }
               else
               {
                  trans_exec(file_path, source_file, p_file_name_buffer, clktck);
               }
            }
#endif

#ifdef _OUTPUT_LOG
            if (db.output_log == YES)
            {
               if (ol_fd == -2)
               {
# ifdef WITHOUT_FIFO_RW_SUPPORT
                  output_log_fd(&ol_fd, &ol_readfd, &db.output_log);
# else
                  output_log_fd(&ol_fd, &db.output_log);
# endif
               }
               if ((ol_fd > -1) && (ol_data == NULL))
               {
                  output_log_ptrs(&ol_retries, &ol_job_number, &ol_data,
                                  &ol_file_name, &ol_file_name_length,
                                  &ol_archive_name_length, &ol_file_size,
                                  &ol_unl, &ol_size, &ol_transfer_time,
                                  &ol_output_type, db.host_alias, 0, LOC,
                                  &db.output_log);
               }
            }
#endif

            /* Now archive file if necessary. */
            if ((db.archive_time > 0) &&
                (p_db->archive_dir[0] != FAILED_TO_CREATE_ARCHIVE_DIR))
            {
#ifdef WITH_ARCHIVE_COPY_INFO
               int ret;
#endif

               /*
                * By telling the function archive_file() that this
                * is the first time to archive a file for this job
                * (in struct p_db) it does not always have to check
                * whether the directory has been created or not. And
                * we ensure that we do not create duplicate names
                * when adding db.archive_time to msg_name.
                */
#ifdef WITH_ARCHIVE_COPY_INFO
               if ((ret = archive_file(file_path, p_file_name_buffer, p_db)) < 0)
#else
               if (archive_file(file_path, p_file_name_buffer, p_db) < 0)
#endif
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Failed to archive file `%s'", file_name);

                  /*
                   * NOTE: We _MUST_ delete the file we just send,
                   *       else the file directory will run full!
                   */
                  if (unlink(source_file) == -1)
                  {
                     system_log(ERROR_SIGN, __FILE__, __LINE__,
                                "Could not unlink() local file `%s' after copying it successfully : %s",
                                source_file, strerror(errno));
                  }

#ifdef _OUTPUT_LOG
                  if (db.output_log == YES)
                  {
                     (void)memcpy(ol_file_name, db.p_unique_name, db.unl);
                     if (db.trans_rename_rule[0] != '\0')
                     {
                        *ol_file_name_length = (unsigned short)snprintf(ol_file_name + db.unl,
                                                                        MAX_FILENAME_LENGTH + 1 + MAX_FILENAME_LENGTH + 2,
                                                                        "%s%c/%s",
                                                                        p_file_name_buffer,
                                                                        SEPARATOR_CHAR,
                                                                        ff_name) + db.unl;
                        if (*ol_file_name_length >= (MAX_FILENAME_LENGTH + 1 + MAX_FILENAME_LENGTH + 2 + db.unl))
                        {
                           *ol_file_name_length = MAX_FILENAME_LENGTH + 1 + MAX_FILENAME_LENGTH + 2 + db.unl;
                        }
                     }
                     else
                     {
                        (void)strcpy(ol_file_name + db.unl, p_file_name_buffer);
                        *ol_file_name_length = (unsigned short)strlen(ol_file_name);
                        ol_file_name[*ol_file_name_length] = SEPARATOR_CHAR;
                        ol_file_name[*ol_file_name_length + 1] = '\0';
                        (*ol_file_name_length)++;
                     }
                     *ol_file_size = *p_file_size_buffer + additional_length;
                     *ol_job_number = fsa->job_status[(int)db.job_no].job_id;
                     *ol_retries = db.retries;
                     *ol_unl = db.unl;
                     *ol_transfer_time = end_time - start_time;
                     *ol_archive_name_length = 0;
                     *ol_output_type = OT_NORMAL_DELIVERED + '0';
                     ol_real_size = *ol_file_name_length + ol_size;
                     if (write(ol_fd, ol_data, ol_real_size) != ol_real_size)
                     {
                        system_log(ERROR_SIGN, __FILE__, __LINE__,
                                   "write() error : %s", strerror(errno));
                     }
                  }
#endif
               }
               else
               {
                  if (fsa->debug > NORMAL_MODE)
                  {
                     trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                  "Archived file `%s'.", file_name);
                  }
#ifdef WITH_ARCHIVE_COPY_INFO
                  if (ret == DATA_COPIED)
                  {
                     archived_copied++;
                  }
#endif

#ifdef _OUTPUT_LOG
                  if (db.output_log == YES)
                  {
                     (void)memcpy(ol_file_name, db.p_unique_name, db.unl);
                     if (db.trans_rename_rule[0] != '\0')
                     {
                        *ol_file_name_length = (unsigned short)snprintf(ol_file_name + db.unl,
                                                                        MAX_FILENAME_LENGTH,
                                                                        "%s%c/%s",
                                                                        p_file_name_buffer,
                                                                        SEPARATOR_CHAR,
                                                                        ff_name) + db.unl;
                     }
                     else
                     {
                        (void)strcpy(ol_file_name + db.unl, p_file_name_buffer);
                        *ol_file_name_length = (unsigned short)strlen(ol_file_name);
                        ol_file_name[*ol_file_name_length] = SEPARATOR_CHAR;
                        ol_file_name[*ol_file_name_length + 1] = '\0';
                        (*ol_file_name_length)++;
                     }
                     (void)strcpy(&ol_file_name[*ol_file_name_length + 1], &db.archive_dir[db.archive_offset]);
                     *ol_file_size = *p_file_size_buffer + additional_length;
                     *ol_job_number = fsa->job_status[(int)db.job_no].job_id;
                     *ol_retries = db.retries;
                     *ol_unl = db.unl;
                     *ol_transfer_time = end_time - start_time;
                     *ol_archive_name_length = (unsigned short)strlen(&ol_file_name[*ol_file_name_length + 1]);
                     *ol_output_type = OT_NORMAL_DELIVERED + '0';
                     ol_real_size = *ol_file_name_length +
                                    *ol_archive_name_length + 1 + ol_size;
                     if (write(ol_fd, ol_data, ol_real_size) != ol_real_size)
                     {
                        system_log(ERROR_SIGN, __FILE__, __LINE__,
                                   "write() error : %s", strerror(errno));
                     }
                  }
#endif
               }
            }
            else
            {
#ifdef WITH_UNLINK_DELAY
               int unlink_loops = 0;

try_again_unlink:
#endif
               /* Delete the file we just have copied. */
               if (unlink(source_file) == -1)
               {
#ifdef WITH_UNLINK_DELAY
                  if ((errno == EBUSY) && (unlink_loops < 20))
                  {
                     (void)my_usleep(100000L);
                     unlink_loops++;
                     goto try_again_unlink;
                  }
#endif
                  system_log(ERROR_SIGN, __FILE__, __LINE__,
                             "Could not unlink() local file %s after copying it successfully : %s",
                             source_file, strerror(errno));
               }

#ifdef _OUTPUT_LOG
               if (db.output_log == YES)
               {
                  (void)memcpy(ol_file_name, db.p_unique_name, db.unl);
                  if (db.trans_rename_rule[0] != '\0')
                  {
                     *ol_file_name_length = (unsigned short)snprintf(ol_file_name + db.unl,
                                                                     MAX_FILENAME_LENGTH + 1 + MAX_FILENAME_LENGTH + 2,
                                                                     "%s%c/%s",
                                                                     p_file_name_buffer,
                                                                     SEPARATOR_CHAR,
                                                                     ff_name) + db.unl;
                     if (*ol_file_name_length >= (MAX_FILENAME_LENGTH + 1 + MAX_FILENAME_LENGTH + 2 + db.unl))
                     {
                        *ol_file_name_length = MAX_FILENAME_LENGTH + 1 + MAX_FILENAME_LENGTH + 2 + db.unl;
                     }
                  }
                  else
                  {
                     (void)strcpy(ol_file_name + db.unl, p_file_name_buffer);
                     *ol_file_name_length = (unsigned short)strlen(ol_file_name);
                     ol_file_name[*ol_file_name_length] = SEPARATOR_CHAR;
                     ol_file_name[*ol_file_name_length + 1] = '\0';
                     (*ol_file_name_length)++;
                  }
                  *ol_file_size = *p_file_size_buffer + additional_length;
                  *ol_job_number = fsa->job_status[(int)db.job_no].job_id;
                  *ol_retries = db.retries;
                  *ol_unl = db.unl;
                  *ol_transfer_time = end_time - start_time;
                  *ol_archive_name_length = 0;
                  *ol_output_type = OT_NORMAL_DELIVERED + '0';
                  ol_real_size = *ol_file_name_length + ol_size;
                  if (write(ol_fd, ol_data, ol_real_size) != ol_real_size)
                  {
                     system_log(ERROR_SIGN, __FILE__, __LINE__,
                                "write() error : %s", strerror(errno));
                  }
               }
#endif
            }

            /*
             * After each successful transfer set error counter to zero,
             * so that other jobs can be started.
             */
            if (gsf_check_fsa(p_db) != NEITHER)
            {
               if (fsa->error_counter > 0)
               {
                  int  fd,
#ifdef WITHOUT_FIFO_RW_SUPPORT
                       readfd,
#endif
                       j;
                  char fd_wake_up_fifo[MAX_PATH_LENGTH];

#ifdef LOCK_DEBUG
                  lock_region_w(fsa_fd, db.lock_offset + LOCK_EC, __FILE__, __LINE__);
#else
                  lock_region_w(fsa_fd, db.lock_offset + LOCK_EC);
#endif
                  fsa->error_counter = 0;

                  /*
                   * Wake up FD!
                   */
                  (void)snprintf(fd_wake_up_fifo, MAX_PATH_LENGTH, "%s%s%s",
                                 p_work_dir, FIFO_DIR, FD_WAKE_UP_FIFO);
#ifdef WITHOUT_FIFO_RW_SUPPORT
                  if (open_fifo_rw(fd_wake_up_fifo, &readfd, &fd) == -1)
#else
                  if ((fd = open(fd_wake_up_fifo, O_RDWR)) == -1)
#endif
                  {
                     system_log(WARN_SIGN, __FILE__, __LINE__,
                                "Failed to open() FIFO %s : %s",
                                fd_wake_up_fifo, strerror(errno));
                  }
                  else
                  {
                     if (write(fd, "", 1) != 1)
                     {
                        system_log(WARN_SIGN, __FILE__, __LINE__,
                                   "Failed to write() to FIFO %s : %s",
                                   fd_wake_up_fifo, strerror(errno));
                     }
#ifdef WITHOUT_FIFO_RW_SUPPORT
                     if (close(readfd) == -1)
                     {
                        system_log(DEBUG_SIGN, __FILE__, __LINE__,
                                   "Failed to close() FIFO %s : %s",
                                   fd_wake_up_fifo, strerror(errno));
                     }
#endif
                     if (close(fd) == -1)
                     {
                        system_log(DEBUG_SIGN, __FILE__, __LINE__,
                                   "Failed to close() FIFO %s : %s",
                                   fd_wake_up_fifo, strerror(errno));
                     }
                  }

                  /*
                   * Remove the error condition (NOT_WORKING) from all jobs
                   * of this host.
                   */
                  for (j = 0; j < fsa->allowed_transfers; j++)
                  {
                     if ((j != db.job_no) &&
                         (fsa->job_status[j].connect_status == NOT_WORKING))
                     {
                        fsa->job_status[j].connect_status = DISCONNECT;
                     }
                  }
                  fsa->error_history[0] = 0;
                  fsa->error_history[1] = 0;
#ifdef LOCK_DEBUG
                  unlock_region(fsa_fd, db.lock_offset + LOCK_EC, __FILE__, __LINE__);
#else
                  unlock_region(fsa_fd, db.lock_offset + LOCK_EC);
#endif

#ifdef LOCK_DEBUG
                  lock_region_w(fsa_fd, db.lock_offset + LOCK_HS, __FILE__, __LINE__);
#else
                  lock_region_w(fsa_fd, db.lock_offset + LOCK_HS);
#endif
                  now = time(NULL);
                  if (now > fsa->end_event_handle)
                  {
                     fsa->host_status &= ~(EVENT_STATUS_FLAGS | AUTO_PAUSE_QUEUE_STAT);
                     if (fsa->end_event_handle > 0L)
                     {
                        fsa->end_event_handle = 0L;
                     }
                     if (fsa->start_event_handle > 0L)
                     {
                        fsa->start_event_handle = 0L;
                     }
                  }
                  else
                  {
                     fsa->host_status &= ~(EVENT_STATUS_STATIC_FLAGS | AUTO_PAUSE_QUEUE_STAT);
                  }
#ifdef LOCK_DEBUG
                  unlock_region(fsa_fd, db.lock_offset + LOCK_HS, __FILE__, __LINE__);
#else
                  unlock_region(fsa_fd, db.lock_offset + LOCK_HS);
#endif

                  /*
                   * Since we have successfully transmitted a file, no need to
                   * have the queue stopped anymore.
                   */
                  if (fsa->host_status & AUTO_PAUSE_QUEUE_STAT)
                  {
                     char sign[LOG_SIGN_LENGTH];

                     error_action(fsa->host_alias, "stop", HOST_ERROR_ACTION);
                     event_log(0L, EC_HOST, ET_EXT, EA_ERROR_END, "%s",
                               fsa->host_alias);
                     if ((fsa->host_status & HOST_ERROR_OFFLINE_STATIC) ||
                         (fsa->host_status & HOST_ERROR_OFFLINE) ||
                         (fsa->host_status & HOST_ERROR_OFFLINE_T))
                     {
                        (void)memcpy(sign, OFFLINE_SIGN, LOG_SIGN_LENGTH);
                     }
                     else
                     {
                        (void)memcpy(sign, INFO_SIGN, LOG_SIGN_LENGTH);
                     }
                     trans_log(sign, __FILE__, __LINE__, NULL, NULL,
                               "Starting input queue that was stopped by init_afd.");
                     event_log(0L, EC_HOST, ET_AUTO, EA_START_QUEUE, "%s",
                               fsa->host_alias);
                  }
               } /* if (fsa->error_counter > 0) */
#ifdef WITH_ERROR_QUEUE
               if (fsa->host_status & ERROR_QUEUE_SET)
               {
                  remove_from_error_queue(db.id.job, fsa, db.fsa_pos, fsa_fd);
               }
#endif
               if (fsa->host_status & HOST_ACTION_SUCCESS)
               {
                  error_action(fsa->host_alias, "start", HOST_SUCCESS_ACTION);
               }
            }

            p_file_name_buffer += MAX_FILENAME_LENGTH;
            p_file_size_buffer++;
            if (file_mtime_buffer != NULL)
            {
               p_file_mtime_buffer++;
            }
         } /* for (files_send = 0; files_send < files_to_send; files_send++) */

#ifdef WITH_ARCHIVE_COPY_INFO
         if (archived_copied > 0)
         {
            trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
                      "Copied %u files to archive.", archived_copied);
            archived_copied = 0;
         }
#endif

         if (local_file_counter)
         {
            if (gsf_check_fsa(p_db) != NEITHER)
            {
               update_tfc(local_file_counter, local_file_size,
                          p_file_size_buffer, files_to_send, files_send,
                          time(NULL));
               local_file_size = 0;
               local_file_counter = 0;
            }
         }

         /* Do not forget to remove lock file if we have created one. */
         if ((db.lock == LOCKFILE) && (fsa->active_transfers == 1))
         {
            if (unlink(db.lock_file_name) == -1)
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                         "Failed to unlink() lock file `%s' : %s",
                         db.lock_file_name, strerror(errno));
               exit(REMOVE_LOCKFILE_ERROR);
            }
            else
            {
               if (fsa->debug > NORMAL_MODE)
               {
                  trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                               "Removed lock file `%s'.", db.lock_file_name);
               }
            }
         }

         /*
          * Remove file directory.
          */
#ifdef AFDBENCH_CONFIG
         if (rec_rmdir(file_path) == INCORRECT)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       "Failed to rec_rmdir() `%s' : %s",
                       file_path, strerror(errno));
            exit_status = STILL_FILES_TO_SEND;
         }
#else
         if (rmdir(file_path) == -1)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       "Failed to rmdir() `%s' : %s", file_path, strerror(errno));
            exit_status = STILL_FILES_TO_SEND;
         }
#endif
         if (db.special_flag & MIRROR_DIR)
         {
            compare_dir_local();
         }
#ifdef WITH_FAST_MOVE
      }
#endif

#ifdef _WITH_BURST_2
      burst_2_counter++;
      diff_time = time(NULL) - connected;
      if (((fsa->protocol_options & KEEP_CONNECTED_DISCONNECT) &&
           (db.keep_connected > 0) && (diff_time > db.keep_connected)) ||
          ((db.disconnect > 0) && (diff_time > db.disconnect)))
      {
         cb2_ret = NO;
         break;
      }
   } while ((cb2_ret = check_burst_sf(file_path, &files_to_send, move_flag,
# ifdef _WITH_INTERRUPT_JOB
                                      0,
# endif
# ifdef _OUTPUT_LOG
                                      &ol_fd,
# endif
# ifndef AFDBENCH_CONFIG
                                      NULL,
# endif
                                      NULL)) == YES);
   burst_2_counter--;

   if (cb2_ret == NEITHER)
   {
      exit_status = STILL_FILES_TO_SEND;
   }
#endif /* _WITH_BURST_2 */

   exitflag = 0;
   exit(exit_status);
}


/*++++++++++++++++++++++++++ copy_file_mkdir() ++++++++++++++++++++++++++*/
static int
copy_file_mkdir(char *from,
                char *to,
                char *orig_file_name,
                int  *additional_length)
{
   int from_fd,
       ret = SUCCESS;

   /* Open source file. */
#ifdef O_LARGEFILE
   if ((from_fd = open(from, O_RDONLY | O_LARGEFILE)) == -1)
#else
   if ((from_fd = open(from, O_RDONLY)) == -1)
#endif
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                "Could not open `%s' for copying : %s", from, strerror(errno));
      ret = MOVE_ERROR;
   }
   else
   {
      struct stat stat_buf;

      /* Need size and permissions of input file. */
      if (fstat(from_fd, &stat_buf) == -1)
      {
         trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                   "Could not fstat() on `%s' : %s", from, strerror(errno));
         (void)close(from_fd);
         ret = MOVE_ERROR;
      }
      else
      {
         int to_fd;

         if (simulation_mode == YES)
         {
            if ((to_fd = open("/dev/null", O_WRONLY)) == -1)
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                         "Failed to open() /dev/null for writting : %s",
                         strerror(errno));
               ret = MOVE_ERROR;
            }
         }
         else
         {
            /* Open destination file. */
#ifdef O_LARGEFILE
            if ((to_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE,
#else
            if ((to_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC,
#endif
                              stat_buf.st_mode)) == -1)
            {
               if ((errno == ENOENT) && (db.special_flag & CREATE_TARGET_DIR))
               {
                  char created_path[MAX_PATH_LENGTH],
                       *p_file = to;

                  p_file += strlen(to);
                  while ((*p_file != '/') && (p_file != to))
                  {
                     p_file--;
                  }
                  if (*p_file == '/')
                  {
                     char *error_ptr;

                     *p_file = '\0';
                     created_path[0] = '\0';
                     if (((ret = check_create_path(to, db.dir_mode, &error_ptr,
                                                   YES, YES,
                                                   created_path)) == CREATED_DIR) ||
                         (ret == CHOWN_ERROR))
                     {
                        if (CHECK_STRCMP(to, created_path) == 0)
                        {
                           trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Created directory `%s'", to);
                        }
                        else
                        {
                           trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Created directory part `%s' for `%s'",
                                     created_path, to);
                        }
                        if (ret == CHOWN_ERROR)
                        {
                           trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Failed to chown() of directory `%s' : %s",
                                     to, strerror(errno));
                        }
                        *p_file = '/';
#ifdef O_LARGEFILE
                        if ((to_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_LARGEFILE,
#else
                        if ((to_fd = open(to, O_WRONLY | O_CREAT | O_TRUNC,
#endif
                                          stat_buf.st_mode)) == -1)
                        {
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Could not open `%s' for copying : %s",
                                     to, strerror(errno));
                           ret = MOVE_ERROR;
                        }
                        else
                        {
                           ret = SUCCESS;
                        }
                     }
                     else if (ret == MKDIR_ERROR)
                          {
                             if (error_ptr != NULL)
                             {
                                *error_ptr = '\0';
                             }
                             trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                       "Failed to mkdir() `%s' error : %s",
                                       to, strerror(errno));
                             if (error_ptr != NULL)
                             {
                                *error_ptr = '/';
                             }
                          }
                     else if (ret == STAT_ERROR)
                          {
                             if (error_ptr != NULL)
                             {
                                *error_ptr = '\0';
                             }
                             trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                       "Failed to stat() `%s' error : %s",
                                       to, strerror(errno));
                             if (error_ptr != NULL)
                             {
                                *error_ptr = '/';
                             }
                          }
                     else if (ret == NO_ACCESS)
                          {
                             if (error_ptr != NULL)
                             {
                                *error_ptr = '\0';
                             }
                             trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                       "Cannot access directory `%s' : %s",
                                       to, strerror(errno));
                             if (error_ptr != NULL)
                             {
                                *error_ptr = '/';
                             }
                             ret = MOVE_ERROR;
                          }
                     else if (ret == ALLOC_ERROR)
                          {
                             trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                       "Failed to allocate memory : %s",
                                       strerror(errno));
                          }
                     else if (ret == SUCCESS)
                          {
                             trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
                                       "Hmmm, directory does seem to be ok, so why can we not open the file!?");
                             ret = MOVE_ERROR;
                          }
                  }
               }
               else
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Could not open `%s' for copying : %s",
                            to, strerror(errno));
                  ret = MOVE_ERROR;
               }
            }
         }
         if (to_fd != -1)
         {
            if (db.special_flag & FILE_NAME_IS_HEADER)
            {
               int  header_length,
                    space_count = 0;
               char buffer[4 + MAX_TTAAii_HEADER_LENGTH + 3 + 1 + 48],
                          /* + 48 is just some arbitrary extra length. */
                    *ptr = orig_file_name;

               buffer[0] = 1; /* SOH */
               buffer[1] = '\015'; /* CR */
               buffer[2] = '\015'; /* CR */
               buffer[3] = '\012'; /* LF */
               header_length = 4;

               for (;;)
               {
                  while ((header_length < sizeof(buffer)) &&
                         (*ptr != '_') && (*ptr != '-') && (*ptr != ' ') &&
                         (*ptr != '\0') && (*ptr != '.') && (*ptr != ';')) 
                  {
                     buffer[header_length] = *ptr;
                     header_length++; ptr++;
                  }
                  if ((*ptr == '\0') || (*ptr == '.') || (*ptr == ';') ||
                      (header_length >= sizeof(buffer)))
                  {
                     break;
                  }
                  else
                  {
                     if (space_count == 2)
                     {
                        if ((isalpha((int)(*(ptr + 1)))) &&
                            (isalpha((int)(*(ptr + 2)))) &&
                            (isalpha((int)(*(ptr + 3)))))
                        {
                           if ((header_length + 4) < sizeof(buffer))
                           {
                              buffer[header_length] = ' ';
                              buffer[header_length + 1] = *(ptr + 1);
                              buffer[header_length + 2] = *(ptr + 2);
                              buffer[header_length + 3] = *(ptr + 3);
                              header_length += 4;                    
                           }
                        }
                        break;
                     }
                     else
                     {
                        buffer[header_length] = ' ';
                        header_length++; ptr++; space_count++;
                     }
                  }
               }
               buffer[header_length] = '\015'; /* CR */
               buffer[header_length + 1] = '\015'; /* CR */
               buffer[header_length + 2] = '\012'; /* LF */
               header_length += 3;

               if (write(to_fd, buffer, header_length) != header_length)
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Failed to write() `%s' : %s",
                            to, strerror(errno));
                  ret = MOVE_ERROR;
               }
               else
               {
                  *additional_length += header_length;
               }
            }

            if ((stat_buf.st_size > 0) && (ret == SUCCESS))
            {
               time_t end_transfer_time_file,
                      start_transfer_time_file = 0;
#ifdef WITH_SPLICE_SUPPORT
               int    fd_pipe[2];

               if (pipe(fd_pipe) == -1)
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Failed to create pipe for copying : %s",
                            strerror(errno));
                  ret = MOVE_ERROR;
               }
               else
               {
                  long  bytes_read,
                        bytes_written;
                  off_t bytes_left;

                  if (fsa->protocol_options & TIMEOUT_TRANSFER)
                  {
                     start_transfer_time_file = time(NULL);
                  }
                  bytes_left = stat_buf.st_size;
                  while (bytes_left)
                  {
                     if ((bytes_read = splice(from_fd, NULL, fd_pipe[1], NULL,
                                              bytes_left,
                                              SPLICE_F_MOVE | SPLICE_F_MORE)) == -1)
                     {
                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                  "splice() error : %s", strerror(errno));
                        ret = MOVE_ERROR;
                        break;
                     }
                     bytes_left -= bytes_read;

                     while (bytes_read)
                     {
                        if ((bytes_written = splice(fd_pipe[0], NULL, to_fd,
                                                    NULL, bytes_read,
                                                    SPLICE_F_MOVE | SPLICE_F_MORE)) == -1)
                        {
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "splice() error : %s", strerror(errno));
                           ret = MOVE_ERROR;
                           bytes_left = 0;
                           break;
                        }
                        bytes_read -= bytes_written;
                     }
                     if ((db.fsa_pos != INCORRECT) &&
                         (fsa->protocol_options & TIMEOUT_TRANSFER))
                     {
                        end_transfer_time_file = time(NULL);
                        if (end_transfer_time_file < start_transfer_time_file)
                        {
                           start_transfer_time_file = end_transfer_time_file;
                        }
                        else
                        {
                           if ((end_transfer_time_file - start_transfer_time_file) > transfer_timeout)
                           {
                              trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
# if SIZEOF_TIME_T == 4
                                        "Transfer timeout reached for `%s' after %ld seconds.",
# else
                                        "Transfer timeout reached for `%s' after %lld seconds.",
# endif
                                        fsa->job_status[(int)db.job_no].file_name_in_use,
                                        (pri_time_t)(end_transfer_time_file - start_transfer_time_file));
                              exitflag = 0;
                              exit(STILL_FILES_TO_SEND);
                           }
                        }
                     }
                  }
                  if ((close(fd_pipe[0]) == -1) || (close(fd_pipe[1]) == -1))
                  {
                     trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                               "Failed to close() pipe : %s", strerror(errno));
                  }
               }
#else
               char *buffer;

               if ((buffer = malloc(stat_buf.st_blksize)) == NULL)
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Failed to allocate memory : %s", strerror(errno));
                  ret = MOVE_ERROR;
               }
               else
               {
                  int bytes_buffered;

                  if (fsa->protocol_options & TIMEOUT_TRANSFER)
                  {
                     start_transfer_time_file = time(NULL);
                  }
                  do
                  {
                     if ((bytes_buffered = read(from_fd, buffer,
                                                stat_buf.st_blksize)) == -1)
                     {
                        trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                  "Failed to read() `%s' : %s",
                                  from, strerror(errno));
                        ret = MOVE_ERROR;
                        break;
                     }
                     if (bytes_buffered > 0)
                     {
                        if (write(to_fd, buffer, bytes_buffered) != bytes_buffered)
                        {
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                     "Failed to write() `%s' : %s",
                                     to, strerror(errno));
                           ret = MOVE_ERROR;
                           break;
                        }
                        if ((db.fsa_pos != INCORRECT) &&
                            (fsa->protocol_options & TIMEOUT_TRANSFER))
                        {
                           end_transfer_time_file = time(NULL);
                           if (end_transfer_time_file < start_transfer_time_file)
                           {
                              start_transfer_time_file = end_transfer_time_file;
                           }
                           else
                           {
                              if ((end_transfer_time_file - start_transfer_time_file) > transfer_timeout)
                              {
                                 trans_log(INFO_SIGN, __FILE__, __LINE__, NULL, NULL,
# if SIZEOF_TIME_T == 4
                                           "Transfer timeout reached for `%s' after %ld seconds.",
# else
                                           "Transfer timeout reached for `%s' after %lld seconds.",
# endif
                                           fsa->job_status[(int)db.job_no].file_name_in_use,
                                           (pri_time_t)(end_transfer_time_file - start_transfer_time_file));
                                 exit(STILL_FILES_TO_SEND);
                              }
                           }
                        }
                     }
                  } while (bytes_buffered == stat_buf.st_blksize);
                  free(buffer);
               }
#endif /* !WITH_SPLICE_SUPPORT */
            }
            if (db.special_flag & FILE_NAME_IS_HEADER)
            {
               char buffer[4];

               buffer[0] = '\015';
               buffer[1] = '\015';
               buffer[2] = '\012';
               buffer[3] = 3;  /* ETX */

               if (write(to_fd, buffer, 4) != 4)
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Failed to write() <CR><CR><LF><ETX> to `%s' : %s",
                            to, strerror(errno));
                  ret = MOVE_ERROR;
               }
               else
               {
                  *additional_length += 4;
               }
            }
            if (close(to_fd) == -1)
            {
               trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                         "Failed to close() `%s' : %s", to, strerror(errno));
            }
         }
      }
      if (close(from_fd) == -1)
      {
         trans_log(WARN_SIGN, __FILE__, __LINE__, NULL, NULL,
                   "Failed to close() `%s' : %s", from, strerror(errno));
      }
   }

   return(ret);
}


/*+++++++++++++++++++++++++++++ sf_loc_exit() +++++++++++++++++++++++++++*/
static void
sf_loc_exit(void)
{
   if ((fsa != NULL) && (db.fsa_pos >= 0))
   {
      int     diff_no_of_files_done;
      u_off_t diff_file_size_done;

      if (local_file_counter)
      {
         if (gsf_check_fsa((struct job *)&db) != NEITHER)
         {
            update_tfc(local_file_counter, local_file_size,
                       p_file_size_buffer, files_to_send, files_send,
                       time(NULL));
         }
      }

      diff_no_of_files_done = fsa->job_status[(int)db.job_no].no_of_files_done -
                              prev_no_of_files_done;
      diff_file_size_done = fsa->job_status[(int)db.job_no].file_size_done -
                            prev_file_size_done;
      if ((diff_file_size_done > 0) || (diff_no_of_files_done > 0))
      {
         int  length;
#ifdef _WITH_BURST_2
         char buffer[MAX_INT_LENGTH + 5 + MAX_OFF_T_LENGTH + 24 + MAX_INT_LENGTH + 11 + MAX_INT_LENGTH + 1];

         length = MAX_INT_LENGTH + 5 + MAX_OFF_T_LENGTH + 24 + MAX_INT_LENGTH + 11 + MAX_INT_LENGTH + 1;
#else
         char buffer[MAX_INT_LENGTH + 5 + MAX_OFF_T_LENGTH + 24 + MAX_INT_LENGTH + 1];

         length = MAX_INT_LENGTH + 5 + MAX_OFF_T_LENGTH + 24 + MAX_INT_LENGTH + 1;
#endif

         if ((move_flag & FILES_MOVED) && ((move_flag & FILES_COPIED) == 0))
         {
            WHAT_DONE_BUFFER(length, buffer, "moved",
                              diff_file_size_done, diff_no_of_files_done);
         }
         else if (((move_flag & FILES_MOVED) == 0) && (move_flag & FILES_COPIED))
              {
                 WHAT_DONE_BUFFER(length, buffer, "copied",
                                   diff_file_size_done, diff_no_of_files_done);
              }
              else
              {
                 WHAT_DONE_BUFFER(length, buffer, "copied/moved",
                                   diff_file_size_done, diff_no_of_files_done);
              }
#ifdef _WITH_BURST_2
         /* Write " [BURST]" */
         if (burst_2_counter == 1)
         {
            if ((length + 9) <= (MAX_INT_LENGTH + 5 + MAX_OFF_T_LENGTH + 24 + MAX_INT_LENGTH + 11 + MAX_INT_LENGTH + 1))
            {   
               buffer[length] = ' '; buffer[length + 1] = '[';
               buffer[length + 2] = 'B'; buffer[length + 3] = 'U';              
               buffer[length + 4] = 'R'; buffer[length + 5] = 'S';              
               buffer[length + 6] = 'T'; buffer[length + 7] = ']';              
               buffer[length + 8] = '\0';
            }
         }
         else if (burst_2_counter > 1)
              {
                 (void)snprintf(buffer + length,
                                MAX_INT_LENGTH + 5 + MAX_OFF_T_LENGTH + 24 + MAX_INT_LENGTH + 11 + MAX_INT_LENGTH + 1 - length,
                                " [BURST * %u]", burst_2_counter);
              }
#endif /* _WITH_BURST_2 */
         trans_log(INFO_SIGN, NULL, 0, NULL, NULL, "%s #%x",
                   buffer, db.id.job);
      }
      reset_fsa((struct job *)&db, exitflag, 0, 0);
   }

   free(file_name_buffer);
   free(file_size_buffer);

   send_proc_fin(NO);
   if (sys_log_fd != STDERR_FILENO)
   {
      (void)close(sys_log_fd);
   }

   return;
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
   if (fsa->job_status[(int)db.job_no].unique_name[2] == 5)
   {
      exit(SUCCESS);
   }
   else
   {
      exit(GOT_KILLED);
   }
}


/*++++++++++++++++++++++++++++++ sig_exit() +++++++++++++++++++++++++++++*/
static void
sig_exit(int signo)
{
   exit(INCORRECT);
}
