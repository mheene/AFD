/*
 *  sftpcmd.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 2005 - 2019 Holger Kiehl <Holger.Kiehl@dwd.de>
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
 **   sftpcmd - commands to send files via the SFTP protocol
 **
 ** SYNOPSIS
 **   int          sftp_cd(char *directory, int create_dir, mode_t dir_mode,
 **                        char *created_path)
 **   void         sftp_nocd(void)
 **   int          sftp_chmod(char *filename, mode_t mode)
 **   int          sftp_close_dir(void)
 **   int          sftp_close_file(void)
 **   int          sftp_connect(char *hostname, int port, char *user,
 **                             unsigned char ssh_protocol, int ssh_options,
 **                             char *fingerprint, char *passwd, char *dir,
 **                             char debug)
 **   int          sftp_dele(char *filename)
 **   int          sftp_flush(void)
 **   int          sftp_mkdir(char *directory, mode_t dir_mode)
 **   int          sftp_move(char *from, char *to, int create_dir,
 **                          mode_t dir_mode, char *created_path)
 **   int          sftp_noop(void)
 **   int          sftp_open_dir(char *dirname, char debug)
 **   int          sftp_open_file(char *filename, off_t size, mode_t *mode,
 **                               char debug)
 **   int          sftp_pwd(void)
 **   void         sftp_quit(void)
 **   int          sftp_read(char *block, int size)
 **   int          sftp_readdir(char *name, struct stat *p_stat_buf)
 **   int          sftp_set_file_time(char *filename, time_t mtime,
 **                                   time_t atime)
 **   int          sftp_stat(char *filename, struct stat *p_stat_buf)
 **   int          sftp_write(char *block, int size)
 **   unsigned int sftp_version(void)
 **
 ** DESCRIPTION
 **   scpcmd provides a set of commands to communicate SFTP with a
 **   SSH (Secure Shell) server via pipes. The following functions are
 **   required to send a file:
 **      sftp_close_dir()  - close a directory
 **      sftp_close_file() - close a file
 **      sftp_connect()    - build a connection to the SSH server
 **      sftp_dele()       - deletes a file/link
 **      sftp_flush()      - flush all pending writes
 **      sftp_mkdir()      - creates a directory
 **      sftp_move()       - move/rename a file
 **      sftp_open_dir()   - open a directory
 **      sftp_open_file()  - open a file
 **      sftp_quit()       - disconnect from the SSH server
 **      sftp_read()       - read data from a file
 **      sftp_readdir()    - read a directory entry
 **      sftp_write()      - write data to a file
 **      sftp_version()    - returns SSH version agreed on
 **
 ** RETURN VALUES
 **   Returns SUCCESS when successful. When an error has occurred
 **   it will return INCORRECT. 'timeout_flag' is just a flag to
 **   indicate that the 'transfer_timeout' time has been reached.
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   17.12.2005 H.Kiehl Created.
 **   26.04.2006 H.Kiehl Support for scanning directories.
 **   07.03.2013 H.Kiehl Added function sftp_chmod().
 **   11.09.2014 H.Kiehl Added simulation mode.
 **   16.04.2015 H.Kiehl Added function sftp_nocd().
 **   23.04.2018 H.Kiehl Show better error message when remote server just
 **                      closes connection.
 **
 */
DESCR__E_M3


#include <stdio.h>            /* fdopen(), fclose()                      */
#include <string.h>           /* memset(), memcpy()                      */
#include <stdlib.h>           /* malloc(), free(), exit()                */
#ifdef WITH_OWNER_GROUP_EVAL
# include <grp.h>             /* getgrnam()                              */
# include <pwd.h>             /* getpwnam()                              */
#endif
#include <time.h>             /* gmtime(), strftime()                    */
#include <sys/types.h>        /* fd_set                                  */
#include <sys/time.h>         /* struct timeval                          */
#include <sys/wait.h>         /* waitpid()                               */
#include <sys/stat.h>         /* S_ISUID, S_ISGID, etc                   */
#include <setjmp.h>           /* sigsetjmp(), siglongjmp()               */
#include <signal.h>           /* signal(), kill()                        */
#include <unistd.h>           /* select(), write(), read(), close()      */
#include <fcntl.h>            /* O_NONBLOCK                              */
#include <errno.h>
#include "ssh_commondefs.h"
#include "sftpdefs.h"
#include "fddefs.h"           /* trans_log()                             */

#define DIR_NOT_EXIST_WORKAROUND 1


#define SFTP_CD_TRY_CREATE_DIR()                                          \
        {                                                                 \
           if ((create_dir == YES) && (retries == 0) &&                   \
               (get_xfer_uint(&msg[5]) == SSH_FX_NO_SUCH_FILE))           \
           {                                                              \
              char *p_start,                                              \
                   *ptr,                                                  \
                   tmp_char;                                              \
                                                                          \
              ptr = directory;                                            \
              do                                                          \
              {                                                           \
                 while (*ptr == '/')                                      \
                 {                                                        \
                    ptr++;                                                \
                 }                                                        \
                 p_start = ptr;                                           \
                 while ((*ptr != '/') && (*ptr != '\0'))                  \
                 {                                                        \
                    ptr++;                                                \
                 }                                                        \
                 if ((*ptr == '/') || (*ptr == '\0'))                     \
                 {                                                        \
                    tmp_char = *ptr;                                      \
                    *ptr = '\0';                                          \
                    if ((status = sftp_stat(directory, NULL)) != SUCCESS) \
                    {                                                     \
                       status = sftp_mkdir(directory, dir_mode);          \
                       if (status == SUCCESS)                             \
                       {                                                  \
                          if (created_path != NULL)                       \
                          {                                               \
                              if (created_path[0] != '\0')                \
                              {                                           \
                                 (void)strcat(created_path, "/");         \
                              }                                           \
                              (void)strcat(created_path, p_start);        \
                          }                                               \
                       }                                                  \
                    }                                                     \
                    else if (scd.version > 3)                             \
                         {                                                \
                            if (scd.stat_buf.st_mode != S_IFDIR)          \
                            {                                             \
                               status = INCORRECT;                        \
                            }                                             \
                         }                                                \
                    *ptr = tmp_char;                                      \
                 }                                                        \
              } while ((*ptr != '\0') && (status == SUCCESS));            \
                                                                          \
              if ((*ptr == '\0') && (status == SUCCESS))                  \
              {                                                           \
                 retries++;                                               \
                 goto retry_cd;                                           \
              }                                                           \
           }                                                              \
           else                                                           \
           {                                                              \
              /* Some error has occured. */                               \
              get_msg_str(&msg[9]);                                       \
              trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_cd", NULL,  \
                        "%s", error_2_str(&msg[5]));                      \
              status = INCORRECT;                                         \
           }                                                              \
        }


/* External global variables. */
extern int                      simulation_mode,
                                timeout_flag;
extern char                     msg_str[];
extern long                     transfer_timeout;
extern pid_t                    data_pid;
extern char                     tr_hostname[];

/* Local global variables. */
static int                      byte_order = 1,
                                data_fd = -1;
static char                     *msg = NULL;
static sigjmp_buf               env_alrm;
static struct sftp_connect_data scd;

/* Local function prototypes. */
static unsigned int             get_xfer_uint(char *);
static u_long_64                get_xfer_uint64(char *);
static int                      check_msg_pending(void),
                                get_reply(unsigned int, int),
                                get_write_reply(unsigned int, int),
                                get_xfer_names(unsigned int, char *),
                                get_xfer_str(char *, char **),
                                is_with_path(char *),
                                read_msg(char *, int, int),
                                store_attributes(char *, unsigned int *,
                                                 struct stat *),
                                write_msg(char *, int, int);
static char                     *error_2_str(char *),
                                *response_2_str(char);
static void                     get_msg_str(char *),
                                set_xfer_str(char *, char *, int),
                                set_xfer_uint(char *, unsigned int),
                                set_xfer_uint64(char *, u_long_64),
                                sig_handler(int);
#ifdef WITH_TRACE
static void                     show_sftp_cmd(unsigned int, int),
                                show_trace_handle(char *, unsigned int,
                                                  char *, char *,
                                                  unsigned int, off_t,
                                                  int, char *, int, int);
static char                     mode2type(unsigned);
#endif


/*########################### sftp_connect() ############################*/
int
sftp_connect(char          *hostname,
             int           port,
             unsigned char ssh_protocol,
             int           ssh_options,
             char          *user,
#ifdef WITH_SSH_FINGERPRINT
             char          *fingerprint,
#endif
             char          *passwd,
             char          debug)
{
   int retries = 0,
       status;

retry_connect:
   if (simulation_mode == YES)
   {
      if ((data_fd = open("/dev/null", O_RDWR)) == -1)
      {
         system_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_connect", "Simulated sftp_connect()",
                    _("Failed to open() /dev/null : %s"), strerror(errno));
         return(INCORRECT);
      }
      else
      {
#ifdef WITH_TRACE
         int length;

         length = snprintf(msg_str, MAX_RET_MSG_LENGTH, _("Simulated SFTP connect to %s (port=%d)"), hostname, port);
         trace_log(NULL, 0, C_TRACE, msg_str, length, NULL);
#else
         (void)snprintf(msg_str, MAX_RET_MSG_LENGTH, _("Simulated SFTP connect to %s (port=%d)"), hostname, port);
#endif
      }
      if (msg == NULL)
      {
         if ((msg = malloc(MAX_SFTP_MSG_LENGTH)) == NULL)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
                      _("malloc() error : %s"), strerror(errno));
            return(INCORRECT);
         }
      }
      scd.version        = 3; /* OpenSSH */
      scd.posix_rename   = 1;
      scd.statvfs        = 2;
      scd.fstatvfs       = 2;
      scd.hardlink       = 1;
      scd.fsync          = 1;
      scd.request_id     = 0;
      scd.stored_replies = 0;
      scd.cwd            = NULL;
      scd.file_handle    = NULL;
      scd.dir_handle     = NULL;

      return(SUCCESS);
   }

   if ((status = ssh_exec(hostname, port, ssh_protocol, ssh_options, user,
                          passwd, NULL, "sftp", &data_fd)) == SUCCESS)
   {
      unsigned int ui_var = 5;

      if (debug > 0)
      {
         trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
# if SIZEOF_PID_T == 4
                   "Started SSH client with pid %d.",
# else
                   "Started SSH client with pid %lld.",
# endif
                   (pri_pid_t)data_pid);
      }

      if (msg == NULL)
      {
         if ((msg = malloc(MAX_SFTP_MSG_LENGTH)) == NULL)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
                      _("malloc() error : %s"), strerror(errno));
            return(INCORRECT);
         }
      }

      if (*(char *)&byte_order == 1)
      {
         /* little-endian */
         msg[0] = ((char *)&ui_var)[3];
         msg[1] = ((char *)&ui_var)[2];
         msg[2] = ((char *)&ui_var)[1];
         msg[3] = ((char *)&ui_var)[0];
         ui_var = SSH_FILEXFER_VERSION;
         msg[5] = ((char *)&ui_var)[3];
         msg[6] = ((char *)&ui_var)[2];
         msg[7] = ((char *)&ui_var)[1];
         msg[8] = ((char *)&ui_var)[0];
      }
      else
      {
         /* big-endian */
         msg[0] = ((char *)&ui_var)[0];
         msg[1] = ((char *)&ui_var)[1];
         msg[2] = ((char *)&ui_var)[2];
         msg[3] = ((char *)&ui_var)[3];
         ui_var = SSH_FILEXFER_VERSION;
         msg[5] = ((char *)&ui_var)[0];
         msg[6] = ((char *)&ui_var)[1];
         msg[7] = ((char *)&ui_var)[2];
         msg[8] = ((char *)&ui_var)[3];
      }
      msg[4] = SSH_FXP_INIT;
      scd.debug = debug;

      if ((status = write_msg(msg, 9, __LINE__)) == SUCCESS)
      {
         if (debug > 0)
         {
            trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
                      "Trying to login as %s.", user);
         }
#ifdef WITH_SSH_FINGERPRINT
         if ((status = ssh_login(data_fd, passwd, debug, fingerprint)) == SUCCESS)
#else
         if ((status = ssh_login(data_fd, passwd, debug)) == SUCCESS)
#endif
         {
            if ((status = read_msg(msg, 4, __LINE__)) == SUCCESS)
            {
               ui_var = get_xfer_uint(msg);
               if (ui_var <= MAX_SFTP_MSG_LENGTH)
               {
                  if ((status = read_msg(msg, (int)ui_var, __LINE__)) == SUCCESS)
                  {
#ifdef WITH_TRACE
                     if (scd.debug == TRACE_MODE)
                     {
                        show_sftp_cmd(ui_var, R_TRACE);
                     }
#endif
                     if (msg[0] == SSH_FXP_VERSION)
                     {
                        char *ptr,
                             *p_xfer_str = NULL;

                        scd.version = get_xfer_uint(&msg[1]);
                        if (scd.version > SSH_FILEXFER_VERSION)
                        {
                           trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
                                     _("Server version (%u) is higher, downgrading to version we can handle (%d)."),
                                     scd.version, SSH_FILEXFER_VERSION);
                           scd.version = SSH_FILEXFER_VERSION;
                        }
                        ui_var -= 5;
                        scd.posix_rename = 0;
                        scd.statvfs = 0;
                        scd.fstatvfs = 0;
                        scd.hardlink = 0;
                        scd.fsync = 0;
                        if (ui_var > 0)
                        {
                           int str_len;

                           /*
                            * Check for any extensions from the server side.
                            */
                           ptr = &msg[5];
                           while (ui_var > 0)
                           {
                              if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                                  (str_len > ui_var))
                              {
                                 break;
                              }
                              else
                              {
                                 if (my_strcmp(p_xfer_str,
                                               OPENSSH_POSIX_RENAME_EXT) == 0)
                                 {
                                    ui_var -= (str_len + 4);
                                    ptr += (str_len + 4);
                                    free(p_xfer_str);
                                    p_xfer_str = NULL;
                                    if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                                        (str_len > ui_var))
                                    {
                                       break;
                                    }
                                    else
                                    {
                                       scd.posix_rename = atoi(p_xfer_str);
                                    }
                                 }
                                 else if (my_strcmp(p_xfer_str,
                                                    OPENSSH_STATFS_EXT) == 0)
                                      {
                                         ui_var -= (str_len + 4);
                                         ptr += (str_len + 4);
                                         free(p_xfer_str);
                                         p_xfer_str = NULL;
                                         if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                                             (str_len > ui_var))
                                         {
                                            break;
                                         }
                                         else
                                         {
                                            scd.statvfs = atoi(p_xfer_str);
                                         }
                                      }
                                 else if (my_strcmp(p_xfer_str,
                                                    OPENSSH_FSTATFS_EXT) == 0)
                                      {
                                         ui_var -= (str_len + 4);
                                         ptr += (str_len + 4);
                                         free(p_xfer_str);
                                         p_xfer_str = NULL;
                                         if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                                             (str_len > ui_var))
                                         {
                                            break;
                                         }
                                         else
                                         {
                                            scd.fstatvfs = atoi(p_xfer_str);
                                         }
                                      }
                                 else if (my_strcmp(p_xfer_str,
                                                    OPENSSH_HARDLINK_EXT) == 0)
                                      {
                                         ui_var -= (str_len + 4);
                                         ptr += (str_len + 4);
                                         free(p_xfer_str);
                                         p_xfer_str = NULL;
                                         if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                                             (str_len > ui_var))
                                         {
                                            break;
                                         }
                                         else
                                         {
                                            scd.hardlink = atoi(p_xfer_str);
                                         }
                                      }
                                 else if (my_strcmp(p_xfer_str,
                                                    OPENSSH_FSYNC_EXT) == 0)
                                      {
                                         ui_var -= (str_len + 4);
                                         ptr += (str_len + 4);
                                         free(p_xfer_str);
                                         p_xfer_str = NULL;
                                         if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                                             (str_len > ui_var))
                                         {
                                            break;
                                         }
                                         else
                                         {
                                            scd.fsync = atoi(p_xfer_str);
                                         }
                                      }
                                      else
                                      {
                                         /* Away with the unknown extension. */
                                         ui_var -= (str_len + 4);
                                         ptr += (str_len + 4);
                                         free(p_xfer_str);
                                         p_xfer_str = NULL;
                                         if ((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0)
                                         {
                                            break;
                                         }
                                      }

                                 /* Away with the version number. */
                                 ui_var -= (str_len + 4);
                                 ptr += (str_len + 4);
                                 free(p_xfer_str);
                                 p_xfer_str = NULL;
                              }
                           }
                        }
                        scd.request_id     = 0;
                        scd.stored_replies = 0;
                        scd.cwd            = NULL;
                        scd.file_handle    = NULL;
                        scd.dir_handle     = NULL;
                        if (p_xfer_str != NULL)
                        {
                           free(p_xfer_str);
                        }
                     }
                     else
                     {
                        if (msg[0] == SSH_FXP_STATUS)
                        {
                           /* Some error has occured. */
                           get_msg_str(&msg[9]);
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_connect", error_2_str(&msg[5]),
                                     _("Received invalid reply (%d = %s) from SSH_FXP_INIT."),
                                     (int)msg[0], response_2_str(msg[0]));
                        }
                        else
                        {
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
                                     _("Received invalid reply (%d = %s) from SSH_FXP_INIT."),
                                     (int)msg[0], response_2_str(msg[0]));
                        }
                        status = INCORRECT;
                     }
                  }
               }
               else
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_connect", NULL,
                            _("Received message is %u bytes, can only handle %d bytes."),
                            ui_var, MAX_SFTP_MSG_LENGTH);
                  status = INCORRECT;
                  sftp_quit();
               }
            }
         }
         else if (status == RETRY)
              {
                 retries++;
                 sftp_quit();
                 if (retries < 5)
                 {
                    goto retry_connect;
                 }
              }
      }
   }

   return(status);
}


