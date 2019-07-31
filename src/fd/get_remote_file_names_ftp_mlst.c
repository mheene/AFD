/*
 *  get_remote_file_names_ftp_mlst.c - Part of AFD, an automatic file
 *                                     distribution program.
 *  Copyright (c) 2013 - 2019 Holger Kiehl <Holger.Kiehl@dwd.de>
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

DESCR__S_M3
/*
 ** NAME
 **   get_remote_file_names_ftp_mlst - retrieves filename, size and date
 **
 ** SYNOPSIS
 **   int get_remote_file_names_ftp_mlst(off_t *file_size_to_retrieve,
 **                                      int   *more_files_in_list)
 **
 ** DESCRIPTION
 **
 ** RETURN VALUES
 **   None.
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   28.04.2013 H.Kiehl Created
 **   03.09.2017 H.Kiehl Added option to get only appended part.
 **   29.07.2019 H.Kiehl Added check if ls_data file is changed while we
 **                      work with it.
 **
 */
DESCR__E_M3

#include <stdio.h>
#include <string.h>                /* strcpy(), strerror(), memmove()    */
#include <stdlib.h>                /* malloc(), realloc(), free()        */
#include <ctype.h>                 /* isdigit()                          */
#include <time.h>                  /* time(), mktime(), strftime()       */ 
#ifdef TM_IN_SYS_TIME
# include <sys/time.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>                /* unlink()                           */
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include "ftpdefs.h"
#include "fddefs.h"

#define FTP_PERM_APPEND       1
#define FTP_PERM_CREATE       2
#define FTP_PERM_DELETE       4
#define FTP_PERM_ENTER        8
#define FTP_PERM_RNFR        16
#define FTP_PERM_LIST        32
#define FTP_PERM_MKDIR       64
#define FTP_PERM_PURGE      128
#define FTP_PERM_RETR       256
#define FTP_PERM_STOR       512

#define FTP_TYPE_FILE         1
#define FTP_TYPE_CDIR         2
#define FTP_TYPE_PDIR         4
#define FTP_TYPE_DIR          8
#define FTP_TYPE_OS_SPECIAL  16


/* External global variables. */
extern int                        *current_no_of_listed_files,
                                  exitflag,
                                  no_of_listed_files,
                                  rl_fd,
                                  timeout_flag;
extern char                       msg_str[],
                                  *p_work_dir;
extern off_t                      rl_size;
extern struct job                 db;
extern struct retrieve_list       *rl;
extern struct fileretrieve_status *fra;
extern struct filetransfer_status *fsa;

/* Local global variables. */
static time_t                     current_time;

/* Local function prototypes. */
static void                       do_scan(int *, off_t *, int *);
static int                        check_list(char *, off_t, time_t, int *,
                                             off_t *, int *);


/*################## get_remote_file_names_ftp_mlst() ###################*/
int
get_remote_file_names_ftp_mlst(off_t *file_size_to_retrieve,
                               int   *more_files_in_list)
{
   int files_to_retrieve = 0,
       i = 0,
       notified = NO;

   *file_size_to_retrieve = 0;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
   if ((*more_files_in_list == YES) ||
       (db.special_flag & DISTRIBUTED_HELPER_JOB) ||
       ((db.special_flag & OLD_ERROR_JOB) && (db.retries < 30) &&
        (fra[db.fra_pos].stupid_mode != YES) &&
        (fra[db.fra_pos].remove != YES)))
#else
   if (rl_fd == -1)
   {
try_attach_again:
      if (attach_ls_data(db.fra_pos, db.fsa_pos, db.special_flag,
                         YES) == INCORRECT)
      {
         (void)ftp_quit();
         exit(INCORRECT);
      }
      if ((db.special_flag & DISTRIBUTED_HELPER_JOB) &&
          ((fra[db.fra_pos].stupid_mode == YES) ||
           (fra[db.fra_pos].remove == YES)))
      {
# ifdef LOCK_DEBUG
         if (rlock_region(rl_fd, LOCK_RETR_PROC,
                          __FILE__, __LINE__) == LOCK_IS_SET)
# else
         if (rlock_region(rl_fd, LOCK_RETR_PROC) == LOCK_IS_SET)
# endif
         {
            if (i == 0)
            {
               system_log(DEBUG_SIGN, __FILE__, __LINE__,
                          "Hmm, lock is set. Assume ls_data file was just modified. Lets try it again. (job_no=%d fsa_pos=%d)",
                          (int)db.job_no, db.fsa_pos);
            }
            else
            {
               if (i == 30)
               {
                  trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
                            "Have waited %d seconds, but unable to get a lock. Terminating.",
                            (i * 100000) / 1000000);
                  (void)ftp_quit();
                  exit(SUCCESS);
               }
               my_usleep(100000L);
            }
            detach_ls_data(NO);
            i++;
            goto try_attach_again;
         }
      }
   }

   if ((*more_files_in_list == YES) ||
       (db.special_flag & DISTRIBUTED_HELPER_JOB) ||
       ((db.special_flag & OLD_ERROR_JOB) && (db.retries < 30)))
