/*
 *  remove_host.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 1998 - 2017 Deutscher Wetterdienst (DWD),
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

DESCR__S_M3
/*
 ** NAME
 **   remove_host - removes a host from the HOST_CONFIG file
 **
 ** SYNOPSIS
 **   int remove_host(char *host_name, int is_group_name)
 **
 ** DESCRIPTION
 **   The function remove_host() removes any NNN files created via
 **   the assemble() and convert() option and removes the host from
 **   the HOST_CONFIG file.
 **
 ** RETURN VALUES
 **   SUCCESS when the host host_name has been remove from the
 **   HOST_CONFIG file, otherwise INCORRECT will be returned.
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   22.10.1998 H.Kiehl Created
 **   19.08.2013 H.Kiehl Added removing of NNN files.
 **   20.03.2017 H.Kiehl Add support for deleting group names.
 **
 */
DESCR__E_M3

#include <stdio.h>
#include <string.h>
#include <stdlib.h>            /* free()                                 */
#include <unistd.h>            /* write(), close(), unlink()             */
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <Xm/Xm.h>
#include "mafd_ctrl.h"
#include "edit_hc.h"

/* External global variables. */
extern int  sys_log_fd;
extern char *p_work_dir;


/*############################ remove_host() ############################*/
int
remove_host(char *host_name, int is_group_name)
{
   int  fd,
        length;
   char *file_buffer,
        host_config_file[MAX_PATH_LENGTH],
        *ptr,
        *ptr_start,
        search_string[MAX_HOSTNAME_LENGTH + 3];

   if (is_group_name == NO)
   {
      /* First remove any nnn counter files for this host. */
      remove_nnn_files(get_str_checksum(host_name));
   }

   (void)sprintf(host_config_file, "%s%s%s",
                 p_work_dir, ETC_DIR, DEFAULT_HOST_CONFIG_FILE);

   if (read_file_no_cr(host_config_file, &file_buffer, YES, __FILE__, __LINE__) == INCORRECT)
   {
      char tmp_file[256];

      (void)strcpy(tmp_file, DEFAULT_HOST_CONFIG_FILE);
      (void)xrec(ERROR_DIALOG,
                 "Failed to read %s! Thus unable to remove host %s",
                 &tmp_file[1], host_name);
      return(INCORRECT);
   }
   (void)strcpy(&search_string[1], host_name);
   length = strlen(host_name);
   if (is_group_name == NO)
   {
      search_string[1 + length] = ':';
   }
   else
   {
      search_string[1 + length] = '\n';
   }
   search_string[1 + length + 1] = '\0';
   search_string[0] = '\n';

   if ((ptr = posi(file_buffer, search_string)) == NULL)
   {
      char tmp_file[256];

      (void)strcpy(tmp_file, DEFAULT_HOST_CONFIG_FILE);
      (void)xrec(ERROR_DIALOG,
                 "Failed to locate %s in %s, thus unable to remove host.",
                 host_name, &tmp_file[1]);
      free(file_buffer);
      return(INCORRECT);
   }
   ptr_start = ptr - (length + 1); /* + 1 => ':' or '\n' */
   if (is_group_name == YES)
   {
      ptr -= 2;
   }
   while ((*ptr != '\n') && (*ptr != '\0'))
   {
      ptr++;
   }
   if (*ptr == '\0')
   {
      *(ptr_start - 1) = '\0';
   }
   else
   {
      ptr++;
      if ((*ptr == '\0') || (*ptr == '\n'))
      {
         *(ptr_start - 1) = '\0';
      }
      else
      {
         (void)memmove((ptr_start - 1), ptr, strlen(ptr) + 1);
      }
   }

   if ((fd = open(host_config_file, (O_RDWR | O_CREAT | O_TRUNC),
#ifdef GROUP_CAN_WRITE
                  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP))) == -1)
#else
                  (S_IRUSR | S_IWUSR))) == -1)
#endif
   {
      char tmp_file[256];

      (void)strcpy(tmp_file, DEFAULT_HOST_CONFIG_FILE);
      (void)xrec(ERROR_DIALOG,
                 "Failed to open %s, thus unable to remove host %s : %s (%s %d)",
                 &tmp_file[1], host_name, strerror(errno), __FILE__, __LINE__);
      free(file_buffer);
      return(INCORRECT);
   }
   length = strlen(file_buffer) - 1;
   if (writen(fd, file_buffer + 1, length, 0) != length)
   {
      char tmp_file[256];

      (void)strcpy(tmp_file, DEFAULT_HOST_CONFIG_FILE);
      (void)xrec(ERROR_DIALOG,
                 "Failed to write to %s, thus unable to remove host %s : %s (%s %d)!",
                 &tmp_file[1], host_name, strerror(errno), __FILE__, __LINE__);
      (void)close(fd);
      free(file_buffer);
      return(INCORRECT);
   }
   if (close(fd) == -1)
   {
      (void)rec(sys_log_fd, DEBUG_SIGN,
                "close() error : %s (%s %d)\n",
                strerror(errno), __FILE__, __LINE__);
   }

   free(file_buffer);
#ifdef WITH_DUP_CHECK
   (void)sprintf(host_config_file, "%s%s%s/%u",
                 p_work_dir, AFD_FILE_DIR, CRC_DIR,
                 get_str_checksum(host_name));
   (void)unlink(host_config_file);
#endif

   return(SUCCESS);
}