/*########################### sftp_version() ############################*/
unsigned int
sftp_version(void)
{
   return(scd.version);
}


/*############################# sftp_pwd() ##############################*/
int
sftp_pwd(void)
{
   int status;

   /*
    * byte   SSH_FXP_REALPATH
    * uint32 request-id
    * string original-path [UTF-8]
    * byte   control-byte [optional]
    * string compose-path[0..n] [optional]
    */
   msg[4] = SSH_FXP_REALPATH;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   set_xfer_str(&msg[4 + 1 + 4], ".", 1);
   set_xfer_uint(msg, (1 + 4 + 4 + 1)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      int length;

      length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                        "sftp_pwd(): request-id=%d SSH_FXP_REALPATH path=.",
                        scd.request_id);
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, 14, __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_NAME)
         {
            unsigned int ui_var;

            ui_var = get_xfer_uint(&msg[5]);

            /* We can only handle one name. */
            if (ui_var == 1)
            {
               if (scd.cwd != NULL)
               {
                  free(scd.cwd);
                  scd.cwd = NULL;
               }

               if (get_xfer_str(&msg[9], &scd.cwd) == 0)
               {
                  status = INCORRECT;
               }
               else
               {
                  (void)strcpy(msg_str, scd.cwd);
               }
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_pwd", NULL,
                         _("Expecting a one here, but received %d. We are only able to handle one name."),
                         ui_var);
               status = INCORRECT;
            }
         }
         else
         {
            if (msg[0] == SSH_FXP_STATUS)
            {
               /* Some error has occured. */
               get_msg_str(&msg[9]);
               trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_pwd", NULL,
                         "%s", error_2_str(&msg[5]));
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_pwd", NULL,
                         _("Expecting %d (SSH_FXP_NAME) but got %d (%s) as reply."),
                         SSH_FXP_NAME, (int)msg[0], response_2_str(msg[0]));
               msg_str[0] = '\0';
            }
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              (void)strcpy(msg_str, "/simulated/pwd");
              status = SUCCESS;
           }
   }

   return(status);
}


/*############################# sftp_cd() ###############################*/
int
sftp_cd(char *directory, int create_dir, mode_t dir_mode, char *created_path)
{
   int retries = 0,
       status;

retry_cd:
   if ((directory[0] == '\0') || (scd.cwd != NULL))
   {
      /* Go back to users home directory. */
      free(scd.cwd);
      scd.cwd = NULL;
      if (directory[0] == '\0')
      {
         return(SUCCESS);
      }
   }

   /*
    * byte   SSH_FXP_REALPATH
    * uint32 request-id
    * string original-path [UTF-8]
    * byte   control-byte [optional]
    * string compose-path[0..n] [optional]
    */
   msg[4] = SSH_FXP_REALPATH;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   status = strlen(directory);
   set_xfer_str(&msg[4 + 1 + 4], directory, status);
   set_xfer_uint(msg, (1 + 4 + 4 + status)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      int length;

      length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                        "sftp_cd(): request-id=%d SSH_FXP_REALPATH path=%s",
                        scd.request_id, directory);
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + status), __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_NAME)
         {
            unsigned int ui_var;

            ui_var = get_xfer_uint(&msg[5]);

            /* We can only handle one name. */
            if (ui_var == 1)
            {
               if (scd.cwd != NULL)
               {
                  free(scd.cwd);
                  scd.cwd = NULL;
               }
               if (get_xfer_str(&msg[9], &scd.cwd) == 0)
               {
                  msg_str[0] = '\0';
                  status = INCORRECT;
               }
#ifdef DIR_NOT_EXIST_WORKAROUND
               else
               {
                  /*
                   * Some older versions of openssh have the bug
                   * that they return the directory name even if that
                   * directory does not exist. So we must do a sftp_stat()
                   * to make sure the directory does exist.
                   */
                  if (scd.version < 4)
                  {
                     char *tmp_cwd = scd.cwd;

                     scd.cwd = NULL;
                     if (sftp_stat(tmp_cwd, NULL) == INCORRECT)
                     {
                        SFTP_CD_TRY_CREATE_DIR();
                        free(scd.cwd);
                     }
                     scd.cwd = tmp_cwd;
                  }
               }
#endif /* DIR_NOT_EXIST_WORKAROUND */
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_cd", NULL,
                         _("Expecting a one here, but received %d. We are only able to handle one name."),
                         ui_var);
               msg_str[0] = '\0';
               status = INCORRECT;
            }
         }
         else
         {
            if (msg[0] == SSH_FXP_STATUS)
            {
#ifdef MACRO_DEBUG
               if ((create_dir == YES) && (retries == 0) &&
                   (get_xfer_uint(&msg[5]) == SSH_FX_NO_SUCH_FILE))
               {
                  char *p_start,
                       *ptr,
                       tmp_char;

                  ptr = directory;
                  do
                  {
                     while (*ptr == '/')
                     {
                        ptr++;
                     }
                     p_start = ptr;
                     while ((*ptr != '/') && (*ptr != '\0'))
                     {
                        ptr++;
                     }
                     if ((*ptr == '/') || (*ptr == '\0'))
                     {
                        tmp_char = *ptr;
                        *ptr = '\0';
                        if ((status = sftp_stat(directory, NULL)) != SUCCESS)
                        {
                           status = sftp_mkdir(directory, dir_mode);
                           if (status == SUCCESS)
                           {
                              if (created_path != NULL)
                              {
                                  if (created_path[0] != '\0')
                                  {
                                     (void)strcat(created_path, "/");
                                  }
                                  (void)strcat(created_path, p_start);
                              }
                           }
                        }
                        else if (scd.version > 3)
                             {
                                if (scd.stat_buf.st_mode != S_IFDIR)
                                {
                                   status = INCORRECT;
                                }
                             }
                        *ptr = tmp_char;
                     }
                  } while ((*ptr != '\0') && (status == SUCCESS));

                  if ((*ptr == '\0') && (status == SUCCESS))
                  {
                     retries++;
                     goto retry_cd;
                  }
               }
               else
               {
                  /* Some error has occured. */
                  get_msg_str(&msg[9]);
                  trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_cd", NULL,
                            "%s", error_2_str(&msg[5]));
                  status = INCORRECT;
               }
#else
               SFTP_CD_TRY_CREATE_DIR();
#endif
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_cd", NULL,
                         _("Expecting %d (SSH_FXP_NAME) but got %d (%s) as reply."),
                         SSH_FXP_NAME, (int)msg[0], response_2_str(msg[0]));
               msg_str[0] = '\0';
               status = INCORRECT;
            }
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*############################ sftp_stat() ##############################*/
int
sftp_stat(char *filename, struct stat *p_stat_buf)
{
   int pos,
       status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   if ((filename == NULL) && (scd.file_handle == NULL))
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_stat", NULL,
                _("Wrong usage of function. filename and scd.file_handle are both NULL! Remove the programmer."));
      msg_str[0] = '\0';
      return(INCORRECT);
   }

   /*
    * byte   SSH_FXP_STAT | SSH_FXP_FSTAT
    * uint32 request_id
    * string path [UTF-8] | handle
    * [uint32 flags]  Version 6+
    */
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (filename == NULL)
   {
      msg[4] = SSH_FXP_FSTAT;
      status = (int)scd.file_handle_length;
      set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         if (scd.file_handle_length == 4)
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_stat(): request-id=%d SSH_FXP_FSTAT file_handle_length=%u file_handle=%u",
                              scd.request_id, (int)scd.file_handle_length,
                              get_xfer_uint(scd.file_handle));
         }
         else if (scd.file_handle_length == 8)
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# ifdef HAVE_LONG_LONG
                                   "sftp_stat(): request-id=%d SSH_FXP_FSTAT file_handle_length=%u file_handle=%llu",
# else
                                   "sftp_stat(): request-id=%d SSH_FXP_FSTAT file_handle_length=%u file_handle=%lu",
# endif
                                   scd.request_id, (int)scd.file_handle_length,
                                   get_xfer_uint64(scd.file_handle));
              }
              else
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                                   "sftp_stat(): request-id=%d SSH_FXP_FSTAT file_handle_length=%u file_handle=<?>",
                                   scd.request_id, (int)scd.file_handle_length);
              }
      }
#endif
   }
   else
   {
      msg[4] = SSH_FXP_STAT;
      if ((scd.cwd == NULL) || (filename[0] == '/'))
      {
         status = strlen(filename);
         set_xfer_str(&msg[4 + 1 + 4], filename, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_stat(): request-id=%d SSH_FXP_STAT file_name=%s name_length=%d",
                              scd.request_id, filename, status);
         }
#endif
      }
      else if ((scd.cwd != NULL) &&
               (filename[0] == '.') && (filename[1] == '\0'))
           {
              status = strlen(scd.cwd);
              set_xfer_str(&msg[4 + 1 + 4], scd.cwd, status);
#ifdef WITH_TRACE
              if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                                   "sftp_stat(): request-id=%d SSH_FXP_STAT file_name=%s name_length=%d",
                                   scd.request_id, scd.cwd, status);
              }
#endif
           }
           else
           {
              char fullname[MAX_PATH_LENGTH];

              status = snprintf(fullname, MAX_PATH_LENGTH,
                                "%s/%s", scd.cwd, filename);
              set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
              if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                                   "sftp_stat(): request-id=%d SSH_FXP_STAT full_file_name=%s name_length=%d",
                                   scd.request_id, fullname, status);
              }
#endif
           }
   }
   pos = 4 + 1 + 4 + 4 + status;
   if (scd.version > 4)
   {
      set_xfer_uint(&msg[pos],
                    (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_MODIFYTIME));
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " attributes=%d (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_MODIFYTIME)",
                            SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_MODIFYTIME);
      }
#endif
   }
   else
   {
      set_xfer_uint(&msg[pos],
                    (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_ACMODTIME));
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " attributes=%d (SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_ACMODTIME)",
                            SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_ACMODTIME);
      }
#endif
   }
   pos += 4;
   set_xfer_uint(msg, (pos - 4)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, pos, __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_ATTRS)
         {
            (void)store_attributes(&msg[5], &scd.stat_flag, &scd.stat_buf);

            if (p_stat_buf != NULL)
            {
               (void)memcpy(p_stat_buf, &scd.stat_buf, sizeof(struct stat));
            }
         }
         else if (msg[0] == SSH_FXP_STATUS)
              {
                 /* Some error has occured. */
                 get_msg_str(&msg[9]);
                 trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_stat", NULL,
                           "%s", error_2_str(&msg[5]));
                 status = INCORRECT;
              }
              else
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_stat", NULL,
                           _("Expecting %d (SSH_FXP_HANDLE) but got %d (%s) as reply."),
                           SSH_FXP_HANDLE, (int)msg[0], response_2_str(msg[0]));
                 msg_str[0] = '\0';
                 status = INCORRECT;
              }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*####################### sftp_set_file_time() ##########################*/
/*                        --------------------                           */
/* This function is completely untested, since there does not seem to be */
/* an SSH server around that supports this.                              */
/*#######################################################################*/
int
sftp_set_file_time(char *filename, time_t mtime, time_t atime)
{
   int pos,
       status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   if ((filename == NULL) && (scd.file_handle == NULL))
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_set_file_time", NULL,
                _("Wrong usage of function. filename and scd.file_handle are both NULL! Remove the programmer."));
      msg_str[0] = '\0';
      return(INCORRECT);
   }

   /*
    * byte   SSH_FXP_SETSTAT | SSH_FXP_FSETSTAT
    * uint32 request_id
    * string path [UTF-8] | handle
    * ATTRS  attrs
    */
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (filename == NULL)
   {
      msg[4] = SSH_FXP_FSETSTAT;
      status = (int)scd.file_handle_length;
      set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         if (scd.file_handle_length == 4)
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_set_file_time(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=%u",
                              scd.request_id, (int)scd.file_handle_length,
                              get_xfer_uint(scd.file_handle));
         }
         else if (scd.file_handle_length == 8)
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# ifdef HAVE_LONG_LONG
                                   "sftp_set_file_time(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=%llu",
# else
                                   "sftp_set_file_time(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=%lu",
# endif
                                   scd.request_id, (int)scd.file_handle_length,
                                   get_xfer_uint64(scd.file_handle));
              }
              else
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                                   "sftp_set_file_time(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=<?>",
                                   scd.request_id, (int)scd.file_handle_length);
              }
      }
#endif
   }
   else
   {
      msg[4] = SSH_FXP_SETSTAT;
      if (scd.cwd == NULL)
      {
         status = strlen(filename);
         set_xfer_str(&msg[4 + 1 + 4], filename, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_set_file_time(): request-id=%d SSH_FXP_SETSTAT file_name=%s name_length=%d",
                              scd.request_id, filename, status);
         }
#endif
      }
      else
      {
         char fullname[MAX_PATH_LENGTH];

         status = snprintf(fullname, MAX_PATH_LENGTH,
                           "%s/%s", scd.cwd, filename);
         set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_set_file_time(): request-id=%d SSH_FXP_SETSTAT full file_name=%s name_length=%d",
                              scd.request_id, fullname, status);
         }
#endif
      }
   }
   pos = 4 + 1 + 4 + 4 + status;
   if (scd.version < 4)
   {
      set_xfer_uint(&msg[pos], SSH_FILEXFER_ATTR_ACMODTIME);
      pos += 4;
      set_xfer_uint(&msg[pos], (unsigned int)mtime);
      pos += 4;
      set_xfer_uint(&msg[pos], (unsigned int)atime);
      pos += 4;
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " attributes=%d (SSH_FILEXFER_ATTR_ACMODTIME) mtime=%u atime=%u",
                            SSH_FILEXFER_ATTR_ACMODTIME,
                            (unsigned int)mtime, (unsigned int)atime);
      }
#endif
   }
   else
   {
      set_xfer_uint(&msg[pos],
                    (SSH_FILEXFER_ATTR_MODIFYTIME | SSH_FILEXFER_ATTR_ACCESSTIME));
      pos += 4;
      set_xfer_uint64(&msg[pos], (u_long_64)(mtime));
      pos += 8;
      set_xfer_uint64(&msg[pos], (u_long_64)(atime));
      pos += 8;
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
# ifdef HAVE_LONG_LONG
                            " attributes=%d (SSH_FILEXFER_ATTR_MODIFYTIME | SSH_FILEXFER_ATTR_ACCESSTIME) mtime=%llu atime=%llu",
# else
                            " attributes=%d (SSH_FILEXFER_ATTR_MODIFYTIME | SSH_FILEXFER_ATTR_ACCESSTIME) mtime=%lu atime=%lu",
# endif
                            SSH_FILEXFER_ATTR_MODIFYTIME | SSH_FILEXFER_ATTR_ACCESSTIME,
                            (u_long_64)mtime, (u_long_64)atime);
      }
#endif
   }
   set_xfer_uint(msg, (pos - 4)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, pos, __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_STATUS)
         {
            if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
            {
               /* Some error has occured. */
               get_msg_str(&msg[9]);
               trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_set_file_time", NULL,
                         "%s", error_2_str(&msg[5]));
            }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_set_file_time", NULL,
                      _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                      SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
            msg_str[0] = '\0';
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*########################## sftp_open_file() ###########################*/
int
sftp_open_file(int    openmode,
               char   *filename,
               off_t  offset,
               mode_t *mode,
               int    blocksize,
               int    *buffer_offset)
{
   int pos,
       status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   if (scd.file_handle != NULL)
   {
      free(scd.file_handle);
      scd.file_handle = NULL;
   }

   /*
    * byte   SSH_FXP_OPEN
    * uint32 request_id
    * string filename [UTF-8]
    * [uint32 desired-access] Version 6+
    * uint32 flags
    * ATTRS  attrs
    */
   msg[4] = SSH_FXP_OPEN;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (scd.cwd == NULL)
   {
      status = strlen(filename);
      set_xfer_str(&msg[4 + 1 + 4], filename, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_open_file(): request-id=%d SSH_FXP_OPEN file_name=%s name_length=%d",
                           scd.request_id, filename, status);
      }
#endif
   }
   else
   {
      char fullname[MAX_PATH_LENGTH];

      status = snprintf(fullname, MAX_PATH_LENGTH, "%s/%s", scd.cwd, filename);
      set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_open_file(): request-id=%d SSH_FXP_OPEN full_file_name=%s name_length=%d",
                           scd.request_id, fullname, status);
      }