#endif
   {
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
      if (rl_fd == -1)
      {
         if (attach_ls_data(db.fra_pos, db.fsa_pos, db.special_flag, YES) == INCORRECT)
         {
            (void)ftp_quit();
            exit(INCORRECT);
         }
      }
#endif
      *more_files_in_list = NO;
      for (i = 0; i < no_of_listed_files; i++)
      {
         if (*current_no_of_listed_files != no_of_listed_files)
         {
            if (notified == NO)
            {
               system_log(DEBUG_SIGN, __FILE__, __LINE__,
                          "*current_no_of_listed_files (%d) != no_of_listed_files (%d) [fra_pos=%d] @%x",
                          *current_no_of_listed_files, no_of_listed_files,
                          db.fra_pos, db.id.dir);
               notified = YES;
            }
            if (i >= *current_no_of_listed_files)
            {
               system_log(DEBUG_SIGN, __FILE__, __LINE__,
                          "AND has been reduced!!! Bailing out!");

               /* Just in case we do not fall over this in some other */
               /* code path. Let's hope this does not break anything. */
               no_of_listed_files = *current_no_of_listed_files;
               break;
            }
         }
         if ((rl[i].retrieved == NO) && (rl[i].assigned == 0))
         {
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
            if ((fra[db.fra_pos].stupid_mode == YES) ||
                (fra[db.fra_pos].remove == YES) ||
                ((files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
                 (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size)))
#else
            if ((files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
                (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size))
#endif
            {
               /* Lock this file in list. */
#ifdef LOCK_DEBUG
               if (lock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i),
                               __FILE__, __LINE__) == LOCK_IS_NOT_SET)
#else
               if (lock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i)) == LOCK_IS_NOT_SET)
#endif
               {
                  if ((fra[db.fra_pos].ignore_size == -1) ||
                      ((fra[db.fra_pos].gt_lt_sign & ISIZE_EQUAL) &&
                       (fra[db.fra_pos].ignore_size == rl[i].size)) ||
                      ((fra[db.fra_pos].gt_lt_sign & ISIZE_LESS_THEN) &&
                       (fra[db.fra_pos].ignore_size < rl[i].size)) ||
                      ((fra[db.fra_pos].gt_lt_sign & ISIZE_GREATER_THEN) &&
                       (fra[db.fra_pos].ignore_size > rl[i].size)))
                  {
                     if ((rl[i].got_date == NO) ||
                         (fra[db.fra_pos].ignore_file_time == 0))
                     {
                        files_to_retrieve++;
                        if ((fra[db.fra_pos].stupid_mode == APPEND_ONLY) &&
                            (rl[i].size > rl[i].prev_size))
                        {
                           *file_size_to_retrieve += (rl[i].size - rl[i].prev_size);
                        }
                        else
                        {
                           *file_size_to_retrieve += rl[i].size;
                        }
                        rl[i].assigned = (unsigned char)db.job_no + 1;
                     }
                     else
                     {
                        time_t diff_time;

                        diff_time = current_time - rl[i].file_mtime;
                        if (((fra[db.fra_pos].gt_lt_sign & IFTIME_EQUAL) &&
                             (fra[db.fra_pos].ignore_file_time == diff_time)) ||
                            ((fra[db.fra_pos].gt_lt_sign & IFTIME_LESS_THEN) &&
                             (fra[db.fra_pos].ignore_file_time < diff_time)) ||
                            ((fra[db.fra_pos].gt_lt_sign & IFTIME_GREATER_THEN) &&
                             (fra[db.fra_pos].ignore_file_time > diff_time)))
                        {
                           files_to_retrieve++;
                           if ((fra[db.fra_pos].stupid_mode == APPEND_ONLY) &&
                               (rl[i].size > rl[i].prev_size))
                           {
                              *file_size_to_retrieve += (rl[i].size - rl[i].prev_size);
                           }
                           else
                           {
                              *file_size_to_retrieve += rl[i].size;
                           }
                           rl[i].assigned = (unsigned char)db.job_no + 1;
                        }
                     }
#ifdef DEBUG_ASSIGNMENT
                     trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
# if SIZEOF_OFF_T == 4
                               "%s assigned %d: file_name=%s assigned=%d size=%ld",
# else
                               "%s assigned %d: file_name=%s assigned=%d size=%lld",
# endif
                               (fra[db.fra_pos].ls_data_alias[0] == '\0') ? fra[db.fra_pos].dir_alias : fra[db.fra_pos].ls_data_alias,
                               i, rl[i].file_name, (int)rl[i].assigned,
                               (pri_off_t)rl[i].size);
#endif /* DEBUG_ASSIGNMENT */
                  }
#ifdef LOCK_DEBUG
                  unlock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i),
                                __FILE__, __LINE__);
#else
                  unlock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i));
#endif
               }
            }
            else
            {
               *more_files_in_list = YES;
               break;
            }
         }
      }
#ifndef DO_NOT_PARALLELIZE_ALL_FETCH
      if ((files_to_retrieve == 0) && (db.special_flag & OLD_ERROR_JOB))
      {
         do_scan(&files_to_retrieve, file_size_to_retrieve, more_files_in_list);
      }
#endif
   }
   else
   {
      do_scan(&files_to_retrieve, file_size_to_retrieve, more_files_in_list);
   }

   return(files_to_retrieve);
}


/*+++++++++++++++++++++++++++++++ do_scan() +++++++++++++++++++++++++++++*/
static void    
do_scan(int   *files_to_retrieve, 
        off_t *file_size_to_retrieve, 
        int   *more_files_in_list)
{
   unsigned int     files_deleted = 0,
                    list_length = 0;
   int              file_perm,
                    file_type,
                    gotcha,
                    j,
                    k,
                    nfg,           /* Number of file mask. */
                    status,
                    type;
   char             file_name[MAX_FILENAME_LENGTH + 1],
                    *mlist = NULL,
                    *p_end,
                    *p_mask,
                    *p_start;
   time_t           file_mtime;
   off_t            file_size,
                    file_size_deleted = 0,
                    list_size = 0;
   struct file_mask *fml = NULL;
   struct tm        *p_tm;

   /*
    * Get a directory listing from the remote site so we can see
    * what files are there.
    */
#ifdef WITH_SSL
   if (db.auth == BOTH)
   {
      type = MLSD_CMD | BUFFERED_LIST | ENCRYPT_DATA;
   }
   else
   {
#endif
      type = MLSD_CMD | BUFFERED_LIST;
#ifdef WITH_SSL
   }
#endif

   if ((status = ftp_list(db.mode_flag, type, &mlist)) != SUCCESS)
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, msg_str,
                "Failed to send MLSD command (%d).", status);
      (void)ftp_quit();
      exit(LIST_ERROR);
   }

   if (mlist != NULL)
   {
      int i;

      /* Get all file masks for this directory. */
      if ((j = read_file_mask(fra[db.fra_pos].dir_alias, &nfg, &fml)) == INCORRECT)
      {
         if (j == LOCKFILE_NOT_THERE)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       "Failed to set lock in file masks for %s, because the file is not there.",
                       fra[db.fra_pos].dir_alias);
         }
         else if (j == LOCK_IS_SET)
              {
                 system_log(ERROR_SIGN, __FILE__, __LINE__,
                            "Failed to get the file masks for %s, because lock is already set",
                            fra[db.fra_pos].dir_alias);
              }
              else
              {
                 system_log(ERROR_SIGN, __FILE__, __LINE__,
                            "Failed to get the file masks for %s. (%d)",
                            fra[db.fra_pos].dir_alias, j);
              }
         if (fml != NULL)
         {
            free(fml);
         }
         (void)ftp_quit();
         exit(INCORRECT);
      }

#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
      if ((fra[db.fra_pos].stupid_mode == YES) ||
          (fra[db.fra_pos].remove == YES))
      {
         if (reset_ls_data(db.fra_pos) == INCORRECT)
         {
            (void)ftp_quit();
            exit(INCORRECT);
         }
      }
      else
      {
         if (rl_fd == -1)
         {
            if (attach_ls_data(db.fra_pos, db.fsa_pos, db.special_flag,
                               YES) == INCORRECT)
            {
               (void)ftp_quit();
               exit(INCORRECT);
            }
         }
      }