#endif
   }
   if (openmode == SFTP_WRITE_FILE)
   {
      if (scd.version > 4)
      {
         set_xfer_uint(&msg[4 + 1 + 4 + 4 + status],
                       (offset == 0) ? ACE4_WRITE_DATA : ACE4_APPEND_DATA);
         set_xfer_uint(&msg[4 + 1 + 4 + 4 + status + 4],
                       (offset == 0) ? SSH_FXF_CREATE_TRUNCATE : SSH_FXF_OPEN_EXISTING);
         pos = 4 + 1 + 4 + 4 + status + 4 + 4;
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                               " ace_flags=%d (%s) flags=%d (%s)",
                               (offset == 0) ? ACE4_WRITE_DATA : ACE4_APPEND_DATA,
                               (offset == 0) ? "ACE4_WRITE_DATA" : "ACE4_APPEND_DATA",
                               (offset == 0) ? SSH_FXF_CREATE_TRUNCATE : SSH_FXF_OPEN_EXISTING,
                               (offset == 0) ? "SSH_FXF_CREATE_TRUNCATE" : "SSH_FXF_OPEN_EXISTING");
         }
#endif
      }
      else
      {
         set_xfer_uint(&msg[4 + 1 + 4 + 4 + status],
                       (SSH_FXF_WRITE | SSH_FXF_CREAT |
                       ((offset == 0) ? SSH_FXF_TRUNC : 0)));
         pos = 4 + 1 + 4 + 4 + status + 4;
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            if (offset == 0)
            {
               length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                  " flags=%d (SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC)",
                                  SSH_FXF_WRITE | SSH_FXF_CREAT | SSH_FXF_TRUNC);
            }
            else
            {
               length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                  " flags=%d (SSH_FXF_WRITE | SSH_FXF_CREAT)",
                                  SSH_FXF_WRITE | SSH_FXF_CREAT);
            }
         }
#endif
      }
      if (mode == NULL)
      {
         set_xfer_uint(&msg[pos], 0);
         pos += 4;
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                               " mode_type=0");
         }
#endif
         if (scd.version > 3)
         {
            msg[pos] = SSH_FILEXFER_TYPE_REGULAR;
            pos++;
#ifdef WITH_TRACE
            if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
            {
               length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                  " type=%d (SSH_FILEXFER_TYPE_REGULAR)",
                                  SSH_FILEXFER_TYPE_REGULAR);
            }
#endif
         }
      }
      else
      {
         set_xfer_uint(&msg[pos], SSH_FILEXFER_ATTR_PERMISSIONS);
         pos += 4;
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                               " mode_type=%d (SSH_FILEXFER_ATTR_PERMISSIONS)",
                               SSH_FILEXFER_ATTR_PERMISSIONS);
         }
#endif
         if (scd.version > 3)
         {
            msg[pos] = SSH_FILEXFER_TYPE_REGULAR;
            pos++;
#ifdef WITH_TRACE
            if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
            {
               length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                  " type=%d (SSH_FILEXFER_TYPE_REGULAR)",
                                  SSH_FILEXFER_TYPE_REGULAR);
            }
#endif
         }
         set_xfer_uint(&msg[pos], (unsigned int)(*mode));
         pos += 4;
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                               " mode=%u (%x)",
                               (unsigned int)(*mode), (unsigned int)(*mode));
         }
#endif
      }
   }
   else if (openmode == SFTP_READ_FILE)
        {
           if (scd.version > 4)
           {
              set_xfer_uint(&msg[4 + 1 + 4 + 4 + status], ACE4_READ_DATA);
              set_xfer_uint(&msg[4 + 1 + 4 + 4 + status + 4],
                            SSH_FXF_OPEN_EXISTING);
              pos = 4 + 1 + 4 + 4 + status + 4 + 4;
#ifdef WITH_TRACE
              if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
              {
                 length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                    " ace_flags=%d (ACE4_READ_DATA) flags=%d (SSH_FXF_OPEN_EXISTING)",
                                    ACE4_READ_DATA, SSH_FXF_OPEN_EXISTING);
              }
#endif
           }
           else
           {
              set_xfer_uint(&msg[4 + 1 + 4 + 4 + status], SSH_FXF_READ);
              pos = 4 + 1 + 4 + 4 + status + 4;
#ifdef WITH_TRACE
              if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
              {
                 length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                    " flags=%d (SSH_FXF_READ)", SSH_FXF_READ);
              }
#endif
           }
           set_xfer_uint(&msg[pos], 0);
           pos += 4;
           if (scd.version > 3)
           {
              msg[pos] = SSH_FILEXFER_TYPE_REGULAR;
              pos++;
#ifdef WITH_TRACE
              if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
              {
                 length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                                    " type=%d (SSH_FILEXFER_TYPE_REGULAR)",
                                    SSH_FILEXFER_TYPE_REGULAR);
              }
#endif
           }
        }
        else
        {
           trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_open_file", NULL,
                     _("Unknown open mode %d."), openmode);
           msg_str[0] = '\0';
           return(INCORRECT);
        }
   set_xfer_uint(msg, (unsigned int)(pos - 4)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, pos, __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_HANDLE)
         {
            if ((scd.file_handle_length = get_xfer_str(&msg[5],
                                                       &scd.file_handle)) == 0)
            {
               status = INCORRECT;
            }
            else
            {
               scd.file_offset = offset;
               if (openmode == SFTP_WRITE_FILE)
               {
                  scd.pending_write_counter = -1;
                  scd.max_pending_writes = MAX_PENDING_WRITE_BUFFER / blocksize;
                  if (scd.max_pending_writes > MAX_PENDING_WRITES)
                  {
                     scd.max_pending_writes = MAX_PENDING_WRITES;
                  }
               }
               else
               {
                  scd.max_pending_writes = 0;
               }
               *buffer_offset = 4 + 1 + 4 + 4 + scd.file_handle_length + 8 + 4;
            }
         }
         else if (msg[0] == SSH_FXP_STATUS)
              {
                 /* Some error has occured. */
                 get_msg_str(&msg[9]);
                 trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_open_file", NULL,
                           "%s", error_2_str(&msg[5]));
                 status = get_xfer_uint(&msg[5]);
              }
              else
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_open_file", NULL,
                           _("Expecting %d (SSH_FXP_HANDLE) but got %d (%s) as reply."),
                           SSH_FXP_HANDLE, (int)msg[0], response_2_str(msg[0]));
                 msg_str[0] = '\0';
                 status = INCORRECT;
              }
      }
      else if (status == SIMULATION)
           {
              scd.file_offset = offset;
              scd.file_handle = NULL;
              scd.file_handle_length = 0;
              if (openmode == SFTP_WRITE_FILE)
              {
                 scd.pending_write_counter = -1;
                 scd.max_pending_writes = MAX_PENDING_WRITE_BUFFER / blocksize;
                 if (scd.max_pending_writes > MAX_PENDING_WRITES)
                 {
                    scd.max_pending_writes = MAX_PENDING_WRITES;
                 }
              }
              else
              {
                 scd.max_pending_writes = 0;
              }
              status = SUCCESS;
           }
   }

   return(status);
}


/*########################## sftp_open_dir() ############################*/
int
sftp_open_dir(char *dirname)
{
   int status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   if (scd.dir_handle != NULL)
   {
      (void)sftp_close_dir();
   }

   /*
    * byte   SSH_FXP_OPENDIR
    * uint32 request_id
    * string path [UTF-8]
    */
   msg[4] = SSH_FXP_OPENDIR;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (scd.cwd == NULL)
   {
      if (dirname[0] == '\0')
      {
         status = 1;
         set_xfer_str(&msg[4 + 1 + 4], ".", status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_open_dir(): request-id=%d SSH_FXP_OPENDIR path=. path_length=1",
                              scd.request_id);
         }
#endif
      }
      else
      {
         status = strlen(dirname);
         set_xfer_str(&msg[4 + 1 + 4], dirname, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_open_dir(): request-id=%d SSH_FXP_OPENDIR path=%s path_length=%d",
                              scd.request_id, dirname, status);
         }
#endif
      }
   }
   else
   {
      if (dirname[0] == '\0')
      {
         status = strlen(scd.cwd);
         set_xfer_str(&msg[4 + 1 + 4], scd.cwd, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_open_dir(): request-id=%d SSH_FXP_OPENDIR path=%s path_length=%d",
                              scd.request_id, scd.cwd, status);
         }
#endif
      }
      else
      {
         char fullname[MAX_PATH_LENGTH];

         status = snprintf(fullname, MAX_PATH_LENGTH,
                           "%s/%s", scd.cwd, dirname);
         set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_open_dir(): request-id=%d SSH_FXP_OPENDIR path=%s path_length=%d",
                              scd.request_id, fullname, status);
         }
#endif
      }
   }
   set_xfer_uint(msg, (1 + 4 + 4 + status)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + status), __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_HANDLE)
         {
            if ((scd.dir_handle_length = get_xfer_str(&msg[5],
                                                      &scd.dir_handle)) == 0)
            {
               status = INCORRECT;
            }
            else
            {
               scd.nl = NULL;
            }
         }
         else if (msg[0] == SSH_FXP_STATUS)
              {
                 /* Some error has occured. */
                 get_msg_str(&msg[9]);
                 trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_open_dir", NULL,
                           "%s", error_2_str(&msg[5]));
                 status = INCORRECT;
              }
              else
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_open_dir", NULL,
                           _("Expecting %d (SSH_FXP_HANDLE) but got %d (%s) as reply."),
                           SSH_FXP_HANDLE, (int)msg[0], response_2_str(msg[0]));
                 msg_str[0] = '\0';
                 status = INCORRECT;
              }
      }
      else if (status == SIMULATION)
           {
              scd.nl = NULL;
              status = SUCCESS;
           }
   }

   return(status);
}


/*########################## sftp_close_file() ##########################*/
int
sftp_close_file(void)
{
   int status = SUCCESS;

   /*
    * Before doing a close, catch all pending writes.
    */
   if (scd.pending_write_counter > 0)
   {
      status = sftp_flush();
   }

   if (status == SUCCESS)
   {
      /*
       * byte   SSH_FXP_CLOSE
       * uint32 request_id
       * string handle
       */
      msg[4] = SSH_FXP_CLOSE;
      scd.request_id++;
      set_xfer_uint(&msg[4 + 1], scd.request_id);
      set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, scd.file_handle_length);
      set_xfer_uint(msg, (1 + 4 + 4 + scd.file_handle_length)); /* Write message length at start. */
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         show_trace_handle("sftp_close_file()", scd.request_id, "SSH_FXP_CLOSE",
                           scd.file_handle, scd.file_handle_length, 0, 0,
                           __FILE__, __LINE__, NO);
      }
#endif
      if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.file_handle_length), __LINE__)) == SUCCESS)
      {
         if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
         {
            if (msg[0] == SSH_FXP_STATUS)
            {
               if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
               {
                  /* Some error has occured. */
                  get_msg_str(&msg[9]);
                  trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_close_file", NULL,
                            "%s", error_2_str(&msg[5]));
                  status = INCORRECT;
               }
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_close_file", NULL,
                         _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                         SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
               msg_str[0] = '\0';
               status = INCORRECT;
            }
         }
         else if (status == SIMULATION)
              {
                 status = SUCCESS;
              }
      }
   }

   /*
    * Regardless if an error has occurred, we may not try to reuse
    * the handle.
    */
   free(scd.file_handle);
   scd.file_handle = NULL;

   return(status);
}


/*########################## sftp_close_dir() ###########################*/
int
sftp_close_dir(void)
{
   int status;

   /*
    * byte   SSH_FXP_CLOSE
    * uint32 request_id
    * string handle
    */
   msg[4] = SSH_FXP_CLOSE;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   set_xfer_str(&msg[4 + 1 + 4], scd.dir_handle, scd.dir_handle_length);
   set_xfer_uint(msg, (1 + 4 + 4 + scd.dir_handle_length)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      show_trace_handle("sftp_close_dir()", scd.request_id, "SSH_FXP_CLOSE",
                        scd.dir_handle, scd.dir_handle_length, 0, 0,
                        __FILE__, __LINE__, NO);
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.dir_handle_length), __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_STATUS)
         {
            if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
            {
               /* Some error has occured. */
               get_msg_str(&msg[9]);
               trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_close_dir", NULL,
                         "%s", error_2_str(&msg[5]));
               status = INCORRECT;
            }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_close_dir", NULL,
                      _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                      SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
            msg_str[0] = '\0';
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   /*
    * Regardless if an error has occurred, we may not try to reuse
    * the handle.
    */
   free(scd.dir_handle);
   scd.dir_handle = NULL;

   if (scd.nl != NULL)
   {
      int i;

      for (i = 0; i < scd.nl_length; i++)
      {
         free(scd.nl[i].name);
      }
      free(scd.nl);
      scd.nl = NULL;
   }

   return(status);
}


/*############################ sftp_mkdir() #############################*/
int
sftp_mkdir(char *directory, mode_t dir_mode)
{
   int attr_len,
       status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   /*
    * byte   SSH_FXP_MKDIR
    * uint32 request_id
    * string path [UTF-8]
    * ATTRS  attrs
    */
   msg[4] = SSH_FXP_MKDIR;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (scd.cwd == NULL)
   {
      status = strlen(directory);
      set_xfer_str(&msg[4 + 1 + 4], directory, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_mkdir(): request-id=%d SSH_FXP_MKDIR path=%s path_length=%d",
                           scd.request_id, directory, status);
      }
#endif
   }
   else
   {
      char fullname[MAX_PATH_LENGTH];

      status = snprintf(fullname, MAX_PATH_LENGTH, "%s/%s", scd.cwd, directory);
      set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_mkdir(): request-id=%d SSH_FXP_MKDIR full path=%s path_length=%d",
                           scd.request_id, fullname, status);
      }
#endif
   }
   if (dir_mode == 0)
   {
      set_xfer_uint(&msg[4 + 1 + 4 + 4 + status], 0);
      attr_len = 0;
   }
   else
   {
      set_xfer_uint(&msg[4 + 1 + 4 + 4 + status], SSH_FILEXFER_ATTR_PERMISSIONS);
      set_xfer_uint(&msg[4 + 1 + 4 + 4 + status + 4], (unsigned int)(dir_mode));
      attr_len = 4;
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " mode_type=%d (SSH_FILEXFER_ATTR_PERMISSIONS), mode=%d (%x)", 
                            SSH_FILEXFER_ATTR_PERMISSIONS,
                            (unsigned int)(dir_mode), (unsigned int)(dir_mode));
      }
#endif
   }
   set_xfer_uint(msg, (1 + 4 + 4 + status + 4 + attr_len)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + status + 4 + attr_len), __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_STATUS)
         {
            if (get_xfer_uint(&msg[5]) == SSH_FX_OK)
            {
               if (dir_mode != 0)
               {
                  if ((status = sftp_chmod(directory, dir_mode)) != SUCCESS)
                  {
                     trans_log(WARN_SIGN, __FILE__, __LINE__, "sftp_mkdir", NULL,
                               _("Failed to change mode of directory `%s' to %d (%d)"),
                               directory, dir_mode, status);

                     /* Lets not make this fatal and continue. */
                     status = SUCCESS;
                  }
               }
            }
            else
            {
               /* Some error has occured. */
               get_msg_str(&msg[9]);
               trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_mkdir", NULL,
                         "%s", error_2_str(&msg[5]));
               status = INCORRECT;
            }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_mkdir", NULL,
                      _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                      SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
            msg_str[0] = '\0';
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*############################ sftp_move() ##############################*/
int
sftp_move(char   *from,
          char   *to,
          int    create_dir,
          mode_t dir_mode,
          char   *created_path)
{
   int from_length,
       pos,
       retries = 0,
       status,
       to_length;
#ifdef WITH_TRACE
   int length = 0;
#endif

   /*
    * byte   SSH_FXP_RENAME
    * uint32 request_id
    * string oldpath [UTF-8]
    * string newpath [UTF-8]
    * [uint32 flags]  Version 6+
    */
retry_move:
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (scd.posix_rename > 0)
   {
      msg[4] = SSH_FXP_EXTENDED;
      set_xfer_str(&msg[4 + 1 + 4], OPENSSH_POSIX_RENAME_EXT,
                   OPENSSH_POSIX_RENAME_EXT_LENGTH);
      pos = 4 + 1 + 4 + 4 + OPENSSH_POSIX_RENAME_EXT_LENGTH;
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_move(): request-id=%d SSH_FXP_EXTENDED %s",
                           scd.request_id, OPENSSH_POSIX_RENAME_EXT);
      }
#endif
   }
   else
   {
      msg[4] = SSH_FXP_RENAME;
      pos = 4 + 1 + 4;
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_move(): request-id=%d SSH_FXP_RENAME",
                           scd.request_id);
      }
#endif
   }
   if (scd.cwd == NULL)
   {
      from_length = strlen(from);
      set_xfer_str(&msg[pos], from, from_length);
      to_length = strlen(to);
      set_xfer_str(&msg[pos + 4 + from_length], to, to_length);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " from=%s to=%s", from, to);
      }
#endif
   }
   else
   {
      char fullname[MAX_PATH_LENGTH];

      from_length = snprintf(fullname, MAX_PATH_LENGTH, "%s/%s", scd.cwd, from);
      set_xfer_str(&msg[pos], fullname, from_length);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " from=%s", fullname);
      }