#else
      if (rl_fd == -1)
      {
         if (attach_ls_data(db.fra_pos, db.fsa_pos, db.special_flag,
                            YES) == INCORRECT)
         {
            (void)ftp_quit();
            exit(INCORRECT);
         }
      }
      if ((fra[db.fra_pos].stupid_mode == YES) ||
          (fra[db.fra_pos].remove == YES))
      {
         /*
          * If all files from the previous listing have been
          * collected, lets reset the ls_data structure or otherwise
          * it keeps on growing forever.
          */
# ifdef LOCK_DEBUG
         if (lock_region(rl_fd, LOCK_RETR_PROC,
                         __FILE__, __LINE__) == LOCK_IS_NOT_SET)
# else   
         if (lock_region(rl_fd, LOCK_RETR_PROC) == LOCK_IS_NOT_SET)
# endif
         {
            if (reset_ls_data(db.fra_pos) == INCORRECT)
            {
               (void)ftp_quit();
               exit(INCORRECT);
            }
         }
# ifdef LOCK_DEBUG
         unlock_region(rl_fd, LOCK_RETR_PROC, __FILE__, __LINE__);
# else      
         unlock_region(rl_fd, LOCK_RETR_PROC);
# endif
      }
#endif

      if ((fra[db.fra_pos].ignore_file_time != 0) ||
          (fra[db.fra_pos].delete_files_flag & UNKNOWN_FILES) ||
          (fra[db.fra_pos].delete_files_flag & OLD_RLOCKED_FILES))
      {
         /* Note: FTP returns GMT so we need to convert this to GMT! */
         current_time = time(NULL);
         p_tm = gmtime(&current_time);
         current_time = mktime(p_tm);
      }

      /*
       * Evaluate the list from the MLSD command.
       */
      p_end = mlist;
      do
      {
         file_mtime = 0;
         file_perm = -1;
         file_size = 0;
         file_type = 0;

         /* Evaluate one line. */
         do
         {
            /* modify=YYYYMMDDHHMMSS[.sss]; */
            if (((*p_end == 'M') || (*p_end == 'm')) &&
                ((*(p_end + 1) == 'O')  || (*(p_end + 1) == 'o')) &&
                ((*(p_end + 2) == 'D')  || (*(p_end + 2) == 'd')) &&
                ((*(p_end + 3) == 'I')  || (*(p_end + 3) == 'i')) &&
                ((*(p_end + 4) == 'F')  || (*(p_end + 4) == 'f')) &&
                ((*(p_end + 5) == 'Y')  || (*(p_end + 5) == 'y')) &&
                (*(p_end + 6) == '=') &&
                (isdigit((int)(*(p_end + 7)))) &&
                (isdigit((int)(*(p_end + 8)))) &&
                (isdigit((int)(*(p_end + 9)))) &&
                (isdigit((int)(*(p_end + 10)))) &&
                (isdigit((int)(*(p_end + 11)))) &&
                (isdigit((int)(*(p_end + 12)))) &&
                (isdigit((int)(*(p_end + 13)))) &&
                (isdigit((int)(*(p_end + 14)))) &&
                (isdigit((int)(*(p_end + 15)))) &&
                (isdigit((int)(*(p_end + 16)))) &&
                (isdigit((int)(*(p_end + 17)))) &&
                (isdigit((int)(*(p_end + 18)))) &&
                (isdigit((int)(*(p_end + 19)))) &&
                (isdigit((int)(*(p_end + 20)))))
            {
               struct tm bd_time;

               bd_time.tm_isdst = 0;
               bd_time.tm_year  = (((*(p_end + 7) - '0') * 1000) +
                                  ((*(p_end + 8) - '0') * 100) +
                                  ((*(p_end + 9) - '0') * 10) +
                                  (*(p_end + 10) - '0')) - 1900;
               bd_time.tm_mon   = ((*(p_end + 11) - '0') * 10) +
                                  *(p_end + 12) - '0' - 1;
               bd_time.tm_mday  = ((*(p_end + 13) - '0') * 10) +
                                  *(p_end + 14) - '0';
               bd_time.tm_hour  = ((*(p_end + 15) - '0') * 10) +
                                  *(p_end + 16) - '0';
               bd_time.tm_min   = ((*(p_end + 17) - '0') * 10) +
                                  *(p_end + 18) - '0';
               bd_time.tm_sec   = ((*(p_end + 19) - '0') * 10) +
                                  *(p_end + 20) - '0';
               file_mtime = mktime(&bd_time);

               p_end += 21;
            }
                 /* perm=[acdeflmprw]; */
            else if (((*p_end == 'P') || (*p_end == 'p')) &&
                     ((*(p_end + 1) == 'E')  || (*(p_end + 1) == 'e')) &&
                     ((*(p_end + 2) == 'R')  || (*(p_end + 2) == 'r')) &&
                     ((*(p_end + 3) == 'M')  || (*(p_end + 3) == 'm')) &&
                     (*(p_end + 4) == '='))
                 {
                    p_end += 5;
                    file_perm = 0;
                    while ((*p_end != ';') && (*p_end != ' ') &&
                           (*p_end != '\0'))
                    {
                       switch (*p_end)
                       {
                          case 'a' :
                          case 'A' : /* APPEND (file) */
                             file_perm |= FTP_PERM_APPEND;
                             break;
                          case 'c' :
                          case 'C' : /* CREATE (dir|cdir|pdir) */
                             file_perm |= FTP_PERM_CREATE;
                             break;
                          case 'd' :
                          case 'D' : /* DELETE (all) */
                             file_perm |= FTP_PERM_DELETE;
                             break;
                          case 'e' :
                          case 'E' : /* ENTER (dir|cdir|pdir) */
                             file_perm |= FTP_PERM_ENTER;
                             break;
                          case 'f' :
                          case 'F' : /* RNFR (all) */
                             file_perm |= FTP_PERM_RNFR;
                             break;
                          case 'l' :
                          case 'L' : /* LIST (dir|cdir|pdir) */
                             file_perm |= FTP_PERM_LIST;
                             break;
                          case 'm' :
                          case 'M' : /* MKDIR (dir|cdir|pdir) */
                             file_perm |= FTP_PERM_MKDIR;
                             break;
                          case 'p' :
                          case 'P' : /* PURGE (dir|cdir|pdir) */
                             file_perm |= FTP_PERM_PURGE;
                             break;
                          case 'r' :
                          case 'R' : /* RETR aka read (file) */
                             file_perm |= FTP_PERM_RETR;
                             break;
                          case 'w' :
                          case 'W' : /* STOR aka write (file) */
                             file_perm |= FTP_PERM_STOR;
                             break;
                          default  : /* Unknown permission. */
                             break;
                       }
                       p_end++;
                    }
                 }
                 /* size=[0123456789]; */
            else if (((*p_end == 'S') || (*p_end == 's')) &&
                     ((*(p_end + 1) == 'I')  || (*(p_end + 1) == 'i')) &&
                     ((*(p_end + 2) == 'Z')  || (*(p_end + 2) == 'z')) &&
                     ((*(p_end + 3) == 'E')  || (*(p_end + 3) == 'e')) &&
                     (*(p_end + 4) == '='))
                 {
                    p_end += 5;
                    p_start = p_end;
                    while (isdigit((int)(*p_end)))
                    {
                       p_end++;
                    }
                    if ((p_end != p_start) && (*p_end == ';'))
                    {
                       *p_end = '\0';
                       file_size = (off_t)str2offt(p_start, NULL, 10);
                       *p_end = ';';
                    }
                 }
                 /* type=file|cdir|pdir|dir|OS.name=type; */
            else if (((*p_end == 'T') || (*p_end == 't')) &&
                     ((*(p_end + 1) == 'Y')  || (*(p_end + 1) == 'y')) &&
                     ((*(p_end + 2) == 'P')  || (*(p_end + 2) == 'p')) &&
                     ((*(p_end + 3) == 'E')  || (*(p_end + 3) == 'e')) &&
                     (*(p_end + 4) == '='))
                 {
                    p_end += 5;

                    /* file */
                    if (((*p_end == 'F') || (*p_end == 'f')) &&
                        ((*(p_end + 1) == 'I') || (*(p_end + 1) == 'i')) &&
                        ((*(p_end + 2) == 'L') || (*(p_end + 2) == 'l')) &&
                        ((*(p_end + 3) == 'E') || (*(p_end + 3) == 'e')))
                    {
                       file_type = FTP_TYPE_FILE;
                       p_end += 4;
                    }
                         /* cdir */
                    else if (((*p_end == 'C') || (*p_end == 'c')) &&
                             ((*(p_end + 1) == 'D') || (*(p_end + 1) == 'd')) &&
                             ((*(p_end + 2) == 'I') || (*(p_end + 2) == 'i')) &&
                             ((*(p_end + 3) == 'R') || (*(p_end + 3) == 'r')))
                         {
                            file_type = FTP_TYPE_CDIR;
                            p_end += 4;
                         }
                         /* pdir */
                    else if (((*p_end == 'P') || (*p_end == 'p')) &&
                             ((*(p_end + 1) == 'D') || (*(p_end + 1) == 'd')) &&
                             ((*(p_end + 2) == 'I') || (*(p_end + 2) == 'i')) &&
                             ((*(p_end + 3) == 'R') || (*(p_end + 3) == 'r')))
                         {
                            file_type = FTP_TYPE_PDIR;
                            p_end += 4;
                         }
                         /* dir */
                    else if (((*p_end == 'D') || (*p_end == 'd')) &&
                             ((*(p_end + 1) == 'I') || (*(p_end + 1) == 'i')) &&
                             ((*(p_end + 2) == 'R') || (*(p_end + 2) == 'r')))
                         {
                            file_type = FTP_TYPE_DIR;
                            p_end += 3;
                         }
                         /* OS.name= */
                    else if (((*p_end == 'O') || (*p_end == 'o')) &&
                             ((*(p_end + 1) == 'S') || (*(p_end + 1) == 's')) &&
                             (*(p_end + 2) == '.'))
                         {
                            /*
                             * Lets first just know about symbolic link
                             * and treat this if it was a normal file. This
                             * is what we always did. All other types we
                             * will just ignore.
                             */
                            p_end += 3;

                            /* unix=slink.... */
                            if (((*p_end == 'U') || (*p_end == 'u')) &&
                                ((*(p_end + 1) == 'N') || (*(p_end + 1) == 'n')) &&
                                ((*(p_end + 2) == 'I') || (*(p_end + 2) == 'i')) &&
                                ((*(p_end + 3) == 'X') || (*(p_end + 3) == 'x')) &&
                                (*(p_end + 4) == '=') &&
                                ((*(p_end + 5) == 'S') || (*(p_end + 5) == 's')) &&
                                ((*(p_end + 6) == 'L') || (*(p_end + 6) == 'l')) &&
                                ((*(p_end + 7) == 'I') || (*(p_end + 7) == 'i')) &&
                                ((*(p_end + 8) == 'N') || (*(p_end + 8) == 'n')) &&
                                ((*(p_end + 9) == 'K') || (*(p_end + 9) == 'k')))
                            {
                               file_type = FTP_TYPE_FILE;
                               p_end += 10;
                            }
                            else
                            {
                               file_type = FTP_TYPE_OS_SPECIAL;
                            }
                         }
                 }

            while ((*p_end != ';') && (*p_end != ' ') && (*p_end != '\0'))
            {
               p_end++;
            }
            while (*p_end == ';')
            {
               p_end++;
            }
         } while ((*p_end != ' ') && (*p_end != '\0'));

         if ((*p_end == ' ') &&
             ((*(p_end + 1) != '.') ||
              (fra[db.fra_pos].dir_flag & ACCEPT_DOT_FILES)) &&
             (file_type == FTP_TYPE_FILE) &&
             ((file_perm == -1) || (file_perm & FTP_PERM_RETR)))
         {
            list_length++;
            list_size += file_size;

            p_end++;
            i = 0;
            while ((*p_end != '\r') && (*p_end != '\n') &&
                   (*p_end != '\0') && (i < MAX_FILENAME_LENGTH))
            {
               file_name[i] = *p_end;
               i++; p_end++;
            }
            file_name[i] = '\0';

            if ((*p_end == '\r') || (*p_end == '\n'))
            {
               if (fra[db.fra_pos].dir_flag == ALL_DISABLED)
               {
                  delete_remote_file(FTP, file_name, i,
#ifdef _DELETE_LOG
                                     DELETE_HOST_DISABLED,
#endif
                                     &files_deleted, &file_size_deleted,
                                     file_size);
               }
               else
               {
                  gotcha = NO;
                  for (k = 0; k < nfg; k++)
                  {
                     p_mask = fml[k].file_list;
                     for (j = 0; j < fml[k].fc; j++)
                     {
                        if ((status = pmatch(p_mask, file_name, NULL)) == 0)
                        {
                           if (check_list(file_name, file_size, file_mtime,
                                          files_to_retrieve,
                                          file_size_to_retrieve,
                                          more_files_in_list) == 0)
                           {
                              gotcha = YES;
                           }
                           else
                           {
                              gotcha = NEITHER;
                           }
                           break;
                        }
                        else if (status == 1)
                             {
                                /* This file is definitly NOT wanted! */
                                /* Lets skip the rest of this group.  */
                                break;
                             }
#ifdef SHOW_FILTER_MISSES
                        if ((status == -1) ||
                            (fsa->debug > NORMAL_MODE))
                        {
                           char tmp_mask[MAX_FILENAME_LENGTH];

                           if (expand_filter(p_mask, tmp_mask, time(NULL)) == YES)
                           {
                              trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                           "%s (%s) not fitting %s",
                                           p_mask, tmp_mask, file_name);
                           }
                           else
                           {
                              trans_db_log(INFO_SIGN, __FILE__, __LINE__, NULL,
                                           "%s not fitting %s",
                                           p_mask, file_name);
                           }
                        }
#endif
                        NEXT(p_mask);
                     }
                     if ((gotcha == YES) || (gotcha == NEITHER))
                     {
                        break;
                     }
                  }

                  if ((gotcha == NO) && (status != 0) &&
                      (fra[db.fra_pos].delete_files_flag & UNKNOWN_FILES))
                  {
                     time_t diff_time = current_time - file_mtime;

                     if ((fra[db.fra_pos].unknown_file_time == -2) ||
                         ((diff_time > fra[db.fra_pos].unknown_file_time) &&
                          (diff_time > DEFAULT_TRANSFER_TIMEOUT)))
                     {
                        delete_remote_file(FTP, file_name, i,
#ifdef _DELETE_LOG
                                           DEL_UNKNOWN_FILE,
#endif
                                           &files_deleted,
                                           &file_size_deleted, file_size);
                     }
                  }
               }
            }
            else if (i >= MAX_FILENAME_LENGTH)
                 {
                    trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                              "Remote file name `%s' is to long, it may only be %d bytes long.",
                              file_name, MAX_FILENAME_LENGTH);
                    continue;
                 }
                 else
                 {
                    trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                              "Premature end of remote file name `%s'.",
                              file_name);
                    continue;
                 }
         }
         else
         {
            if ((*(p_end + 1) == '.') && (file_type == FTP_TYPE_FILE) &&
                (file_perm & FTP_PERM_DELETE) &&
                (fra[db.fra_pos].delete_files_flag & OLD_RLOCKED_FILES) &&
                (fra[db.fra_pos].locked_file_time != -1))
            {
               time_t diff_time = current_time - file_mtime;

               if (diff_time < 0)
               {
                  diff_time = 0;
               }
               if ((diff_time > fra[db.fra_pos].locked_file_time) &&
                   (diff_time > DEFAULT_TRANSFER_TIMEOUT))
               {
                  p_end++;
                  i = 0;
                  while ((*p_end != '\r') && (*p_end != '\n') &&
                         (*p_end != '\0') && (i < MAX_FILENAME_LENGTH))
                  {
                     file_name[i] = *p_end;
                     i++; p_end++;
                  }
                  file_name[i] = '\0';

                  if ((*p_end == '\r') || (*p_end == '\n'))
                  {
                     delete_remote_file(FTP, file_name, i,
#ifdef _DELETE_LOG
                                        DEL_OLD_LOCKED_FILE,
#endif
                                        &files_deleted,
                                        &file_size_deleted, file_size);
                  }
                  else if (i >= MAX_FILENAME_LENGTH)
                       {
                          trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                    "Remote file name `%s' is to long, it may only be %d bytes long.",
                                    file_name, MAX_FILENAME_LENGTH);
                          continue;
                       }
                       else
                       {
                          trans_log(ERROR_SIGN, __FILE__, __LINE__, NULL, NULL,
                                    "Premature end of remote file name `%s'.",
                                    file_name);
                          continue;
                       }
               }
            }
         }

         while ((*p_end != '\r') && (*p_end != '\n') && (*p_end != '\0'))
         {
            p_end++;
         }
         while ((*p_end == '\r') || (*p_end == '\n'))
         {
            p_end++;
         }
      } while (*p_end != '\0');

      free(mlist);

      /* Free file mask list. */
      for (i = 0; i < nfg; i++)
      {
         free(fml[i].file_list);
      }
      free(fml);
   }

   if (files_deleted > 0)
   {
      trans_log(INFO_SIGN, NULL, 0, NULL, NULL,
#if SIZEOF_OFF_T == 4
                "%d files %ld bytes found for retrieving %s[%u files with %ld bytes in %s (deleted %u files with %ld bytes)]. @%x",
#else
                "%d files %lld bytes found for retrieving %s[%u files with %lld bytes in %s (deleted %u files with %lld bytes)]. @%x",
#endif
                *files_to_retrieve, (pri_off_t)(*file_size_to_retrieve),
                (*more_files_in_list == YES) ? "(+) " : "",
                list_length, (pri_off_t)list_size,
                (db.target_dir[0] == '\0') ? "home dir" : db.target_dir,
                files_deleted, (pri_off_t)file_size_deleted, db.id.dir);
   }
   else
   {
      trans_log(INFO_SIGN, NULL, 0, NULL, NULL,
#if SIZEOF_OFF_T == 4
                "%d files %ld bytes found for retrieving %s[%u files with %ld bytes in %s]. @%x",
#else
                "%d files %lld bytes found for retrieving %s[%u files with %lld bytes in %s]. @%x",
#endif
                *files_to_retrieve, (pri_off_t)(*file_size_to_retrieve),
                (*more_files_in_list == YES) ? "(+) " : "",
                list_length, (pri_off_t)list_size,
                (db.target_dir[0] == '\0') ? "home dir" : db.target_dir,
                db.id.dir);
   }

   /*
    * Remove all files from the remote_list structure that are not
    * in the current buffer.
    */
   if ((fra[db.fra_pos].stupid_mode != YES) &&
       (fra[db.fra_pos].remove == NO))
   {
      int    files_removed = 0,
             i;
      size_t move_size;

      for (i = 0; i < (no_of_listed_files - files_removed); i++)
      {
         if (rl[i].in_list == NO)
         {
            int j = i;

            while ((j < (no_of_listed_files - files_removed)) &&
                   (rl[j].in_list == NO))
            {
               j++;
            }
            if (j != (no_of_listed_files - files_removed))
            {
               move_size = (no_of_listed_files - files_removed - j) *
                           sizeof(struct retrieve_list);
               (void)memmove(&rl[i], &rl[j], move_size);
            }
            files_removed += (j - i);
         }
      }

      if (files_removed > 0)
      {
         int    tmp_current_no_of_listed_files = no_of_listed_files;
         size_t new_size,
                old_size;

         no_of_listed_files -= files_removed;
         if (no_of_listed_files < 0)
         {
            system_log(DEBUG_SIGN, __FILE__, __LINE__,
                       "Hmmm, no_of_listed_files = %d", no_of_listed_files);
            no_of_listed_files = 0;
         }
         if (no_of_listed_files == 0)
         {
            new_size = (RETRIEVE_LIST_STEP_SIZE * sizeof(struct retrieve_list)) +
                       AFD_WORD_OFFSET;
         }
         else
         {
            new_size = (((no_of_listed_files / RETRIEVE_LIST_STEP_SIZE) + 1) *
                        RETRIEVE_LIST_STEP_SIZE * sizeof(struct retrieve_list)) +
                       AFD_WORD_OFFSET;
         }
         old_size = (((tmp_current_no_of_listed_files / RETRIEVE_LIST_STEP_SIZE) + 1) *
                     RETRIEVE_LIST_STEP_SIZE * sizeof(struct retrieve_list)) +
                    AFD_WORD_OFFSET;

         if (old_size != new_size)
         {
            char   *ptr;

            ptr = (char *)rl - AFD_WORD_OFFSET;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
            if ((fra[db.fra_pos].stupid_mode == YES) ||
                (fra[db.fra_pos].remove == YES))
            {
               if ((ptr = realloc(ptr, new_size)) == NULL)
               {
                  system_log(ERROR_SIGN, __FILE__, __LINE__,
                             "realloc() error : %s", strerror(errno));
                  (void)ftp_quit();
                  exit(INCORRECT);
               }
            }
            else
            {
#endif
               if ((ptr = mmap_resize(rl_fd, ptr, new_size)) == (caddr_t) -1)
               {
                  system_log(ERROR_SIGN, __FILE__, __LINE__,
                             "mmap_resize() error : %s", strerror(errno));
                  (void)ftp_quit();
                  exit(INCORRECT);
               }
               rl_size = new_size;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
            }
#endif
            current_no_of_listed_files = (int *)ptr;
            ptr += AFD_WORD_OFFSET;
            rl = (struct retrieve_list *)ptr;
         }
         *(int *)((char *)rl - AFD_WORD_OFFSET) = no_of_listed_files;
      }
   }

   return;
}