#endif
      to_length = snprintf(fullname, MAX_PATH_LENGTH, "%s/%s", scd.cwd, to);
      set_xfer_str(&msg[pos + 4 + from_length], fullname, to_length);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                            " to=%s", fullname);
      }
#endif
   }
   pos += 4 + from_length + 4 + to_length;
   if (scd.version > 5)
   {
      set_xfer_uint(&msg[pos],
                    (SSH_FXF_RENAME_OVERWRITE | SSH_FXF_RENAME_ATOMIC));
      pos += 4;
   }
   set_xfer_uint(msg, (pos - 4)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, pos, __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_STATUS)
         {
            unsigned int ret_status;

            ret_status = get_xfer_uint(&msg[5]);
            if (ret_status != SSH_FX_OK)
            {
               /*
                * In version 3 the default behaviour is to fail
                * when we try to overwrite an existing file.
                * So we must delete it and then retry.
                */
               if ((((ret_status == SSH_FX_FAILURE) && (scd.version < 5)) ||
                    ((ret_status == SSH_FX_NO_SUCH_FILE) && (create_dir == YES) &&
                     (is_with_path(to) == YES))) &&
                   (retries == 0))
               {
                  if (ret_status == SSH_FX_NO_SUCH_FILE)
                  {
                     char full_to[MAX_PATH_LENGTH],
                          *ptr;

                     ptr = to + strlen(to) - 1;
                     while ((*ptr != '/') && (ptr != to))
                     {
                        ptr--;
                     }
                     if (*ptr == '/')
                     {
                        char *p_to,
                             *tmp_cwd = scd.cwd;

                        *ptr = '\0';
                        if (tmp_cwd == NULL)
                        {
                           p_to = to;
                        }
                        else
                        {
                           scd.cwd = NULL;
                           (void)snprintf(full_to, MAX_PATH_LENGTH,
                                          "%s/%s", tmp_cwd, to);
                           p_to = full_to;
                        }

                        /*
                         * NOTE: We do NOT want to go into this directory.
                         *       We just misuse sftp_cd() to create the
                         *       directory for us, nothing more.
                         */
                        if ((status = sftp_cd(p_to, YES, dir_mode,
                                              created_path)) == SUCCESS)
                        {
                           retries++;
                           *ptr = '/';
                           free(scd.cwd);
                           scd.cwd = tmp_cwd;
                           goto retry_move;
                        }
                     }
                     else
                     {
                        trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_move", NULL,
                                  _("Hmm, something wrong here bailing out."));
                        msg_str[0] = '\0';
                        status = INCORRECT;
                     }
                  }
                  else
                  {
                     /* Assuming file already exists, so delete it and retry. */
                     if ((status = sftp_dele(to)) == SUCCESS)
                     {
                        retries++;
                        goto retry_move;
                     }
                  }
               }
               else
               {
                  /* Some error has occured. */
                  get_msg_str(&msg[9]);
                  trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_move", NULL,
                            "%s", error_2_str(&msg[5]));
                  status = INCORRECT;
               }
            }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_move", NULL,
                      _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                      SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
            msg_str[0] = '\0';
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*############################# sftp_write() ############################*/
int                                                                        
sftp_write(char *block, int size)
{
   int status;

   /*
    * byte   SSH_FXP_WRITE
    * uint32 request_id
    * string handle
    * uint64 offset
    * string data
    */
   msg[4] = SSH_FXP_WRITE;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, scd.file_handle_length);
   set_xfer_uint64(&msg[4 + 1 + 4 + 4 + scd.file_handle_length], scd.file_offset);
   set_xfer_str(&msg[4 + 1 + 4 + 4 + scd.file_handle_length + 8], block, size);
   set_xfer_uint(msg, (1 + 4 + 4 + scd.file_handle_length + 8 + 4 + size)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      show_trace_handle("sftp_write()", scd.request_id,
                        "SSH_FXP_WRITE", scd.file_handle,
                        scd.file_handle_length, scd.file_offset, size,
                        __FILE__, __LINE__, YES);
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.file_handle_length + 8 + 4 + size), __LINE__)) == SUCCESS)
   {
      if ((scd.pending_write_counter != -1) &&
          (scd.pending_write_counter < scd.max_pending_writes))
      {
         scd.pending_write_id[scd.pending_write_counter] = scd.request_id;
         scd.pending_write_counter++;
         scd.file_offset += size;
      }
      else
      {
         if ((status = get_write_reply(scd.request_id, __LINE__)) == SUCCESS)
         {
            if (msg[0] == SSH_FXP_STATUS)
            {
               if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
               {
                  /* Some error has occured. */
                  get_msg_str(&msg[9]);
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_write", NULL,
                            "%s", error_2_str(&msg[5]));
                  status = INCORRECT;
               }
               else
               {
                  scd.file_offset += size;
               }
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_write", NULL,
                         _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                         SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
               msg_str[0] = '\0';
               status = INCORRECT;
            }
         }
         else if (status == SIMULATION)
              {
                 scd.file_offset += size;
                 status = SUCCESS;
              }
      }
   }

   return(status);
}


/*############################## sftp_read() ############################*/
int                                                                        
sftp_read(char *block, int size)
{
   int status;

   /*
    * byte   SSH_FXP_READ
    * uint32 request_id
    * string handle
    * uint64 offset
    * uint32 length
    */
   msg[4] = SSH_FXP_READ;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, scd.file_handle_length);
   set_xfer_uint64(&msg[4 + 1 + 4 + 4 + scd.file_handle_length], scd.file_offset);
   set_xfer_uint(&msg[4 + 1 + 4 + 4 + scd.file_handle_length + 8], (unsigned int)size);
   set_xfer_uint(msg, (1 + 4 + 4 + scd.file_handle_length + 8 + 4)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      show_trace_handle("sftp_read()", scd.request_id,
                        "SSH_FXP_READ", scd.file_handle,
                        scd.file_handle_length, scd.file_offset, size,
                        __FILE__, __LINE__, YES);
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.file_handle_length + 8 + 4), __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_DATA)
         {
            unsigned int ui_var;

            if (*(char *)&byte_order == 1)
            {
               /* little-endian */
               ((char *)&ui_var)[3] = msg[5];
               ((char *)&ui_var)[2] = msg[6];
               ((char *)&ui_var)[1] = msg[7];
               ((char *)&ui_var)[0] = msg[8];
            }
            else
            {
               /* big-endian */
               ((char *)&ui_var)[0] = msg[5];
               ((char *)&ui_var)[1] = msg[6];
               ((char *)&ui_var)[2] = msg[7];
               ((char *)&ui_var)[3] = msg[8];
            }
            if (ui_var == 0)
            {
               status = INCORRECT;
            }
            else
            {
               (void)memcpy(block, &msg[9], ui_var);
               scd.file_offset += ui_var;
               status = ui_var;
            }
         }
         else if (msg[0] == SSH_FXP_STATUS)
              {
                 if (get_xfer_uint(&msg[5]) == SSH_FX_EOF)
                 {
                    status = SFTP_EOF;
                 }
                 else
                 {
                    /* Some error has occured. */
                    get_msg_str(&msg[9]);
                    trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_read", NULL,
                              "%s", error_2_str(&msg[5]));
                    status = INCORRECT;
                 }
              }
              else
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_read", NULL,
                           _("Expecting %d (SSH_FXP_DATA) but got %d (%s) as reply."),
                           SSH_FXP_DATA, (int)msg[0], response_2_str(msg[0]));
                 msg_str[0] = '\0';
                 status = INCORRECT;
              }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*####################### sftp_multi_read_init() ########################*/
int                                                                        
sftp_multi_read_init(int blocksize, off_t expected_size)
{
   scd.reads_todo = expected_size / blocksize;
   if ((expected_size % blocksize) != 0)
   {
      scd.reads_todo++;
   }
   scd.reads_done = 0;
   scd.reads_queued = 0;
   scd.reads_low_water_mark = 0;
   scd.pending_id_read_pos = 0;
   scd.pending_id_end_pos = 0;
   scd.blocksize = blocksize;
   if (scd.reads_todo > MAX_PENDING_READS)
   {
      scd.max_pending_reads = MAX_PENDING_READS;
   }
   else
   {
      scd.max_pending_reads = scd.reads_todo;
   }
   if (scd.max_pending_reads > 1)
   {
      scd.current_max_pending_reads = SFTP_READ_STEP_SIZE;
   }
   else
   {
      scd.current_max_pending_reads = 0;
   }
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      int length;

      length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# if SIZEOF_OFF_T == 4
                        _("sftp_multi_read_init() max_pending_reads=%d current_max_pending_reads=%d expected_size=%ld bytes blocksize=%d reads_todo=%u"),
# else
                        _("sftp_multi_read_init() max_pending_reads=%d current_max_pending_reads=%d expected_size=%lld bytes blocksize=%d reads_todo=%u"),
# endif
                        scd.max_pending_reads, scd.current_max_pending_reads,
                        (pri_off_t)expected_size, blocksize, scd.reads_todo);
      trace_log(NULL, 0, C_TRACE, msg_str, length, NULL);
   }
#endif

   return(scd.current_max_pending_reads);
}


/*##################### sftp_multi_read_dispatch() ######################*/
int
sftp_multi_read_dispatch(void)
{
   int status = SUCCESS,
       todo = scd.reads_todo - (scd.reads_done + scd.reads_queued);

   if ((todo > scd.reads_queued) &&
       (scd.reads_queued <= scd.reads_low_water_mark) &&
       (scd.reads_queued < scd.current_max_pending_reads))
   {
      int i,
          rest;

      if (todo > (scd.current_max_pending_reads - scd.reads_queued))
      {
         todo = scd.current_max_pending_reads - scd.reads_queued;
      }

      if ((scd.pending_id_end_pos + todo) > MAX_PENDING_READS)
      {
         rest = (scd.pending_id_end_pos + todo) - MAX_PENDING_READS;
         todo = MAX_PENDING_READS;
      }
      else
      {
         todo = scd.pending_id_end_pos + todo;
         rest = 0;
      }
      for (i = scd.pending_id_end_pos; i < todo; i++)
      {
         /*
          * byte   SSH_FXP_READ
          * uint32 request_id
          * string handle
          * uint64 offset
          * uint32 length
          */
         msg[4] = SSH_FXP_READ;
         scd.request_id++;
         set_xfer_uint(&msg[4 + 1], scd.request_id);
         set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, scd.file_handle_length);
         set_xfer_uint64(&msg[4 + 1 + 4 + 4 + scd.file_handle_length],
                         scd.file_offset);
         set_xfer_uint(&msg[4 + 1 + 4 + 4 + scd.file_handle_length + 8],
                       scd.blocksize);
         set_xfer_uint(msg, (1 + 4 + 4 + scd.file_handle_length + 8 + 4)); /* Write message length at start. */
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            show_trace_handle("sftp_multi_read_dispatch()", scd.request_id,
                              "SSH_FXP_READ", scd.file_handle,
                              scd.file_handle_length, scd.file_offset,
                              scd.blocksize, __FILE__, __LINE__, YES);
         }
#endif
         if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.file_handle_length +
                                       8 + 4), __LINE__)) == SUCCESS)
         {
            scd.pending_read_id[i] = scd.request_id;
            scd.file_offset += scd.blocksize;
            scd.reads_queued++;
            scd.pending_id_end_pos++;
         }
         else
         {
            break;
         }
      }
      if (rest > 0)
      {
         scd.pending_id_end_pos = 0;
         for (i = 0; i < rest; i++)
         {
            /*
             * byte   SSH_FXP_READ
             * uint32 request_id
             * string handle
             * uint64 offset
             * uint32 length
             */
            msg[4] = SSH_FXP_READ;
            scd.request_id++;
            set_xfer_uint(&msg[4 + 1], scd.request_id);
            set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, scd.file_handle_length);
            set_xfer_uint64(&msg[4 + 1 + 4 + 4 + scd.file_handle_length],
                            scd.file_offset);
            set_xfer_uint(&msg[4 + 1 + 4 + 4 + scd.file_handle_length + 8],
                          scd.blocksize);
            set_xfer_uint(msg, (1 + 4 + 4 + scd.file_handle_length + 8 + 4)); /* Write message length at start. */
#ifdef WITH_TRACE
            if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
            {
               show_trace_handle("sftp_multi_read_dispatch()", scd.request_id,
                                 "SSH_FXP_READ", scd.file_handle,
                                 scd.file_handle_length, scd.file_offset,
                                 scd.blocksize, __FILE__, __LINE__, YES);
            }
#endif
            if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.file_handle_length +
                                          8 + 4), __LINE__)) == SUCCESS)
            {
               scd.pending_read_id[i] = scd.request_id;
               scd.file_offset += scd.blocksize;
               scd.reads_queued++;
               scd.pending_id_end_pos++;
            }
            else
            {
               break;
            }
         }
      }

      if (status == SUCCESS)
      {
         status = scd.reads_queued;
      }

#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         i = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           _("sftp_multi_read_dispatch() reads_queued=%d pending_id_read_pos=%d pending_id_end_pos=%d reads_low_water_mark=%d (todo=%d rest=%d)"),
                           scd.reads_queued, scd.pending_id_read_pos,
                           scd.pending_id_end_pos, scd.reads_low_water_mark,
                           todo, rest);
         trace_log(NULL, 0, C_TRACE, msg_str, i, NULL);
      }
#endif
   }

   return(status);
}


/*####################### sftp_multi_read_catch() #######################*/
int
sftp_multi_read_catch(char *buffer)
{
   int status;

   if ((status = get_reply(scd.pending_read_id[scd.pending_id_read_pos],
                           __LINE__)) == SUCCESS)
   {
      if (msg[0] == SSH_FXP_DATA)
      {
         unsigned int ui_var;

         if (*(char *)&byte_order == 1)
         {
            /* little-endian */
            ((char *)&ui_var)[3] = msg[5];
            ((char *)&ui_var)[2] = msg[6];
            ((char *)&ui_var)[1] = msg[7];
            ((char *)&ui_var)[0] = msg[8];
         }
         else
         {
            /* big-endian */
            ((char *)&ui_var)[0] = msg[5];
            ((char *)&ui_var)[1] = msg[6];
            ((char *)&ui_var)[2] = msg[7];
            ((char *)&ui_var)[3] = msg[8];
         }
         if ((ui_var > scd.blocksize) ||
             ((ui_var < scd.blocksize) &&
              (scd.reads_todo != (scd.reads_done + 1))))
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_multi_read_catch", NULL,
                      _("Expecting %d bytes, but received %u bytes. (reads_todo=%d reads_done=%d)"),
                      scd.blocksize, ui_var, scd.reads_todo, scd.reads_done);
            scd.file_offset -= scd.blocksize;
            status = SFTP_DO_SINGLE_READS;
         }
         else
         {
            (void)memcpy(buffer, &msg[9], ui_var);
            status = ui_var;
            if ((scd.reads_todo != (scd.reads_done + 1)) &&
                (scd.reads_queued == (scd.current_max_pending_reads - 1)))
            {
               if (scd.current_max_pending_reads < MAX_PENDING_READS)
               {
                  scd.current_max_pending_reads += SFTP_READ_STEP_SIZE;
                  if (scd.current_max_pending_reads > MAX_PENDING_READS)
                  {
                     scd.current_max_pending_reads = MAX_PENDING_READS;
                  }
                  scd.reads_low_water_mark = scd.current_max_pending_reads / 2;
               }
#ifdef WITH_TRACE
               if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
               {
                  int length;

                  length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                                    _("sftp_multi_read_catch() current_max_pending_reads=%d pending_id_read_pos=%d reads_low_water_mark=%d"),
                                    scd.current_max_pending_reads,
                                    scd.pending_id_read_pos,
                                    scd.reads_low_water_mark);
                  trace_log(NULL, 0, C_TRACE, msg_str, length, NULL);
               }
#endif
            }
         }
      }
      else if (msg[0] == SSH_FXP_STATUS)
           {
              if (get_xfer_uint(&msg[5]) != SSH_FX_EOF)
              {
                 status = SFTP_EOF;
              }
              else
              {
                 /* Some error has occured. */
                 get_msg_str(&msg[9]);
                 trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_multi_read_catch", NULL,
                           "%s", error_2_str(&msg[5]));
                 status = INCORRECT;
              }
           }
           else
           {
              trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_multi_read_catch", NULL,
                        _("Expecting %d (SSH_FXP_DATA) but got %d (%s) as reply."),
                        SSH_FXP_DATA, (int)msg[0], response_2_str(msg[0]));
              msg_str[0] = '\0';
              status = INCORRECT;
           }

      scd.pending_id_read_pos++;
      if (scd.pending_id_read_pos >= MAX_PENDING_READS)
      {
         scd.pending_id_read_pos = 0;
      }
      scd.reads_queued--;
      scd.reads_done++;
   }
   else if (status == SIMULATION)
        {
           status = SUCCESS;
           scd.pending_id_read_pos++;
           if (scd.pending_id_read_pos >= MAX_PENDING_READS)
           {
              scd.pending_id_read_pos = 0;
           }
           scd.reads_queued--;
           scd.reads_done++;
        }

#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      int length;

      length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                        _("sftp_multi_read_catch() reads_done=%u reads_todo=%u left=%d pending_id_read_pos=%d"),
                        scd.reads_done, scd.reads_todo,
                        (scd.reads_todo - scd.reads_done),
                        scd.pending_id_read_pos);
      trace_log(NULL, 0, C_TRACE, msg_str, length, NULL);
   }
#endif

   return(status);
}


/*######################### sftp_multi_read_eof() ##########################*/
int                                                                        
sftp_multi_read_eof(void)
{
   if (scd.reads_todo > scd.reads_done)
   {
      return(NO);
   }
   else
   {
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         int length;

         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           _("sftp_multi_read_eof() reads_done=%u reads_todo=%u left=%d pending_id_read_pos=%d"),
                           scd.reads_done, scd.reads_todo,
                           (scd.reads_todo - scd.reads_done),
                           scd.pending_id_read_pos);
         trace_log(NULL, 0, C_TRACE, msg_str, length, NULL);
      }
#endif
      return(YES);
   }
}


/*####################### sftp_multi_read_discard() ########################*/
void                                                                        
sftp_multi_read_discard(int report_pending_reads)
{
   if (scd.reads_queued != 0)
   {
      int i,
          rest,
          status = SUCCESS,
          todo;

      if (report_pending_reads == YES)
      {
         trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_multi_read_discard", NULL,
                   "Pending read counter is still %d!?",
                   scd.reads_queued);
      }

     if ((scd.pending_id_read_pos + scd.reads_queued) >= MAX_PENDING_READS)
     {
        todo = MAX_PENDING_READS;
        rest = scd.reads_queued - (MAX_PENDING_READS - scd.pending_id_read_pos);
     }
     else
     {
        todo = scd.pending_id_read_pos + scd.reads_queued;
        rest = 0;
     }
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         int length;

         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           _("sftp_multi_read_discard() discarding %d reads (todo=%d rest=%d)"),
                           scd.reads_queued, todo, rest);
         trace_log(NULL, 0, C_TRACE, msg_str, length, NULL);
      }
#endif

      /* Read all pending requests and ignore the data. */
      for (i = scd.pending_id_read_pos; i < todo; i++)
      {
         if (status == SUCCESS)
         {
            status = get_reply(scd.pending_read_id[i], __LINE__);
         }
         scd.file_offset -= scd.blocksize;
      }
      if (rest > 0)
      {
         for (i = 0; i < rest; i++)
         {
            if (status == SUCCESS)
            {
               status = get_reply(scd.pending_read_id[i], __LINE__);
            }
            scd.file_offset -= scd.blocksize;
         }
      }
      scd.reads_queued = 0;
   }

   return;
}


/*############################ sftp_readdir() ###########################*/
int
sftp_readdir(char *name, struct stat *p_stat_buf)
{
   if (scd.nl == NULL)
   {
      int status;

      /*
       * byte   SSH_FXP_READDIR
       * uint32 request_id
       * string handle
       */
      msg[4] = SSH_FXP_READDIR;
      scd.request_id++;
      set_xfer_uint(&msg[4 + 1], scd.request_id);
      set_xfer_str(&msg[4 + 1 + 4], scd.dir_handle, scd.dir_handle_length);
      set_xfer_uint(msg, (1 + 4 + 4 + scd.dir_handle_length)); /* Write message length at start. */
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         show_trace_handle("sftp_readdir()", scd.request_id, "SSH_FXP_READDIR",
                           scd.dir_handle, scd.dir_handle_length, 0, 0,
                           __FILE__, __LINE__, NO);
      }
#endif
      if ((status = write_msg(msg, (4 + 1 + 4 + 4 + scd.dir_handle_length), __LINE__)) == SUCCESS)
      {
         if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
         {
            if (msg[0] == SSH_FXP_NAME)
            {
               unsigned int ui_var;

               ui_var = get_xfer_uint(&msg[5]);

               status = get_xfer_names(ui_var, &msg[1 + 4 + 4]);
            }
            else
            {
               if (msg[0] == SSH_FXP_STATUS)
               {
                  if (get_xfer_uint(&msg[5]) == SSH_FX_EOF)
                  {
                     status = SSH_FX_EOF;
                  }
                  else
                  {
                     /* Some error has occured. */
                     get_msg_str(&msg[9]);
                     trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_readdir", NULL,
                               "%s", error_2_str(&msg[5]));
                     status = INCORRECT;
                  }
               }
               else
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_readdir", NULL,
                            _("Expecting %d (SSH_FXP_NAME) but got %d (%s) as reply."),
                            SSH_FXP_NAME, (int)msg[0], response_2_str(msg[0]));
                  msg_str[0] = '\0';
                  status = INCORRECT;
               }
            }
         }
         else if (status == SIMULATION)
              {
                 return(SUCCESS);
              }
      }
      if (status != SUCCESS)
      {
         return(status);
      }
   }
   (void)strcpy(name, scd.nl[scd.nl_pos].name);
   if (p_stat_buf != NULL)
   {
      (void)memcpy(p_stat_buf, &scd.nl[scd.nl_pos].stat_buf,
                   sizeof(struct stat));
   }
   scd.nl_pos++;
   if (scd.nl_pos >= scd.nl_length)
   {
      int i;

      for (i = 0; i < scd.nl_length; i++)
      {
         free(scd.nl[i].name);
      }
      free(scd.nl);
      scd.nl = NULL;
   }

   return(SUCCESS);
}


/*############################ sftp_flush() #############################*/
int
sftp_flush(void)
{
   if (scd.pending_write_counter > 0)
   {
      int i,
          status;

#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         trace_log(__FILE__, __LINE__, C_TRACE, NULL, 0,
                   "sftp_flush(): flush %d pending writes",
                   scd.pending_write_counter);
      }
#endif
      for (i = 0; i < scd.pending_write_counter; i++)
      {
         if ((status = get_reply(scd.pending_write_id[i], __LINE__)) == SUCCESS)
         {
            if (msg[0] == SSH_FXP_STATUS)
            {
               if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
               {
                  /* Some error has occured. */
                  get_msg_str(&msg[9]);
                  trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_flush", NULL,
                            "%s", error_2_str(&msg[5]));
                  return(INCORRECT);
               }
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_flush", NULL,
                         _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                         SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
               msg_str[0] = '\0';
               return(INCORRECT);
            }
         }
         else if (status == SIMULATION)
              {
                 scd.pending_write_counter = 0;
                 return(SUCCESS);
              }
              else
              {
                 return(INCORRECT);
              }
      }
      scd.pending_write_counter = 0;
   }

   return(SUCCESS);
}


/*############################# sftp_dele() #############################*/
int
sftp_dele(char *filename)
{
   int status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   /*
    * byte   SSH_FXP_REMOVE
    * uint32 request_id
    * string filename [UTF-8]
    */
   msg[4] = SSH_FXP_REMOVE;
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (scd.cwd == NULL)
   {
      status = strlen(filename);
      set_xfer_str(&msg[4 + 1 + 4], filename, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_dele(): request-id=%d SSH_FXP_REMOVE file_name=%s name_length=%d",
                           scd.request_id, filename, status);
      }
#endif
   }
   else
   {
      char fullname[MAX_PATH_LENGTH];

      status = snprintf(fullname, MAX_PATH_LENGTH, "%s/%s", scd.cwd, filename);
      set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "sftp_dele(): request-id=%d SSH_FXP_REMOVE full file_name=%s name_length=%d",
                           scd.request_id, fullname, status);
      }
#endif
   }
   set_xfer_uint(msg, (1 + 4 + 4 + status)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, (4 + 1 + 4 + 4 + status), __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_STATUS)
         {
            if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
            {
               /* Some error has occured. */
               get_msg_str(&msg[9]);
               trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_dele", NULL,
                         "%s", error_2_str(&msg[5]));
               status = INCORRECT;
            }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_dele", NULL,
                      _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                      SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
            msg_str[0] = '\0';
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*############################ sftp_chmod() #############################*/
int
sftp_chmod(char *filename, mode_t mode)
{
   int status;
#ifdef WITH_TRACE
   int length = 0;
#endif

   if ((filename == NULL) && (scd.file_handle == NULL))
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_chmod", NULL,
                _("Wrong usage of function. filename and scd.file_handle are both NULL! Remove the programmer."));
      msg_str[0] = '\0';
      return(INCORRECT);
   }

   /*
    * byte   SSH_FXP_SETSTAT | SSH_FXP_FSETSTAT
    * uint32 request_id
    * string path [UTF-8] | handle
    * ATTRS  attrs
    */
   scd.request_id++;
   set_xfer_uint(&msg[4 + 1], scd.request_id);
   if (filename == NULL)
   {
      msg[4] = SSH_FXP_FSETSTAT;
      status = (int)scd.file_handle_length;
      set_xfer_str(&msg[4 + 1 + 4], scd.file_handle, status);
#ifdef WITH_TRACE
      if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
      {
         if (scd.file_handle_length == 4)
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_chmod(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=%u",
                              scd.request_id, (int)scd.file_handle_length,
                              get_xfer_uint(scd.file_handle));
         }
         else if (scd.file_handle_length == 8)
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# ifdef HAVE_LONG_LONG
                                   "sftp_chmod(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=%llu",
# else
                                   "sftp_chmod(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=%lu",
# endif
                                   scd.request_id, (int)scd.file_handle_length,
                                   get_xfer_uint64(scd.file_handle));
              }
              else
              {
                 length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                                   "sftp_chmod(): request-id=%d SSH_FXP_FSETSTAT file_handle_length=%u file_handle=<?>",
                                   scd.request_id, (int)scd.file_handle_length);
              }
      }
#endif
   }
   else
   {
      msg[4] = SSH_FXP_SETSTAT;
      if (scd.cwd == NULL)
      {
         status = strlen(filename);
         set_xfer_str(&msg[4 + 1 + 4], filename, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_chmod(): request-id=%d SSH_FXP_SETSTAT file_name=%s name_length=%d",
                              scd.request_id, filename, status);
         }
#endif
      }
      else
      {
         char fullname[MAX_PATH_LENGTH];

         status = snprintf(fullname, MAX_PATH_LENGTH,
                           "%s/%s", scd.cwd, filename);
         set_xfer_str(&msg[4 + 1 + 4], fullname, status);
#ifdef WITH_TRACE
         if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
         {
            length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                              "sftp_chmod(): request-id=%d SSH_FXP_SETSTAT full file_name=%s name_length=%d",
                              scd.request_id, fullname, status);
         }
#endif
      }
   }
   set_xfer_uint(&msg[4 + 1 + 4 + 4 + status], SSH_FILEXFER_ATTR_PERMISSIONS);
   set_xfer_uint(&msg[4 + 1 + 4 + 4 + status + 4], (unsigned int)mode);
   set_xfer_uint(msg, (1 + 4 + 4 + status + 4 + 4)); /* Write message length at start. */
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
                         " mode=%o", (mode & ~S_IFMT));
      trace_log(__FILE__, __LINE__, C_TRACE, msg_str, length, NULL);
      msg_str[0] = '\0';
   }
#endif
   if ((status = write_msg(msg, 4 + 1 + 4 + 4 + status + 4 + 4, __LINE__)) == SUCCESS)
   {
      if ((status = get_reply(scd.request_id, __LINE__)) == SUCCESS)
      {
         if (msg[0] == SSH_FXP_STATUS)
         {
            if (get_xfer_uint(&msg[5]) != SSH_FX_OK)
            {
               /* Some error has occured. */
               get_msg_str(&msg[9]);
               trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_chmod", NULL,
                         "%s", error_2_str(&msg[5]));
            }
         }
         else
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_chmod", NULL,
                      _("Expecting %d (SSH_FXP_STATUS) but got %d (%s) as reply."),
                      SSH_FXP_STATUS, (int)msg[0], response_2_str(msg[0]));
            msg_str[0] = '\0';
            status = INCORRECT;
         }
      }
      else if (status == SIMULATION)
           {
              status = SUCCESS;
           }
   }

   return(status);
}


/*############################# sftp_noop() #############################*/
int
sftp_noop(void)
{
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, NULL, 0,
                "sftp_noop(): Calling sftp_stat(\".\", NULL)");
   }
#endif

   if (ssh_child_up() == NO)
   {
      return(INCORRECT);
   }

   /* I do not know of a better way. SFTP does not support */
   /* a NOOP command, so lets just do a stat() on the      */
   /* current working directory.                           */
   return(sftp_stat(".", NULL));
}


/*############################# sftp_quit() #############################*/
void
sftp_quit(void)
{
#ifdef WITH_TRACE
   if ((scd.debug == TRACE_MODE) || (scd.debug == FULL_TRACE_MODE))
   {
      trace_log(__FILE__, __LINE__, C_TRACE, NULL, 0,
                "sftp_quit(): Quitting ...");
   }
#endif

   /* Free all allocated memory. */
   if (scd.cwd != NULL)
   {
      free(scd.cwd);
      scd.cwd = NULL;
   }
   if (scd.file_handle != NULL)
   {
      free(scd.file_handle);
      scd.file_handle = NULL;
   }
   if (scd.dir_handle != NULL)
   {
      (void)sftp_close_dir();
   }
   if (scd.stored_replies > 0)
   {
      int i;

      for (i = 0; i < scd.stored_replies; i++)
      {
         free(scd.sm[i].sm_buffer);
      }
   }
   if (msg != NULL)
   {
      free(msg);
      msg = NULL;
   }

   /* Close pipe for read/write data connection. */
   if (data_fd != -1)
   {
      if (close(data_fd) == -1)
      {
         trans_log(WARN_SIGN, __FILE__, __LINE__, "sftp_quit", NULL,
                   _("Failed to close() write pipe to ssh process : %s"),
                   strerror(errno));
      }
      data_fd = -1;
   }

   /* Remove ssh process for writing data. */
   if (data_pid > 0)
   {
      int loop_counter,
          max_waitpid_loops,
          status;

      errno = 0;
      loop_counter = 0;
      max_waitpid_loops = (transfer_timeout / 2) * 10;
      while ((waitpid(data_pid, &status, WNOHANG) != data_pid) &&
             (loop_counter < max_waitpid_loops))
      {
         my_usleep(100000L);
         loop_counter++;
      }
      if ((errno != 0) || (loop_counter >= max_waitpid_loops))
      {
         msg_str[0] = '\0';
         if (errno != 0)
         {
            trans_log(WARN_SIGN, __FILE__, __LINE__, "sftp_quit", NULL,
                      _("Failed to catch zombie of data ssh process : %s"),
                      strerror(errno));
         }
         if (data_pid > 0)
         {
            if (kill(data_pid, SIGKILL) == -1)
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "sftp_quit", NULL,
                         _("Failed to kill() data ssh process %d : %s"),
                         data_pid, strerror(errno));
            }
            else
            {
               trans_log(WARN_SIGN, __FILE__, __LINE__, "sftp_quit", NULL,
                         _("Killing hanging data ssh process."));
            }
         }
         else
         {
            trans_log(DEBUG_SIGN, __FILE__, __LINE__, "sftp_quit", NULL,
# if SIZEOF_PID_T == 4
                      _("Hmm, pid is %d!!!"), (pri_pid_t)data_pid);
# else
                      _("Hmm, pid is %lld!!!"), (pri_pid_t)data_pid);
# endif
         }
      }
      data_pid = -1;
   }
   else if (simulation_mode == YES)
        {
           /* Close pipe for read/write data connection. */
           if (data_fd != -1)
           {
              if (close(data_fd) == -1)
              {
                 trans_log(WARN_SIGN, __FILE__, __LINE__, "sftp_quit", NULL,
                           _("Failed to close() write pipe to ssh process : %s"),
                           strerror(errno));
              }
              data_fd = -1;
           }
        }

   return;
}


/*++++++++++++++++++++++++++++ get_reply() ++++++++++++++++++++++++++++++*/
static int
get_reply(unsigned int id, int line)
{
   unsigned int msg_length;
   int          reply;

   if (simulation_mode == YES)
   {
      return(SIMULATION);
   }

   if (scd.stored_replies > 0)
   {
      int i;

      for (i = 0; i < scd.stored_replies && i < MAX_SFTP_REPLY_BUFFER; i++)
      {
         if (scd.sm[i].request_id == id)
         {
            /* Save content of stored message to buffer msg. */
            (void)memcpy(msg, scd.sm[i].sm_buffer, scd.sm[i].message_length);

            /* Remove reply from buffer and free its memory. */
            free(scd.sm[i].sm_buffer);
            if ((scd.stored_replies > 1) && (i != (scd.stored_replies - 1)))
            {
               size_t move_size = (scd.stored_replies - 1 - i) *
                                  sizeof(struct stored_messages);

               (void)memmove(&scd.sm[i], &scd.sm[i + 1], move_size);
            }
            scd.stored_replies--;

            return(SUCCESS);
         }
      }
   }

retry:
   if ((reply = read_msg(msg, 4, line)) == SUCCESS)
   {
      msg_length = get_xfer_uint(msg);
      if (msg_length <= MAX_SFTP_MSG_LENGTH)
      {
         if ((reply = read_msg(msg, (int)msg_length, line)) == SUCCESS)
         {
            unsigned int reply_id;

            reply_id = get_xfer_uint(&msg[1]);
            if (reply_id != id)
            {
               if (scd.stored_replies == MAX_SFTP_REPLY_BUFFER)
               {
                  trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_reply", NULL,
                            _("Only able to queue %d replies, try increase MAX_SFTP_REPLY_BUFFER and recompile. [%d]"),
                            MAX_SFTP_REPLY_BUFFER, line);
                  reply = INCORRECT;
               }
               else
               {
                  if ((scd.sm[scd.stored_replies].sm_buffer = malloc(msg_length)) == NULL)
                  {
                     trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_reply", NULL,
                               _("Unable to malloc() %u bytes [%d] : %s"),
                               msg_length, line, strerror(errno));
                     reply = INCORRECT;
                  }
                  else
                  {
                     (void)memcpy(scd.sm[scd.stored_replies].sm_buffer,
                                  msg, msg_length);
                     scd.stored_replies++;

                     goto retry;
                  }
               }
            }
         }
      }
      else
      {
         trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_reply", NULL,
                   _("Received message is %u bytes, can only handle %d bytes. [%d]"),
                   msg_length, MAX_SFTP_MSG_LENGTH, line);
         reply = INCORRECT;
      }
   }