/*+++++++++++++++++++++++++++++ check_list() ++++++++++++++++++++++++++++*/
static int
check_list(char   *file,
           off_t  file_size,
           time_t file_mtime,
           int    *files_to_retrieve,
           off_t  *file_size_to_retrieve,
           int    *more_files_in_list)
{
   int i;

   if ((fra[db.fra_pos].stupid_mode == YES) ||
       (fra[db.fra_pos].remove == YES))
   {
      for (i = 0; i < no_of_listed_files; i++)
      {
         if (CHECK_STRCMP(rl[i].file_name, file) == 0)
         {
            rl[i].in_list = YES;
            if (((rl[i].assigned == 0) || (rl[i].retrieved == YES)) &&
                (((db.special_flag & OLD_ERROR_JOB) == 0) ||
#ifdef LOCK_DEBUG
                   (lock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i),
                                __FILE__, __LINE__) == LOCK_IS_NOT_SET)
#else
                   (lock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i)) == LOCK_IS_NOT_SET)
#endif
                  ))
            {
               int ret;

               rl[i].file_mtime = file_mtime;
               rl[i].got_date = YES;
               rl[i].size = file_size;
               rl[i].prev_size = 0;

               if ((fra[db.fra_pos].ignore_size == -1) ||
                   ((fra[db.fra_pos].gt_lt_sign & ISIZE_EQUAL) &&
                    (fra[db.fra_pos].ignore_size == rl[i].size)) ||
                   ((fra[db.fra_pos].gt_lt_sign & ISIZE_LESS_THEN) &&
                    (fra[db.fra_pos].ignore_size < rl[i].size)) ||
                   ((fra[db.fra_pos].gt_lt_sign & ISIZE_GREATER_THEN) &&
                    (fra[db.fra_pos].ignore_size > rl[i].size)))
               {
                  if (fra[db.fra_pos].ignore_file_time == 0)
                  {
                     *file_size_to_retrieve += rl[i].size;
                     *files_to_retrieve += 1;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
                     if ((fra[db.fra_pos].stupid_mode == YES) ||
                         (fra[db.fra_pos].remove == YES) ||
                         ((*files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
                          (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size)))
#else
                     if ((*files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
                         (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size))
#endif
                     {
                        rl[i].retrieved = NO;
                        rl[i].assigned = (unsigned char)db.job_no + 1;
                     }
                     else
                     {
                        rl[i].assigned = 0;
                        *file_size_to_retrieve -= rl[i].size;
                        *files_to_retrieve -= 1;
                        *more_files_in_list = YES;
                     }
                     ret = 0;
                  }
                  else
                  {
                     time_t diff_time;

                     diff_time = current_time - rl[i].file_mtime;
                     if (((fra[db.fra_pos].gt_lt_sign & IFTIME_EQUAL) &&
                          (fra[db.fra_pos].ignore_file_time == diff_time)) ||
                         ((fra[db.fra_pos].gt_lt_sign & IFTIME_LESS_THEN) &&
                          (fra[db.fra_pos].ignore_file_time < diff_time)) ||
                         ((fra[db.fra_pos].gt_lt_sign & IFTIME_GREATER_THEN) &&
                          (fra[db.fra_pos].ignore_file_time > diff_time)))
                     {
                        *file_size_to_retrieve += rl[i].size;
                        *files_to_retrieve += 1;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
                        if ((fra[db.fra_pos].stupid_mode == YES) ||
                            (fra[db.fra_pos].remove == YES) ||
                            ((*files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
                             (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size)))
#else
                        if ((*files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
                            (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size))
#endif
                        {
                           rl[i].retrieved = NO;
                           rl[i].assigned = (unsigned char)db.job_no + 1;
                        }
                        else
                        {
                           rl[i].assigned = 0;
                           *file_size_to_retrieve -= rl[i].size;
                           *files_to_retrieve -= 1;
                           *more_files_in_list = YES;
                        }
                        ret = 0;
                     }
                     else
                     {
                        ret = 1;
                     }
                  }
#ifdef DEBUG_ASSIGNMENT
                  trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
# if SIZEOF_OFF_T == 4
                            "%s assigned %d: file_name=%s assigned=%d size=%ld",
# else
                            "%s assigned %d: file_name=%s assigned=%d size=%lld",
# endif
                            (fra[db.fra_pos].ls_data_alias[0] == '\0') ? fra[db.fra_pos].dir_alias : fra[db.fra_pos].ls_data_alias,
                            i, rl[i].file_name, (int)rl[i].assigned,
                            (pri_off_t)rl[i].size);
#endif /* DEBUG_ASSIGNMENT */
               }
               else
               {
                  ret = 1;
               }
               if (db.special_flag & OLD_ERROR_JOB)
               {
#ifdef LOCK_DEBUG
                  unlock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i),
                                __FILE__, __LINE__);
#else
                  unlock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i));
#endif
               }
               return(ret);
            }
            else
            {
               return(1);
            }
         }
      } /* for (i = 0; i < no_of_listed_files; i++) */
   }
   else
   {
      /* Check if this file is in the list. */
      for (i = 0; i < no_of_listed_files; i++)
      {
         if (CHECK_STRCMP(rl[i].file_name, file) == 0)
         {
            rl[i].in_list = YES;
            if ((rl[i].assigned != 0) ||
                ((fra[db.fra_pos].stupid_mode == GET_ONCE_ONLY) &&
                 (rl[i].retrieved == YES)))
            {
               return(1);
            }

            if (((db.special_flag & OLD_ERROR_JOB) == 0) ||
#ifdef LOCK_DEBUG
                (lock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i),
                             __FILE__, __LINE__) == LOCK_IS_NOT_SET)
#else
                (lock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i)) == LOCK_IS_NOT_SET)