#ifdef WITH_TRACE
   if ((reply == SUCCESS) && (scd.debug == TRACE_MODE))
   {
      show_sftp_cmd(msg_length, R_TRACE);
   }
#endif

   return(reply);
}


/*++++++++++++++++++++++++++ get_write_reply() ++++++++++++++++++++++++++*/
static int
get_write_reply(unsigned int id, int line)
{
   int reply;

   if (simulation_mode == YES)
   {
      scd.pending_write_counter = 0;
      scd.stored_replies = 0;
      return(SIMULATION);
   }

   if (scd.pending_write_counter == -1)
   {
      if ((reply = get_reply(id, line)) == SUCCESS)
      {
         scd.pending_write_counter = 0;
      }
      else if (reply == SIMULATION)
           {
              scd.pending_write_counter = 0;
              reply = SUCCESS;
           }
   }
   else
   {
      register int i;
      int          got_current_id = NO,
                   gotcha;
      unsigned int msg_length,
                   reply_id;

      do
      {
         if ((reply = read_msg(msg, 4, line)) == SUCCESS)
         {
            msg_length = get_xfer_uint(msg);
            if (msg_length <= MAX_SFTP_MSG_LENGTH)
            {
               if ((reply = read_msg(msg, (int)msg_length, line)) == SUCCESS)
               {
#ifdef WITH_TRACE
                  if (scd.debug == TRACE_MODE)
                  {
                     show_sftp_cmd(msg_length, R_TRACE);
                  }
#endif
                  gotcha = NO;
                  reply_id = get_xfer_uint(&msg[1]);

                  for (i = 0; i < scd.pending_write_counter; i++)
                  {
                     if (reply_id == scd.pending_write_id[i])
                     {
                        if ((scd.pending_write_counter > 1) &&
                            (i != (scd.pending_write_counter - 1)))
                        {
                           size_t move_size = (scd.pending_write_counter - 1 - i) *
                                              sizeof(unsigned int);

                           (void)memmove(&scd.pending_write_id[i],
                                         &scd.pending_write_id[i + 1],
                                         move_size);
                        }
                        scd.pending_write_counter--;
                        gotcha = YES;
                        break;
                     }
                  }
                  if (gotcha == NO)
                  {
                     if (got_current_id == NO)
                     {
                        if (reply_id == id)
                        {
                           got_current_id = YES;
                           gotcha = YES;
                        }
                     }
                     if (gotcha == NO)
                     {
                        if (scd.stored_replies == MAX_SFTP_REPLY_BUFFER)
                        {
                           trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_write_reply", NULL,
                                     _("Only able to queue %d replies, try increase MAX_SFTP_REPLY_BUFFER and recompile. [%d]"),
                                     MAX_SFTP_REPLY_BUFFER, line);
                           reply = INCORRECT;
                        }
                        else
                        {
                           if ((scd.sm[scd.stored_replies].sm_buffer = malloc(msg_length)) == NULL)
                           {
                              trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_write_reply", NULL,
                                        _("Unable to malloc() %u bytes [%d] : %s"),
                                        msg_length, line, strerror(errno));
                              reply = INCORRECT;
                           }
                           else
                           {
                              (void)memcpy(scd.sm[scd.stored_replies].sm_buffer,
                                           msg, msg_length);
                              scd.stored_replies++;
                           }
                        }
                     }
                  }
                  if (gotcha == YES)
                  {
                     if ((msg[0] == SSH_FXP_STATUS) &&
                         (get_xfer_uint(&msg[5]) == SSH_FX_OK))
                     {
                        reply = SUCCESS;
                     }
                     else
                     {
                        reply = INCORRECT;
                     }
                  }
               }
            }
            else
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_write_reply", NULL,
                         _("Received message is %u bytes, can only handle %d bytes. [%d]"),
                         msg_length, MAX_SFTP_MSG_LENGTH, line);
               reply = INCORRECT;
            }
         }
      } while ((reply == SUCCESS) &&
               ((scd.pending_write_counter > 0) || (got_current_id == NO)) &&
               ((scd.pending_write_counter == scd.max_pending_writes) ||
                (check_msg_pending() == YES)));

      if ((got_current_id == NO) && (reply == SUCCESS) &&
          (scd.pending_write_counter < scd.max_pending_writes))
      {
         scd.pending_write_id[scd.pending_write_counter] = id;
         scd.pending_write_counter++;
      }
   }

   return(reply);
}


/*-------------------------- check_msg_pending() ------------------------*/
static int
check_msg_pending(void)
{
   int            status;
   fd_set         rset;
   struct timeval timeout;

   /* Initialise descriptor set. */
   FD_ZERO(&rset);
   FD_SET(data_fd, &rset);
   timeout.tv_usec = 0L;
   timeout.tv_sec = 0L;

   /* Wait for message x seconds and then continue. */
   status = select(data_fd + 1, &rset, NULL, NULL, &timeout);

   if (status == 0)
   {
      /* Nothing in the pipe. */
      status = NO;
   }
   else if (FD_ISSET(data_fd, &rset))
        {
           status = YES;
        }
        else
        {
           status = NO;
        }

   return(status);
}


/*+++++++++++++++++++++++++++++ write_msg() +++++++++++++++++++++++++++++*/
static int
write_msg(char *block, int size, int line)
{
#ifdef WITH_TRACE
   int            continue_show = NO,
                  type,
                  what_to_show;
#endif
   int            nleft,
                  status;
   char           *ptr;
   fd_set         wset;
   struct timeval timeout;

   /* Initialise descriptor set. */
   FD_ZERO(&wset);

   ptr = block;
   nleft = size;
   while (nleft > 0)
   {
      FD_SET(data_fd, &wset);
      timeout.tv_usec = 0L;
      timeout.tv_sec = transfer_timeout;

      /* Wait for message x seconds and then continue. */
      status = select(data_fd + 1, NULL, &wset, NULL, &timeout);

      if (status == 0)
      {
         /* Timeout has arrived. */
         timeout_flag = ON;
         return(INCORRECT);
      }
      else if (FD_ISSET(data_fd, &wset))
           {
              int tmp_errno;

              /* In some cases, the write system call hangs. */
              if (signal(SIGALRM, sig_handler) == SIG_ERR)
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "write_msg", NULL,
                           _("Failed to set signal handler [%d] : %s"),
                           line, strerror(errno));
                 return(INCORRECT);
              }
              if (sigsetjmp(env_alrm, 1) != 0)
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "write_msg", NULL,
                           _("write() timeout (%ld) [%d]"),
                           transfer_timeout, line);
                 timeout_flag = ON;
                 return(INCORRECT);
              }
              (void)alarm(transfer_timeout);
              status = write(data_fd, ptr, nleft);
              tmp_errno = errno;
              (void)alarm(0);

              if (status <= 0)
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "write_msg", NULL,
                           _("write() error (%d) [%d] : %s"),
                           status, line, strerror(tmp_errno));
                 return(tmp_errno);
              }
#ifdef WITH_TRACE
              if (scd.debug == TRACE_MODE)
              {
                 if ((nleft == size) && (status > 4))
                 {
                    show_sftp_cmd((unsigned int)(size - 4), W_TRACE);
                    if (*(ptr + 4) == SSH_FXP_WRITE)
                    {
                       if (status < (4 + 1 + 4 + 4 + scd.file_handle_length + 8 + 4))
                       {
                          what_to_show = status;
                       }
                       else
                       {
                          what_to_show = 4 + 1 + 4 + 4 + scd.file_handle_length + 8 + 4;
                       }
                    }
                    else
                    {
                       what_to_show = status;
                       continue_show = YES;
                    }
                 }
                 else
                 {
                    if ((continue_show == YES) ||
                        ((nleft == size) && (status < 5)))
                    {
                       what_to_show = status;
                    }
                    else
                    {
                       what_to_show = 0;
                    }
                 }
                 type = BIN_CMD_W_TRACE;
              }
              else if (scd.debug == FULL_TRACE_MODE)
                   {
                      what_to_show = status;
                      type = BIN_W_TRACE;
                   }
                   else
                   {
                      what_to_show = 0;
                   }
              if (what_to_show > 0)
              {
                 trace_log(NULL, 0, type, block, what_to_show, NULL);
              }
#endif
              nleft -= status;
              ptr   += status;
           }
      else if (status < 0)
           {
              trans_log(ERROR_SIGN, __FILE__, __LINE__, "write_msg", NULL,
                        _("select() error [%d] : %s"), line, strerror(errno));
              return(INCORRECT);
           }
           else
           {
              trans_log(ERROR_SIGN, __FILE__, __LINE__, "write_msg", NULL,
                        _("Unknown condition. [%d]"), line);
              return(INCORRECT);
           }
   }
   
   return(SUCCESS);
}


/*++++++++++++++++++++++++++++++ read_msg() +++++++++++++++++++++++++++++*/
static int
read_msg(char *block, int blocksize, int line)
{
#ifdef WITH_TRACE
   int            type;
#endif
   int            bytes_read,
                  status,
                  total_read = 0;
   fd_set         rset;
   struct timeval timeout;

   FD_ZERO(&rset);
   do
   {
      /* Initialise descriptor set. */
      FD_SET(data_fd, &rset);
      timeout.tv_usec = 0L;
      timeout.tv_sec = transfer_timeout;

      /* Wait for message x seconds and then continue. */
      status = select(data_fd + 1, &rset, NULL, NULL, &timeout);

      if (FD_ISSET(data_fd, &rset))
      {
         int tmp_errno;

         if (signal(SIGALRM, sig_handler) == SIG_ERR)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "read_msg", NULL,
                      _("Failed to set signal handler [%d] : %s"),
                      line, strerror(errno));
            msg_str[0] = '\0';
            return(INCORRECT);
         }
         if (sigsetjmp(env_alrm, 1) != 0)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "read_msg", NULL,
                      _("read() timeout (%ld) [%d]"), transfer_timeout, line);
            timeout_flag = ON;
            return(INCORRECT);
         }
         (void)alarm(transfer_timeout);
         bytes_read = read(data_fd, &block[total_read], blocksize - total_read);
         tmp_errno = errno;
         (void)alarm(0);

         if (bytes_read == -1)
         {
            if (tmp_errno == ECONNRESET)
            {
               timeout_flag = CON_RESET;
            }
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "read_msg", NULL,
                      _("read() error [%d] : %s"), line, strerror(tmp_errno));
            msg_str[0] = '\0';
            return(INCORRECT);
         }
         else if (bytes_read == 0)
              {
                 trans_log(ERROR_SIGN, __FILE__, __LINE__, "read_msg", NULL,
                           _("Pipe has been closed! [%d]"), line);
                 (void)strcpy(msg_str, "Connection closed");
                 return(INCORRECT);
              }
              else
              {
                 total_read += bytes_read;
              }

#ifdef WITH_TRACE
         if (scd.debug == TRACE_MODE)
         {
            if ((bytes_read > 4) && (*(block + 4) == SSH_FXP_DATA))
            {
               /*
                * From a SSH_FXP_READ request we just want to see
                * the beginning, not the data.
                * NOTE: Since we can show 16 bytes on one line, lets
                *       always show the first 3 bytes of data as well.
                */
               if (bytes_read < (4 + 1 + 4 + 4 + 3))
               {
                  status = bytes_read;
               }
               else
               {
                  status = 4 + 1 + 4 + 4 + 3;
               }
            }
            else
            {
               status = 0;
            }
            type = BIN_CMD_R_TRACE;
         }
         else if (scd.debug == FULL_TRACE_MODE)
              {
                 status = bytes_read;
                 type = BIN_R_TRACE;
              }
              else
              {
                 status = 0;
              }
         if (status > 0)
         {
            trace_log(NULL, 0, type, block, status, NULL);
         }
#endif
      }
      else if (status == 0)
           {
              /* Timeout has arrived. */
              timeout_flag = ON;
              return(INCORRECT);
           }
           else
           {
              trans_log(ERROR_SIGN, __FILE__, __LINE__, "read_msg", NULL,
                        _("select() error [%d] : %s"), line, strerror(errno));
              msg_str[0] = '\0';
              return(INCORRECT);
           }
   } while (total_read < blocksize);

   return(SUCCESS);
}