#endif
               )
            {
               int   ret;
               off_t prev_size = 0;

               if (rl[i].file_mtime != file_mtime)
               {
                  rl[i].file_mtime = file_mtime;
                  rl[i].retrieved = NO;
                  rl[i].assigned = 0;
               }
               rl[i].got_date = YES;
               if (rl[i].size != file_size)
               {
                  prev_size = rl[i].size;
                  rl[i].size = file_size;
                  rl[i].retrieved = NO;
                  rl[i].assigned = 0;
               }
               if (rl[i].retrieved == NO)
               {
                  if ((fra[db.fra_pos].ignore_size == -1) ||
                      ((fra[db.fra_pos].gt_lt_sign & ISIZE_EQUAL) &&
                       (fra[db.fra_pos].ignore_size == rl[i].size)) ||
                      ((fra[db.fra_pos].gt_lt_sign & ISIZE_LESS_THEN) &&
                       (fra[db.fra_pos].ignore_size < rl[i].size)) ||
                      ((fra[db.fra_pos].gt_lt_sign & ISIZE_GREATER_THEN) &&
                       (fra[db.fra_pos].ignore_size > rl[i].size)))
                  {
                     off_t size_to_retrieve;

                     if ((rl[i].got_date == NO) ||
                         (fra[db.fra_pos].ignore_file_time == 0))
                     {
                        if ((fra[db.fra_pos].stupid_mode == APPEND_ONLY) &&
                            (rl[i].size > prev_size))
                        {
                           size_to_retrieve = rl[i].size - prev_size;
                        }
                        else
                        {
                           size_to_retrieve = rl[i].size;
                        }
                        rl[i].prev_size = prev_size;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
                        if ((fra[db.fra_pos].stupid_mode == YES) ||
                            (fra[db.fra_pos].remove == YES) ||
                            (((*files_to_retrieve + 1) < fra[db.fra_pos].max_copied_files) &&
                             ((*file_size_to_retrieve + size_to_retrieve) < fra[db.fra_pos].max_copied_file_size)))
#else
                        if (((*files_to_retrieve + 1) < fra[db.fra_pos].max_copied_files) &&
                            ((*file_size_to_retrieve + size_to_retrieve) < fra[db.fra_pos].max_copied_file_size))
#endif
                        {
                           rl[i].assigned = (unsigned char)db.job_no + 1;
                           *file_size_to_retrieve += size_to_retrieve;
                           *files_to_retrieve += 1;
                        }
                        else
                        {
                           rl[i].assigned = 0;
                           *more_files_in_list = YES;
                        }
                        ret = 0;
                     }
                     else
                     {
                        time_t diff_time;

                        diff_time = current_time - rl[i].file_mtime;
                        if (((fra[db.fra_pos].gt_lt_sign & IFTIME_EQUAL) &&
                             (fra[db.fra_pos].ignore_file_time == diff_time)) ||
                            ((fra[db.fra_pos].gt_lt_sign & IFTIME_LESS_THEN) &&
                             (fra[db.fra_pos].ignore_file_time < diff_time)) ||
                            ((fra[db.fra_pos].gt_lt_sign & IFTIME_GREATER_THEN) &&
                             (fra[db.fra_pos].ignore_file_time > diff_time)))
                        {
                           if ((fra[db.fra_pos].stupid_mode == APPEND_ONLY) &&
                               (rl[i].size > prev_size))
                           {
                              size_to_retrieve = rl[i].size - prev_size;
                           }
                           else
                           {
                              size_to_retrieve = rl[i].size;
                           }
                           rl[i].prev_size = prev_size;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
                           if ((fra[db.fra_pos].stupid_mode == YES) ||
                               (fra[db.fra_pos].remove == YES) ||
                               (((*files_to_retrieve + 1) < fra[db.fra_pos].max_copied_files) &&
                                ((*file_size_to_retrieve + size_to_retrieve) < fra[db.fra_pos].max_copied_file_size)))
#else
                           if (((*files_to_retrieve + 1) < fra[db.fra_pos].max_copied_files) &&
                               ((*file_size_to_retrieve + size_to_retrieve) < fra[db.fra_pos].max_copied_file_size))
#endif
                           {
                              rl[i].assigned = (unsigned char)db.job_no + 1;
                              *file_size_to_retrieve += size_to_retrieve;
                              *files_to_retrieve += 1;
                           }
                           else
                           {
                              rl[i].assigned = 0;
                              *more_files_in_list = YES;
                           }
                           ret = 0;
                        }
                        else
                        {
                           ret = 1;
                        }
                     }
#ifdef DEBUG_ASSIGNMENT
                     trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
# if SIZEOF_OFF_T == 4
                               "%s assigned %d: file_name=%s assigned=%d size=%ld",
# else
                               "%s assigned %d: file_name=%s assigned=%d size=%lld",
# endif
                               (fra[db.fra_pos].ls_data_alias[0] == '\0') ? fra[db.fra_pos].dir_alias : fra[db.fra_pos].ls_data_alias,
                               i, rl[i].file_name, (int)rl[i].assigned,
                               (pri_off_t)rl[i].size);
#endif /* DEBUG_ASSIGNMENT */
                  }
                  else
                  {
                     ret = 1;
                  }
               }
               else
               {
                  ret = 1;
               }
               if (db.special_flag & OLD_ERROR_JOB)
               {
#ifdef LOCK_DEBUG
                  unlock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i), __FILE__, __LINE__);