#ifdef WITH_TRACE
/*++++++++++++++++++++++++++ show_sftp_cmd() ++++++++++++++++++++++++++++*/
static void
show_sftp_cmd(unsigned int ui_var, int type)
{
   int  length,
        offset;
   char buffer[1024];

   if (type == R_TRACE)
   {
      offset = 0;
   }
   else
   {
      offset = 4;
   }

   switch ((unsigned char)msg[offset])
   {
      case SSH_FXP_INIT :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_INIT length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_VERSION :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_VERSION length=%u version=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         if ((offset == 0) && (ui_var > 5))
         {
            int  str_len;
            char *ptr,
                 *p_xfer_str;

            length += snprintf(buffer + length, 1024 - length, " extensions=");
            ui_var -= 5;
            ptr = &msg[5];
            while ((ui_var > 0) && (length < 1024))
            {
               if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                   (str_len > ui_var))
               {
                  break;
               }
               else
               {
                  length += snprintf(buffer + length, 1024 - length,
                                     "%s", p_xfer_str);
                  ui_var -= (str_len + 4);
                  ptr += (str_len + 4);
                  free(p_xfer_str);
                  p_xfer_str = NULL;
                  if (((str_len = get_xfer_str(ptr, &p_xfer_str)) == 0) ||
                      (str_len > ui_var))
                  {
                     break;
                  }
                  else
                  {
                     length += snprintf(buffer + length, 1024 - length,
                                        ":%s ", p_xfer_str);
                  }

                  /* Away with the version number. */
                  ui_var -= (str_len + 4);
                  ptr += (str_len + 4);
                  free(p_xfer_str);
                  p_xfer_str = NULL;
               }
            }
         }
         break;
      case SSH_FXP_OPEN :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_OPEN length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_CLOSE :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_CLOSE length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_READ :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_READ length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_WRITE :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_WRITE length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_LSTAT :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_LSTAT length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_FSTAT :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_FSTAT length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_SETSTAT :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_SETSTAT length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_FSETSTAT :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_FSETSTAT length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_OPENDIR :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_OPENDIR length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_READDIR :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_READDIR length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_REMOVE :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_REMOVE length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_MKDIR :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_MKDIR length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_RMDIR :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_RMDIR length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_REALPATH :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_REALPATH length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_STAT :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_STAT length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_RENAME :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_RENAME length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_READLINK :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_READLINK length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_SYMLINK :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_SYMLINK length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_LINK :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_LINK length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_BLOCK :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_BLOCK length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_UNBLOCK :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_UNBLOCK length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_STATUS :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_STATUS length=%u id=%u %s",
                           ui_var, get_xfer_uint(&msg[offset + 1]),
                           error_2_str(&msg[5]));
         break;
      case SSH_FXP_HANDLE :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_HANDLE length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         if ((offset == 0) && (ui_var > 5))
         {
            unsigned int handle_length;
            char         *handle = NULL;

            if ((handle_length = get_xfer_str(&msg[5], &handle)) != 0)
            {
               if (handle_length == 4)
               {
                  length += snprintf(buffer + length, 1024 - length,
                                     " handle=%u", get_xfer_uint(handle));
               }
               else if (handle_length == 8)
                    {
                       length += snprintf(buffer + length, 1024 - length,
# ifdef HAVE_LONG_LONG
                                          " handle=%llu",
# else
                                          " handle=%lu",
# endif
                                          get_xfer_uint64(handle));
                    }
                    else
                    {
                       int i;

                       length += snprintf(buffer + length, 1024 - length,
                                         " handle=<");
                       for (i = 0; i < (int)handle_length; i++)
                       {
                          if (((unsigned char)handle[i] < 32) ||
                              ((unsigned char)handle[i] > 126))
                          {
                             *(buffer + length + i) = '.';
                          }
                          else
                          {
                             *(buffer + length + i) = handle[i];
                          }
                       }
                       length = length + i;
                       *(buffer + length) = '>';
                       length++;
                       *(buffer + length) = '\0';
                    }
            }
            free(handle);
         }
         break;
      case SSH_FXP_DATA :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_DATA length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_NAME :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_NAME length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         if ((offset == 0) && (ui_var > 5))
         {
            unsigned int ui_var;

            ui_var = get_xfer_uint(&msg[5]);
            if (ui_var == 1)
            {
               char *name = NULL;

               (void)get_xfer_str(&msg[9], &name);
               length += snprintf(buffer + length, 1024 - length,
                                  " name=%s", name);
               free(name);
            }
            else if (ui_var > 1)
                 {
                    int       i;
                    time_t    mtime;
                    char      dstr[26],
                              mstr[11];
                    struct tm *p_tm;

                    length += snprintf(buffer + length, 1024 - length,
                                       " name list with %u elements", ui_var);
                    trace_log(NULL, 0, type, buffer, length, NULL);
                    length = 0;

                    get_xfer_names(ui_var, &msg[1 + 4 + 4]);
                    for (i = 0; i < ui_var; i++)
                    {
                       if (scd.nl_pos < scd.nl_length)
                       {
                          mtime = scd.nl[scd.nl_pos].stat_buf.st_mtime;
                          p_tm = gmtime(&mtime);
                          (void)strftime(dstr, 26, "%a %h %d %H:%M:%S %Y", p_tm);
                          mode_t2str(scd.nl[scd.nl_pos].stat_buf.st_mode, mstr),
                          length = snprintf(buffer, 1024,
# if SIZEOF_OFF_T == 4
                                            "SSH_FXP_NAME[%d]: %s %s %*ld uid=%06u gid=%06u mode=%05o %s",
# else
                                            "SSH_FXP_NAME[%d]: %s %s %*lld uid=%06u gid=%06u mode=%05o %s",
# endif
                                            i, mstr, dstr, MAX_OFF_T_LENGTH,
                                            (pri_off_t)scd.nl[scd.nl_pos].stat_buf.st_size,
                                            (unsigned int)scd.nl[scd.nl_pos].stat_buf.st_uid,
                                            (unsigned int)scd.nl[scd.nl_pos].stat_buf.st_gid,
                                            (scd.nl[scd.nl_pos].stat_buf.st_mode & ~S_IFMT),
                                            scd.nl[scd.nl_pos].name);
                          trace_log(NULL, 0, type, buffer, length, NULL);
                          scd.nl_pos++;
                       }
                    }
                    for (i = 0; i < scd.nl_length; i++)
                    {
                       free(scd.nl[i].name);
                    }
                    free(scd.nl);
                    scd.nl = NULL;
                    length = 0;
                 }
                 else
                 {
                    length += snprintf(buffer + length, 1024 - length,
                                       " name=");
                 }
         }
         break;
      case SSH_FXP_ATTRS :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_ATTRS length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         if ((offset == 0) && (ui_var > 5))
         {
            unsigned int stat_flag;
            struct stat  stat_buf;

            (void)store_attributes(&msg[5], &stat_flag, &stat_buf);
            length += snprintf(buffer + length, 1024 - length,
                               " st_mode=%c", mode2type(stat_buf.st_mode));
            if (stat_flag & SSH_FILEXFER_ATTR_PERMISSIONS)
            {
                *(buffer + length) = (stat_buf.st_mode & 0400) ? 'r' : '-';
                length++;
                *(buffer + length) = (stat_buf.st_mode & 0200) ? 'w' : '-';
                length++;
                if (stat_buf.st_mode & 04000)
                {
                   *(buffer + length) = (stat_buf.st_mode & 0100) ? 's' : 'S';
                }
                else
                {
                   *(buffer + length) = (stat_buf.st_mode & 0100) ? 'x' : '-';
                }
                length++;
                *(buffer + length) = (stat_buf.st_mode & 040) ? 'r' : '-';
                length++;
                *(buffer + length) = (stat_buf.st_mode & 020) ? 'w' : '-';
                length++;
                if (stat_buf.st_mode & 02000)
                {
                   *(buffer + length) = (stat_buf.st_mode & 010) ? 's' : 'S';
                }
                else
                {
                   *(buffer + length) = (stat_buf.st_mode & 010) ? 'x' : '-';
                }
                length++;
                *(buffer + length) = (stat_buf.st_mode & 04) ? 'r' : '-';
                length++;
                *(buffer + length) = (stat_buf.st_mode & 02) ? 'w' : '-';
                length++;
                if (stat_buf.st_mode & 01000)
                {
                   *(buffer + length) = (stat_buf.st_mode & 01) ? 't' : 'T';
                }
                else
                {
                   *(buffer + length) = (stat_buf.st_mode & 01) ? 'x' : '-';
                }
                length++;
                *(buffer + length) = '\0';
            }
            else
            {
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '.';
                length++;
                *(buffer + length) = '\0';
            }
            if (stat_flag & SSH_FILEXFER_ATTR_SIZE)
            {
               length += snprintf(buffer + length, 1024 - length,
# if SIZEOF_OFF_T == 4
                                  " st_size=%ld",
# else
                                  " st_size=%lld",
# endif
                                  (pri_off_t)stat_buf.st_size);
            }
            if (stat_flag & SSH_FILEXFER_ATTR_UIDGID)
            {
               length += snprintf(buffer + length, 1024 - length,
                                  " st_uid=%u st_gid=%u",
                                  (unsigned int)stat_buf.st_uid,
                                  (unsigned int)stat_buf.st_gid);
            }
# ifdef WITH_OWNER_GROUP_EVAL
            if (stat_flag & SSH_FILEXFER_ATTR_OWNERGROUP)
            {
               length += snprintf(buffer + length, 1024 - length,
                                  " st_uid=%u st_gid=%u",
                                  (unsigned int)stat_buf.st_uid,
                                  (unsigned int)stat_buf.st_gid);
            }
# endif
            if (stat_flag & SSH_FILEXFER_ATTR_ACMODTIME)
            {
               length += snprintf(buffer + length, 1024 - length,
                                  " st_atime=%u st_mtime=%u",
                                  (unsigned int)stat_buf.st_atime,
                                  (unsigned int)stat_buf.st_mtime);
            }
         }
         break;
      case SSH_FXP_EXTENDED :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_EXTENDED length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      case SSH_FXP_EXTENDED_REPLY :
         length = snprintf(buffer, 1024,
                           "SSH_FXP_EXTENDED_REPLY length=%u id=%u",
                           ui_var, get_xfer_uint(&msg[offset + 1]));
         break;
      default :
         length = 0;
         break;
   }
   if (length > 0)
   {
      trace_log(NULL, 0, type, buffer, length, NULL);
   }

   return;
}


/*----------------------------- mode2type() -----------------------------*/
static char
mode2type(unsigned st_mode)
{
   switch(st_mode & S_IFMT)
   {
      case S_IFREG : return '-';
      case S_IFDIR : return 'd';
      case S_IFLNK : return 'l';
# ifdef S_IFSOCK
      case S_IFSOCK: return 's';
# endif
      case S_IFCHR : return 'c';
      case S_IFBLK : return 'b';
      case S_IFIFO : return 'p';
      default      : return '?';
   }
}


/*+++++++++++++++++++++++++ show_trace_handle() +++++++++++++++++++++++++*/
static void
show_trace_handle(char         *function,
                  unsigned int request_id,
                  char         *ssh_fxp_cmd,
                  char         *handle,
                  unsigned int handle_length,
                  off_t        offset,
                  int          block_size,
                  char         *file,
                  int          line,
                  int          rw_mode)
{
   int length;

   if (handle_length == 4)
   {
      if (rw_mode == YES)
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# if SIZEOF_OFF_T == 4
                           "%s: request-id=%d %s handle=%u offset=%ld block_size=%d",
# else
                           "%s: request-id=%d %s handle=%u offset=%lld block_size=%d",
# endif
                           function, request_id, ssh_fxp_cmd,
                           get_xfer_uint(handle), (pri_off_t)offset,
                           block_size);
      }
      else
      {
         length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                           "%s: request-id=%d %s handle=%u",
                           function, request_id, ssh_fxp_cmd,
                           get_xfer_uint(handle));
      }
   }
   else if (handle_length == 8)
        {
           if (rw_mode == YES)
           {
              length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# ifdef HAVE_LONG_LONG
#  if SIZEOF_OFF_T == 4
                                "%s: request-id=%d %s handle=%llu offset=%ld block_size=%d",
#  else
                                "%s: request-id=%d %s handle=%llu offset=%lld block_size=%d",
#  endif
# else
#  if SIZEOF_OFF_T == 4
                                "%s: request-id=%d %s handle=%lu offset=%ld block_size=%d",
#  else
                                "%s: request-id=%d %s handle=%lu offset=%lld block_size=%d",
#  endif
# endif
                                function, request_id, ssh_fxp_cmd,
                                get_xfer_uint64(handle), (pri_off_t)offset,
                                block_size);
           }
           else
           {
              length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
# ifdef HAVE_LONG_LONG
                                "%s: request-id=%d %s handle=%llu",
# else
                                "%s: request-id=%d %s handle=%lu",
# endif
                                function, request_id, ssh_fxp_cmd,
                                get_xfer_uint64(handle));
           }
        }
        else
        {
           int i;

           length = snprintf(msg_str, MAX_RET_MSG_LENGTH,
                             "%s: request-id=%d %s handle=<",
                             function, request_id, ssh_fxp_cmd);
           for (i = 0; i < (int)handle_length; i++)
           {
              if (((unsigned char)handle[i] < 32) ||
                  ((unsigned char)handle[i] > 126))
              {
                 msg_str[i] = '.';
              }
              else
              {
                 msg_str[i] = handle[i];
              }
           }
           length = length + i;
           if (rw_mode == YES)
           {
              length += snprintf(msg_str + length, MAX_RET_MSG_LENGTH - length,
# if SIZEOF_OFF_T == 4
                                 "> offset=%ld block_size=%d",
# else
                                 "> offset=%lld block_size=%d",
# endif
                                 (pri_off_t)offset, block_size);
           }
           else
           {
              msg_str[length] = '>';
              length++;
              msg_str[length] = '\0';
           }
        }
   trace_log(file, line, C_TRACE, msg_str, length, NULL);
   msg_str[0] = '\0';

   return;
}
#endif /* WITH_TRACE */


/*++++++++++++++++++++++++++ get_xfer_uint() ++++++++++++++++++++++++++++*/
static unsigned int
get_xfer_uint(char *msg)
{
   unsigned int ui_var;

   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
      ((char *)&ui_var)[3] = msg[0];
      ((char *)&ui_var)[2] = msg[1];
      ((char *)&ui_var)[1] = msg[2];
      ((char *)&ui_var)[0] = msg[3];
   }
   else
   {
      /* big-endian */
      ((char *)&ui_var)[0] = msg[0];
      ((char *)&ui_var)[1] = msg[1];
      ((char *)&ui_var)[2] = msg[2];
      ((char *)&ui_var)[3] = msg[3];
   }

   return(ui_var);
}


/*+++++++++++++++++++++++++ get_xfer_uint64() +++++++++++++++++++++++++++*/
static u_long_64
get_xfer_uint64(char *msg)
{
   u_long_64 ui_var;

   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
#ifdef HAVE_LONG_LONG
      ((char *)&ui_var)[7] = msg[0];
      ((char *)&ui_var)[6] = msg[1];
      ((char *)&ui_var)[5] = msg[2];
      ((char *)&ui_var)[4] = msg[3];
#endif
      ((char *)&ui_var)[3] = msg[4];
      ((char *)&ui_var)[2] = msg[5];
      ((char *)&ui_var)[1] = msg[6];
      ((char *)&ui_var)[0] = msg[7];
   }
   else
   {
      /* big-endian */
      ((char *)&ui_var)[0] = msg[0];
      ((char *)&ui_var)[1] = msg[1];
      ((char *)&ui_var)[2] = msg[2];
      ((char *)&ui_var)[3] = msg[3];
#ifdef HAVE_LONG_LONG
      ((char *)&ui_var)[4] = msg[4];
      ((char *)&ui_var)[5] = msg[5];
      ((char *)&ui_var)[6] = msg[6];
      ((char *)&ui_var)[7] = msg[7];
#endif
   }

   return(ui_var);
}


/*++++++++++++++++++++++++++ get_xfer_str() +++++++++++++++++++++++++++++*/
static int
get_xfer_str(char *msg, char **p_xfer_str)
{
   unsigned int ui_var;

   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
      ((char *)&ui_var)[3] = msg[0];
      ((char *)&ui_var)[2] = msg[1];
      ((char *)&ui_var)[1] = msg[2];
      ((char *)&ui_var)[0] = msg[3];
   }
   else
   {
      /* big-endian */
      ((char *)&ui_var)[0] = msg[0];
      ((char *)&ui_var)[1] = msg[1];
      ((char *)&ui_var)[2] = msg[2];
      ((char *)&ui_var)[3] = msg[3];
   }
   if (ui_var <= MAX_SFTP_MSG_LENGTH)
   {
      if (p_xfer_str != NULL)
      {
         if ((*p_xfer_str = malloc((ui_var + 1))) == NULL)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_xfer_str", NULL,
                      _("Failed to malloc() %u bytes : %s"),
                      ui_var, strerror(errno));
            ui_var = 0;
         }
         else
         {
            (void)memcpy(*p_xfer_str, &msg[4], ui_var);
            *(*p_xfer_str + ui_var) = '\0';
         }
      }
   }
   else
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_xfer_str", NULL,
                _("Received message is %u bytes, can only handle %d bytes."),
                ui_var, MAX_SFTP_MSG_LENGTH);
      ui_var = 0;
   }

   return(ui_var);
}


/*+++++++++++++++++++++++++++ get_msg_str() +++++++++++++++++++++++++++++*/
static void
get_msg_str(char *msg)
{
   unsigned int ui_var;

   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
      ((char *)&ui_var)[3] = msg[0];
      ((char *)&ui_var)[2] = msg[1];
      ((char *)&ui_var)[1] = msg[2];
      ((char *)&ui_var)[0] = msg[3];
   }
   else
   {
      /* big-endian */
      ((char *)&ui_var)[0] = msg[0];
      ((char *)&ui_var)[1] = msg[1];
      ((char *)&ui_var)[2] = msg[2];
      ((char *)&ui_var)[3] = msg[3];
   }
   if (ui_var > (MAX_RET_MSG_LENGTH - 1))
   {
      ui_var = MAX_RET_MSG_LENGTH - 1;
   }
   (void)memcpy(msg_str, &msg[4], ui_var);
   *(msg_str + ui_var) = '\0';

   return;
}


/*+++++++++++++++++++++++++ get_xfer_names() ++++++++++++++++++++++++++++*/
static int
get_xfer_names(unsigned int no_of_names, char *msg)
{
   int    status = SUCCESS;
   size_t length;

   free(scd.nl);
   scd.nl_length = no_of_names;
   length = no_of_names * sizeof(struct name_list);
   if ((scd.nl = malloc(length)) == NULL)
   {
      trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_xfer_names", NULL,
                _("Failed to malloc() %u bytes : %s"), length, strerror(errno));
      status = INCORRECT;
   }
   else
   {
      int          str_len;
      unsigned int i,
                   ui_var;
      char         *ptr;

      scd.nl_pos = 0;
      ptr = msg;
      for (i = 0; i < no_of_names; i++)
      {
         if ((str_len = get_xfer_str(ptr, &scd.nl[i].name)) == 0)
         {
            scd.nl_length = i;
            return(INCORRECT);
         }
         ptr += str_len + 4;
         if (scd.version < 4)
         {
            /*
             * Not sure what the long name is good for,
             * so lets just ignore it.
             */
            if (*(char *)&byte_order == 1)
            {
               /* little-endian */
               ((char *)&ui_var)[3] = *ptr;
               ((char *)&ui_var)[2] = *(ptr + 1);
               ((char *)&ui_var)[1] = *(ptr + 2);
               ((char *)&ui_var)[0] = *(ptr + 3);
            }
            else
            {
               /* big-endian */
               ((char *)&ui_var)[0] = *ptr;
               ((char *)&ui_var)[1] = *(ptr + 1);
               ((char *)&ui_var)[2] = *(ptr + 2);
               ((char *)&ui_var)[3] = *(ptr + 3);
            }
            if (ui_var > MAX_SFTP_MSG_LENGTH)
            {
               trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_xfer_names", NULL,
                         _("String is %u bytes, can only handle %d bytes."),
                         ui_var, MAX_SFTP_MSG_LENGTH);
               scd.nl_length = i;
               return(INCORRECT);
            }
            ptr += ui_var + 4;
         }
         if ((ui_var = (unsigned int)store_attributes(ptr,
                                                      &scd.nl[i].stat_flag,
                                                      &scd.nl[i].stat_buf)) == INCORRECT)
         {
            trans_log(ERROR_SIGN, __FILE__, __LINE__, "get_xfer_names", NULL,
                      _("Unable to evaluate the file attributes part."));
            scd.nl_length = i;
            return(INCORRECT);
         }
         ptr += ui_var;
      }
   }

   return(status);
}


/*++++++++++++++++++++++++++ set_xfer_uint() ++++++++++++++++++++++++++++*/
static void
set_xfer_uint(char *msg, unsigned int ui_var)
{
   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
      msg[0] = ((char *)&ui_var)[3];
      msg[1] = ((char *)&ui_var)[2];
      msg[2] = ((char *)&ui_var)[1];
      msg[3] = ((char *)&ui_var)[0];
   }
   else
   {
      /* big-endian */
      msg[0] = ((char *)&ui_var)[0];
      msg[1] = ((char *)&ui_var)[1];
      msg[2] = ((char *)&ui_var)[2];
      msg[3] = ((char *)&ui_var)[3];
   }

   return;
}


/*+++++++++++++++++++++++++ set_xfer_uint64() +++++++++++++++++++++++++++*/
static void
set_xfer_uint64(char *msg, u_long_64 ui_var)
{
   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
#ifdef HAVE_LONG_LONG
      msg[0] = ((char *)&ui_var)[7];
      msg[1] = ((char *)&ui_var)[6];
      msg[2] = ((char *)&ui_var)[5];
      msg[3] = ((char *)&ui_var)[4];
#else
      msg[0] = 0;
      msg[1] = 0;
      msg[2] = 0;
      msg[3] = 0;
#endif
      msg[4] = ((char *)&ui_var)[3];
      msg[5] = ((char *)&ui_var)[2];
      msg[6] = ((char *)&ui_var)[1];
      msg[7] = ((char *)&ui_var)[0];
   }
   else
   {
      /* big-endian */
      msg[0] = ((char *)&ui_var)[0];
      msg[1] = ((char *)&ui_var)[1];
      msg[2] = ((char *)&ui_var)[2];
      msg[3] = ((char *)&ui_var)[3];
#ifdef HAVE_LONG_LONG
      msg[4] = ((char *)&ui_var)[4];
      msg[5] = ((char *)&ui_var)[5];
      msg[6] = ((char *)&ui_var)[6];
      msg[7] = ((char *)&ui_var)[7];
#else
      msg[4] = 0;
      msg[5] = 0;
      msg[6] = 0;
      msg[7] = 0;
#endif
   }

   return;
}


/*++++++++++++++++++++++++++ set_xfer_str() +++++++++++++++++++++++++++++*/
static void
set_xfer_str(char *msg, char *p_xfer_str, int length)
{
   if (*(char *)&byte_order == 1)
   {
      /* little-endian */
      msg[0] = ((char *)&length)[3];
      msg[1] = ((char *)&length)[2];
      msg[2] = ((char *)&length)[1];
      msg[3] = ((char *)&length)[0];
   }
   else
   {
      /* big-endian */
      msg[0] = ((char *)&length)[0];
      msg[1] = ((char *)&length)[1];
      msg[2] = ((char *)&length)[2];
      msg[3] = ((char *)&length)[3];
   }
   (void)memcpy(&msg[4], p_xfer_str, length);

   return;
}


/*------------------------ store_attributes() ---------------------------*/
static int
store_attributes(char *msg, unsigned int *p_stat_flag, struct stat *p_stat_buf)
{
   int          pos;
   unsigned int stat_flag;

   (void)memset(p_stat_buf, 0, sizeof(struct stat));
   stat_flag = *p_stat_flag = get_xfer_uint(msg);
   if (scd.version > 3)
   {
      switch (msg[4])
      {
         case SSH_FILEXFER_TYPE_REGULAR      :
            p_stat_buf->st_mode = S_IFREG;
            break;
         case SSH_FILEXFER_TYPE_DIRECTORY    :
            p_stat_buf->st_mode = S_IFDIR;
            break;
         case SSH_FILEXFER_TYPE_SYMLINK      :
            p_stat_buf->st_mode = S_IFLNK;
            break;
         case SSH_FILEXFER_TYPE_SPECIAL      :
            break;
         case SSH_FILEXFER_TYPE_UNKNOWN      :
            break;
         case SSH_FILEXFER_TYPE_SOCKET       :
#ifdef S_IFSOCK
            p_stat_buf->st_mode = S_IFSOCK;
#endif
            break;
         case SSH_FILEXFER_TYPE_CHAR_DEVICE  :
            p_stat_buf->st_mode = S_IFCHR;
            break;
         case SSH_FILEXFER_TYPE_BLOCK_DEVICE :
            p_stat_buf->st_mode = S_IFBLK;
            break;
         case SSH_FILEXFER_TYPE_FIFO         :
            p_stat_buf->st_mode = S_IFIFO;
            break;
         default                             :
            trans_log(DEBUG_SIGN, __FILE__, __LINE__, "store_attributes", NULL,
                      _("Unknown type field %d in protocol."), (int)msg[4]);
            break;
      }
      pos = 5;
   }
   else
   {
      pos = 4;
   }
   if (stat_flag & SSH_FILEXFER_ATTR_SIZE)
   {
      p_stat_buf->st_size = (off_t)get_xfer_uint64(&msg[pos]);
      pos += 8;
      stat_flag &= ~SSH_FILEXFER_ATTR_SIZE;
   }
   if (stat_flag & SSH_FILEXFER_ATTR_UIDGID) /* Up to version 3. */
   {
      p_stat_buf->st_uid = (uid_t)get_xfer_uint(&msg[pos]);
      pos += 4;
      p_stat_buf->st_gid = (gid_t)get_xfer_uint(&msg[pos]);
      pos += 4;
      stat_flag &= ~SSH_FILEXFER_ATTR_UIDGID;
   }
   if (stat_flag & SSH_FILEXFER_ATTR_ALLOCATION_SIZE)
   {
      pos += 8;
      stat_flag &= ~SSH_FILEXFER_ATTR_ALLOCATION_SIZE;
   }
   if (stat_flag & SSH_FILEXFER_ATTR_OWNERGROUP)
   {
      unsigned int  length;
#ifdef WITH_OWNER_GROUP_EVAL
      char          *p_owner_group = NULL,
                    *ptr;
      struct passwd *p_pw;
      struct group  *p_gr;

      /* Get the user ID. */
      if ((length = get_xfer_str(&msg[pos], &p_owner_group)) == 0)
      {
         return(INCORRECT);
      }
      pos += (length + 4);
      ptr = p_owner_group;
      while ((*ptr != '@') && (*ptr != '\0'))
      {
         ptr++;
      }
      if (*ptr == '@')
      {
         *ptr = '\0';
      }
      if ((p_pw = getpwnam(p_owner_group)) != NULL)
      {
         scd.stat_buf.st_uid = p_pw->pw_uid;
      }
      free(p_owner_group);
      p_owner_group = NULL;

      /* Get the group ID. */
      if ((length = get_xfer_str(&msg[pos], &p_owner_group)) == 0)
      {
         return(INCORRECT);
      }
      pos += (length + 4);
      ptr = p_owner_group;
      while ((*ptr != '@') && (*ptr != '\0'))
      {
         ptr++;
      }
      if (*ptr == '@')
      {
         *ptr = '\0';
      }
      if ((p_gr = getgrnam(p_owner_group)) != NULL)
      {
         p_stat_buf->st_gid = p_gr->gr_gid;
      }
      free(p_owner_group);
#else
      if ((length = get_xfer_str(&msg[pos], NULL)) == 0)
      {
         return(INCORRECT);
      }
      pos += (length + 4);
      if ((length = get_xfer_str(&msg[pos], NULL)) == 0)
      {
         return(INCORRECT);
      }
      pos += (length + 4);
#endif
      stat_flag &= ~SSH_FILEXFER_ATTR_OWNERGROUP;
   }
   if (stat_flag & SSH_FILEXFER_ATTR_PERMISSIONS)
   {
      unsigned int ui_var;

      ui_var = get_xfer_uint(&msg[pos]);
      p_stat_buf->st_mode |= ui_var;
      pos += 4;
      stat_flag &= ~SSH_FILEXFER_ATTR_PERMISSIONS;
   }
   if (scd.version < 4)
   {
      if (stat_flag & SSH_FILEXFER_ATTR_ACMODTIME)
      {
         p_stat_buf->st_atime = (time_t)get_xfer_uint(&msg[pos]);
         pos += 4;
         p_stat_buf->st_mtime = (time_t)get_xfer_uint(&msg[pos]);
         pos += 4;
         stat_flag &= ~SSH_FILEXFER_ATTR_ACMODTIME;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_EXTENDED)
      {
      }
   }
   else
   {
      if (stat_flag & SSH_FILEXFER_ATTR_ACCESSTIME)
      {
         p_stat_buf->st_atime = (time_t)get_xfer_uint64(&msg[pos]);
         pos += 8;
         stat_flag &= ~SSH_FILEXFER_ATTR_ACCESSTIME;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_SUBSECOND_TIMES)
      {
         pos += 4;
         stat_flag &= ~SSH_FILEXFER_ATTR_SUBSECOND_TIMES;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_CREATETIME)
      {
         pos += 8;
         stat_flag &= ~SSH_FILEXFER_ATTR_CREATETIME;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_SUBSECOND_TIMES)
      {
         pos += 4;
         stat_flag &= ~SSH_FILEXFER_ATTR_SUBSECOND_TIMES;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_MODIFYTIME)
      {
         p_stat_buf->st_mtime = (time_t)get_xfer_uint64(&msg[pos]);
         pos += 8;
         stat_flag &= ~SSH_FILEXFER_ATTR_MODIFYTIME;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_SUBSECOND_TIMES)
      {
         pos += 4;
         stat_flag &= ~SSH_FILEXFER_ATTR_SUBSECOND_TIMES;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_CTIME)
      {
         p_stat_buf->st_ctime = (time_t)get_xfer_uint64(&msg[pos]);
         pos += 8;
         stat_flag &= ~SSH_FILEXFER_ATTR_CTIME;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_SUBSECOND_TIMES)
      {
         pos += 4;
         stat_flag &= ~SSH_FILEXFER_ATTR_SUBSECOND_TIMES;
      }
      if (stat_flag & SSH_FILEXFER_ATTR_BITS)
      {
         pos += 4;
         stat_flag &= ~SSH_FILEXFER_ATTR_BITS;
      }
   }
   if (stat_flag != 0)
   {
      trans_log(DEBUG_SIGN, __FILE__, __LINE__, "store_attributes", NULL,
                _("Attribute flag still contains unaccounted flags : %u"),
                stat_flag);
   }

   return(pos);
}


/*++++++++++++++++++++++++++ error_2_str() ++++++++++++++++++++++++++++++*/
static char *
error_2_str(char *msg)
{
   unsigned int error_code;

   error_code = get_xfer_uint(msg);
   switch (error_code)
   {
      case SSH_FX_OK                          : /*  0 */
         return(_("SSH_FX_OK: No error. (0)"));
      case SSH_FX_EOF                         : /*  1 */
         return(_("SSH_FX_EOF: Attempted to read past the end-of-file or there are no more directory entries. (1)"));
      case SSH_FX_NO_SUCH_FILE                : /*  2 */
         return(_("SSH_FX_NO_SUCH_FILE: A reference was made to a file which does not exist. (2)"));
      case SSH_FX_PERMISSION_DENIED           : /*  3 */
         return(_("SSH_FX_PERMISSION_DENIED: Permission denied. (3)"));
      case SSH_FX_FAILURE                     : /*  4 */
         return(_("SSH_FX_FAILURE: An error occurred, but no specific error code exists to describe the failure. (4)"));
      case SSH_FX_BAD_MESSAGE                 : /*  5 */
         return(_("SSH_FX_BAD_MESSAGE: A badly formatted packet or other SFTP protocol incompatibility was detected. (5)"));
      case SSH_FX_NO_CONNECTION               : /*  6 */
         return(_("SSH_FX_NO_CONNECTION: There is no connection to the server. (6)"));
      case SSH_FX_CONNECTION_LOST             : /*  7 */
         return(_("SSH_FX_CONNECTION_LOST: The connection to the server was lost. (7)"));
      case SSH_FX_OP_UNSUPPORTED              : /*  8 */
         return(_("SSH_FX_OP_UNSUPPORTED: Operation unsupported. (8)"));
      case SSH_FX_INVALID_HANDLE              : /*  9 */
         return(_("SSH_FX_INVALID_HANDLE: The handle value was invalid. (9)"));
      case SSH_FX_NO_SUCH_PATH                : /* 10 */
         return(_("SSH_FX_NO_SUCH_PATH: File path does not exist or is invalid. (10)"));
      case SSH_FX_FILE_ALREADY_EXISTS         : /* 11 */
         return(_("SSH_FX_FILE_ALREADY_EXISTS: File already exists. (11)"));
      case SSH_FX_WRITE_PROTECT               : /* 12 */
         return(_("SSH_FX_WRITE_PROTECT: File is on read-only media, or the media is write protected. (12)"));
      case SSH_FX_NO_MEDIA                    : /* 13 */
         return(_("SSH_FX_NO_MEDIA: The requested operation cannot be completed because there is no media available in the drive. (13)"));
      case SSH_FX_NO_SPACE_ON_FILESYSTEM      : /* 14 */
         return(_("SSH_FX_NO_SPACE_ON_FILESYSTEM: No space on filesystem. (14)"));
      case SSH_FX_QUOTA_EXCEEDED              : /* 15 */
         return(_("SSH_FX_QUOTA_EXCEEDED: Quota exceeded. (15)"));
      case SSH_FX_UNKNOWN_PRINCIPAL           : /* 16 */
         return(_("SSH_FX_UNKNOWN_PRINCIPAL: Unknown principal. (16)"));
      case SSH_FX_LOCK_CONFLICT               : /* 17 */
         return(_("SSH_FX_LOCK_CONFLICT: File could not be opened because it is locked by another process. (17)"));
      case SSH_FX_DIR_NOT_EMPTY               : /* 18 */
         return(_("SSH_FX_DIR_NOT_EMPTY: Directory is not empty. (18)"));
      case SSH_FX_NOT_A_DIRECTORY             : /* 19 */
         return(_("SSH_FX_NOT_A_DIRECTORY: The specified file is not a directory. (19)"));
      case SSH_FX_INVALID_FILENAME            : /* 20 */
         return(_("SSH_FX_INVALID_FILENAME: Invalid filename. (20)"));
      case SSH_FX_LINK_LOOP                   : /* 21 */
         return(_("SSH_FX_LINK_LOOP: Too many symbolic links encountered. (21)"));
      case SSH_FX_CANNOT_DELETE               : /* 22 */
         return(_("SSH_FX_CANNOT_DELETE: File cannot be deleted. (22)"));
      case SSH_FX_INVALID_PARAMETER           : /* 23 */
         return(_("SSH_FX_INVALID_PARAMETER: Invalid parameter. (23)"));
      case SSH_FX_FILE_IS_A_DIRECTORY         : /* 24 */
         return(_("SSH_FX_FILE_IS_A_DIRECTORY: File is a directory. (24)"));
      case SSH_FX_BYTE_RANGE_LOCK_CONFLICT    : /* 25 */
         return(_("SSH_FX_BYTE_RANGE_LOCK_CONFLICT: Byte range lock conflict. (25)"));
      case SSH_FX_BYTE_RANGE_LOCK_REFUSED     : /* 26 */
         return(_("SSH_FX_BYTE_RANGE_LOCK_REFUSED: Byte range lock refused. (26)"));
      case SSH_FX_DELETE_PENDING              : /* 27 */
         return(_("SSH_FX_DELETE_PENDING: Delete is pending. (27)"));
      case SSH_FX_FILE_CORRUPT                : /* 28 */
         return(_("SSH_FX_FILE_CORRUPT: File is corrupt. (28)"));
      case SSH_FX_OWNER_INVALID               : /* 29 */
         return(_("SSH_FX_OWNER_INVALID: Invalid owner. (29)"));
      case SSH_FX_GROUP_INVALID               : /* 30 */
         return(_("SSH_FX_GROUP_INVALID: Invalid group. (30)"));
      case SSH_FX_NO_MATCHING_BYTE_RANGE_LOCK : /* 31 */
         return(_("SSH_FX_NO_MATCHING_BYTE_RANGE_LOCK: Requested operation could not be completed, because byte range lock has not been granted. (31)"));
      default                                 : /* ?? */
         (void)snprintf(msg_str, MAX_PATH_LENGTH,
                        _("Unknown error code. (%u)"), error_code);
         return(msg_str);
   }
}


/*+++++++++++++++++++++++++ response_2_str() ++++++++++++++++++++++++++++*/
static char *
response_2_str(char response_type)
{
   switch (response_type)
   {
      case SSH_FXP_STATUS  : return("SSH_FXP_STATUS");
      case SSH_FXP_HANDLE  : return("SSH_FXP_HANDLE");
      case SSH_FXP_DATA    : return("SSH_FXP_DATA");
      case SSH_FXP_NAME    : return("SSH_FXP_NAME");
      case SSH_FXP_ATTRS   : return("SSH_FXP_ATTRS");
      case SSH_FXP_VERSION : return("SSH_FXP_VERSION");
      default              : return("Unknown response");
   }
}


/*+++++++++++++++++++++++++++ is_with_path() ++++++++++++++++++++++++++++*/
static int
is_with_path(char *name)
{
   while (*name != '\0')
   {
      if (*name == '/')
      {
         return(YES);
      }
      name++;
   }

   return(NO);
}


/*++++++++++++++++++++++++++++ sig_handler() ++++++++++++++++++++++++++++*/
static void
sig_handler(int signo)
{
   siglongjmp(env_alrm, 1);
}