#else
                  unlock_region(rl_fd, (off_t)(LOCK_RETR_FILE + i));
#endif
               }
               return(ret);
            }
            else
            {
               return(1);
            }
         }
      } /* for (i = 0; i < no_of_listed_files; i++) */
   }

   /* Add this file to the list. */
   if ((no_of_listed_files != 0) &&
       ((no_of_listed_files % RETRIEVE_LIST_STEP_SIZE) == 0))
   {
      char   *ptr;
      size_t new_size = (((no_of_listed_files / RETRIEVE_LIST_STEP_SIZE) + 1) *
                         RETRIEVE_LIST_STEP_SIZE * sizeof(struct retrieve_list)) +
                         AFD_WORD_OFFSET;

      ptr = (char *)rl - AFD_WORD_OFFSET;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
      if ((fra[db.fra_pos].stupid_mode == YES) ||
          (fra[db.fra_pos].remove == YES))
      {
         if ((ptr = realloc(ptr, new_size)) == NULL)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       "realloc() error : %s", strerror(errno));
            (void)ftp_quit();
            exit(INCORRECT);
         }
      }
      else
      {
#endif
         if ((ptr = mmap_resize(rl_fd, ptr, new_size)) == (caddr_t) -1)
         {
            system_log(ERROR_SIGN, __FILE__, __LINE__,
                       "mmap_resize() error : %s", strerror(errno));
            (void)ftp_quit();
            exit(INCORRECT);
         }
         rl_size = new_size;
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
      }
#endif
      if (no_of_listed_files < 0)
      {
         system_log(DEBUG_SIGN, __FILE__, __LINE__,
                    "Hmmm, no_of_listed_files = %d", no_of_listed_files);
         no_of_listed_files = 0;
      }
      *(int *)ptr = no_of_listed_files;
      current_no_of_listed_files = (int *)ptr;
      ptr += AFD_WORD_OFFSET;
      rl = (struct retrieve_list *)ptr;
   }
   (void)strcpy(rl[no_of_listed_files].file_name, file);
   rl[no_of_listed_files].retrieved = NO;
   rl[no_of_listed_files].in_list = YES;
   rl[no_of_listed_files].size = file_size;
   rl[no_of_listed_files].prev_size = 0;
   rl[no_of_listed_files].file_mtime = file_mtime;
   rl[no_of_listed_files].got_date = YES;

   if ((fra[db.fra_pos].ignore_size == -1) ||
       ((fra[db.fra_pos].gt_lt_sign & ISIZE_EQUAL) &&
        (fra[db.fra_pos].ignore_size == rl[no_of_listed_files].size)) ||
       ((fra[db.fra_pos].gt_lt_sign & ISIZE_LESS_THEN) &&
        (fra[db.fra_pos].ignore_size < rl[no_of_listed_files].size)) ||
       ((fra[db.fra_pos].gt_lt_sign & ISIZE_GREATER_THEN) &&
        (fra[db.fra_pos].ignore_size > rl[no_of_listed_files].size)))
   {
      if ((rl[no_of_listed_files].got_date == NO) ||
          (fra[db.fra_pos].ignore_file_time == 0))
      {
         *file_size_to_retrieve += file_size;
         *files_to_retrieve += 1;
         no_of_listed_files++;
      }
      else
      {
         time_t diff_time;

         diff_time = current_time - rl[no_of_listed_files].file_mtime;
         if (((fra[db.fra_pos].gt_lt_sign & IFTIME_EQUAL) &&
              (fra[db.fra_pos].ignore_file_time == diff_time)) ||
             ((fra[db.fra_pos].gt_lt_sign & IFTIME_LESS_THEN) &&
              (fra[db.fra_pos].ignore_file_time < diff_time)) ||
             ((fra[db.fra_pos].gt_lt_sign & IFTIME_GREATER_THEN) &&
                 (fra[db.fra_pos].ignore_file_time > diff_time)))
         {
            *file_size_to_retrieve += file_size;
            *files_to_retrieve += 1;
            no_of_listed_files++;
         }
         else
         {
            return(1);
         }
      }
#ifdef DO_NOT_PARALLELIZE_ALL_FETCH
      if ((fra[db.fra_pos].stupid_mode == YES) ||
          (fra[db.fra_pos].remove == YES) ||
          ((*files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
           (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size)))
#else
      if ((*files_to_retrieve < fra[db.fra_pos].max_copied_files) &&
          (*file_size_to_retrieve < fra[db.fra_pos].max_copied_file_size))
#endif
      {
         rl[no_of_listed_files - 1].assigned = (unsigned char)db.job_no + 1;
      }
      else
      {
         rl[no_of_listed_files - 1].assigned = 0;
         *file_size_to_retrieve -= file_size;
         *files_to_retrieve -= 1;
         *more_files_in_list = YES;
      }
      *(int *)((char *)rl - AFD_WORD_OFFSET) = no_of_listed_files;
#ifdef DEBUG_ASSIGNMENT
      trans_log(DEBUG_SIGN, __FILE__, __LINE__, NULL, NULL,
# if SIZEOF_OFF_T == 4
                "%s assigned %d: file_name=%s assigned=%d size=%ld",
# else
                "%s assigned %d: file_name=%s assigned=%d size=%lld",
# endif
                (fra[db.fra_pos].ls_data_alias[0] == '\0') ? fra[db.fra_pos].dir_alias : fra[db.fra_pos].ls_data_alias,
                i, rl[i].file_name, (int)rl[i].assigned,
                (pri_off_t)rl[i].size);
#endif /* DEBUG_ASSIGNMENT */
      return(0);
   }
   else
   {
      return(1);
   }
}
