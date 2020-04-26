/*
 *  show_ilog.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 1997 - 2020 Holger Kiehl <Holger.Kiehl@dwd.de>
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
 **   show_ilog - displays the input log file from the AFD
 **
 ** SYNOPSIS
 **   show_ilog [--version]
 **                OR
 **   show_ilog [-w <AFD working directory>] [fontname] [hostname 1..n]
 **
 ** DESCRIPTION
 **
 ** RETURN VALUES
 **
 ** AUTHOR
 **   H.Kiehl
 **
 ** HISTORY
 **   26.05.1997 H.Kiehl Created
 **   15.02.1999 H.Kiehl Multiple recipients.
 **   23.11.2003 H.Kiehl Disallow user to change window width even if
 **                      window manager allows this, but allow to change
 **                      height.
 **
 */
DESCR__E_M1

#include <stdio.h>
#include <string.h>            /* strcpy(), strcat(), strerror()         */
#include <ctype.h>             /* toupper()                              */
#include <signal.h>            /* signal()                               */
#include <stdlib.h>            /* malloc(), free()                       */
#ifdef HAVE_SETPRIORITY
# include <sys/time.h>
# include <sys/resource.h>     /* setpriority()                          */
#endif
#include <sys/types.h>
#include <unistd.h>            /* gethostname()                          */
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include <Xm/Xm.h>
#include <Xm/Text.h>
#include <Xm/List.h>
#include <Xm/ToggleBG.h>
#include <Xm/PushB.h>
#include <Xm/PushBG.h>
#include <Xm/Label.h>
#include <Xm/LabelG.h>
#include <Xm/Separator.h>
#include <Xm/RowColumn.h>
#include <Xm/Form.h>
#include "mafd_ctrl.h"
#include "show_ilog.h"
#include "logdefs.h"
#include "permission.h"
#include "version.h"
#ifdef WITH_EDITRES
# include <X11/Xmu/Editres.h>
#endif

/* Global variables. */
Display                    *display;
XtAppContext               app;
Widget                     appshell,
                           cont_togglebox_w,
                           start_time_w,
                           end_time_w,
                           file_name_w,
                           directory_w,
                           file_length_w,
                           recipient_w,
                           headingbox_w,
                           listbox_w,
                           print_button_w,
                           radiobox_w,
                           scrollbar_w,
                           select_all_button_w,
                           statusbox_w,
                           summarybox_w,
                           special_button_w;
Window                     main_window;
XmFontList                 fontlist;
int                        acd_counter = 0,
                           char_width,
                           continues_toggle_set,
                           file_name_length,
                           fra_fd = -1,
                           fra_id,
                           log_date_length = LOG_DATE_LENGTH,
                           max_hostname_length = MAX_HOSTNAME_LENGTH, /* not used */
                           max_input_log_files = MAX_INPUT_LOG_FILES,
                           no_of_dirs = 0,
                           no_of_log_files,
                           no_of_search_hosts,
                           no_of_search_dirs,
                           no_of_search_dirids,
                           *search_dir_length = NULL,
                           special_button_flag,
                           sum_line_length,
                           sys_log_fd = STDERR_FILENO;
unsigned int               all_list_items = 0,
                           *search_dirid = NULL;
#ifdef HAVE_MMAP
off_t                      fra_size;
#endif
Dimension                  button_height;
time_t                     start_time_val,
                           end_time_val;
size_t                     search_file_size;
char                       *p_work_dir,
                           font_name[40],
                           header_line[MAX_OUTPUT_LINE_LENGTH + SHOW_LONG_FORMAT + 1],
                           search_file_name[MAX_PATH_LENGTH],
                           **search_dir = NULL,
                           *search_dir_filter = NULL,
                           **search_recipient,
                           **search_user;
struct item_list           *il = NULL;
struct alda_call_data      *acd = NULL;
struct sol_perm            perm;
struct fileretrieve_status *fra = NULL;
const char                 *sys_log_name = SYSTEM_LOG_FIFO;

/* Local function prototypes. */
static void                eval_permissions(char *),
#ifdef HAVE_SETPRIORITY
                           get_afd_config_value(void),
#endif
                           init_show_ilog(int *, char **, char *),
                           sig_bus(int),
                           sig_segv(int),
                           usage(char *);


/*$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$ main() $$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$$*/
int
main(int argc, char *argv[])
{
   char            window_title[MAX_WNINDOW_TITLE_LENGTH],
                   work_dir[MAX_PATH_LENGTH],
                   *radio_label[] = { "Short", "Long" };
   static String   fallback_res[] =
                   {
                      ".show_ilog*background : NavajoWhite2",
                      ".show_ilog.mainform*background : NavajoWhite2",
                      ".show_ilog.mainform*XmText.background : NavajoWhite1",
                      ".show_ilog.mainform*listbox.background : NavajoWhite1",
                      ".show_ilog.mainform.buttonbox*background : PaleVioletRed2",
                      ".show_ilog.mainform.buttonbox*foreground : Black",
                      ".show_ilog.mainform.buttonbox*highlightColor : Black",
                      ".show_ilog.show_info*mwmDecorations : 10",
                      ".show_ilog.show_info*mwmFunctions : 4",
                      ".show_ilog.show_info*background : NavajoWhite2",
                      ".show_ilog.show_info*XmText.background : NavajoWhite1",
                      ".show_ilog.show_info.infoform.buttonbox*background : PaleVioletRed2",
                      ".show_ilog.show_info.infoform.buttonbox*foreground : Black",
                      ".show_ilog.show_info.infoform.buttonbox*highlightColor : Black",
                      ".show_ilog.Print Data*background : NavajoWhite2",
                      ".show_ilog.Print Data*XmText.background : NavajoWhite1",
                      ".show_ilog.Print Data.main_form.buttonbox*background : PaleVioletRed2",
                      ".show_ilog.Print Data.main_form.buttonbox*foreground : Black",
                      ".show_ilog.Print Data.main_form.buttonbox*highlightColor : Black",
                      NULL
                   };
   Widget          block_w,
                   buttonbox_w,
                   button_w,
                   criteriabox_w,
                   currenttime_w,
                   entertime_w,
                   label_w,
                   mainform_w,
                   radio_w,
                   rowcol_w,
                   separator_w,
                   timebox_w,
                   toggle_w;
   Arg             args[MAXARGS];
   Cardinal        argcount;
   XmFontListEntry entry;
   XFontStruct     *font_struct;
   XmFontType      dummy;
   uid_t           euid, /* Effective user ID. */
                   ruid; /* Real user ID. */

   CHECK_FOR_VERSION(argc, argv);

   /* Initialise global values. */
   p_work_dir = work_dir;
   init_show_ilog(&argc, argv, window_title);
#ifdef HAVE_SETPRIORITY
   get_afd_config_value();
#endif

   /*
    * SSH uses wants to look at .Xauthority and with setuid flag
    * set we cannot do that. So when we initialize X lets temporaly
    * disable it. After XtAppInitialize() we set it back.
    */
   euid = geteuid();
   ruid = getuid();
   if (euid != ruid)
   {
      if (seteuid(ruid) == -1)
      {
         (void)fprintf(stderr, "Failed to seteuid() to %d : %s (%s %d)\n",
                       ruid, strerror(errno), __FILE__, __LINE__);
      }
   }

   argcount = 0;
   XtSetArg(args[argcount], XmNtitle, window_title); argcount++;
   appshell = XtAppInitialize(&app, "AFD", NULL, 0,
                              &argc, argv, fallback_res, args, argcount);
   disable_drag_drop(appshell);

   if (euid != ruid)
   {
      if (seteuid(euid) == -1)
      {
         (void)fprintf(stderr, "Failed to seteuid() to %d : %s (%s %d)\n",
                       euid, strerror(errno), __FILE__, __LINE__);
      }
   }

   display = XtDisplay(appshell);

#ifdef HAVE_XPM
   /* Setup AFD logo as icon. */
   setup_icon(display, appshell);
#endif

   /* Create managing widget. */
   mainform_w = XmCreateForm(appshell, "mainform", NULL, 0);

   /* Prepare font. */
   if ((entry = XmFontListEntryLoad(display, font_name,
                                    XmFONT_IS_FONT, "TAG1")) == NULL)
   {
       if ((entry = XmFontListEntryLoad(display, DEFAULT_FONT,
                                        XmFONT_IS_FONT, "TAG1")) == NULL)
       {
          (void)fprintf(stderr,
                        "Failed to load font with XmFontListEntryLoad() : %s (%s %d)\n",
                        strerror(errno), __FILE__, __LINE__);
          exit(INCORRECT);
       }
       else
       {
          (void)strcpy(font_name, DEFAULT_FONT);
       }
   }
   font_struct = (XFontStruct *)XmFontListEntryGetFont(entry, &dummy);
   char_width  = font_struct->per_char->width;
   fontlist = XmFontListAppendEntry(NULL, entry);
   XmFontListEntryFree(&entry);

/*-----------------------------------------------------------------------*/
/*                           Time Box                                    */
/*                           --------                                    */
/* Start and end time to search ouput log file. If no time is entered it */
/* means we should search through all log files.                         */
/*-----------------------------------------------------------------------*/
   /* Create managing widget to enter the time interval. */
   argcount = 0;
   XtSetArg(args[argcount], XmNtopAttachment,   XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,  XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment, XmATTACH_FORM);
   argcount++;
   timebox_w = XmCreateForm(mainform_w, "timebox", args, argcount);

   argcount = 0;
   XtSetArg(args[argcount], XmNtopAttachment,    XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_FORM);
   argcount++;
   entertime_w = XmCreateForm(timebox_w, "entertime", args, argcount);
   rowcol_w = XtVaCreateWidget("rowcol",
                           xmRowColumnWidgetClass, entertime_w,
                           XmNorientation,         XmHORIZONTAL,
                           NULL);
   block_w = XmCreateForm(rowcol_w, "rowcol", NULL, 0);
   label_w = XtVaCreateManagedWidget(" Start time :",
                           xmLabelGadgetClass,  block_w,
                           XmNfontList,         fontlist,
                           XmNtopAttachment,    XmATTACH_FORM,
                           XmNbottomAttachment, XmATTACH_FORM,
                           XmNleftAttachment,   XmATTACH_FORM,
                           XmNalignment,        XmALIGNMENT_END,
                           NULL);
   start_time_w = XtVaCreateManagedWidget("starttime",
                           xmTextWidgetClass,   block_w,
                           XmNfontList,         fontlist,
                           XmNmarginHeight,     1,
                           XmNmarginWidth,      1,
                           XmNshadowThickness,  1,
                           XmNtopAttachment,    XmATTACH_FORM,
                           XmNbottomAttachment, XmATTACH_FORM,
                           XmNrightAttachment,  XmATTACH_FORM,
                           XmNleftAttachment,   XmATTACH_WIDGET,
                           XmNleftWidget,       label_w,
                           XmNcolumns,          8,
                           XmNmaxLength,        8,
                           NULL);
   XtAddCallback(start_time_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)START_TIME_NO_ENTER);
   XtAddCallback(start_time_w, XmNactivateCallback, save_input,
                 (XtPointer)START_TIME);
   XtManageChild(block_w);

   block_w = XmCreateForm(rowcol_w, "rowcol", NULL, 0);
   label_w = XtVaCreateManagedWidget("End time :",
                           xmLabelGadgetClass,  block_w,
                           XmNfontList,         fontlist,
                           XmNtopAttachment,    XmATTACH_FORM,
                           XmNbottomAttachment, XmATTACH_FORM,
                           XmNleftAttachment,   XmATTACH_FORM,
                           XmNalignment,        XmALIGNMENT_END,
                           NULL);
   end_time_w = XtVaCreateManagedWidget("endtime",
                           xmTextWidgetClass,   block_w,
                           XmNfontList,         fontlist,
                           XmNmarginHeight,     1,
                           XmNmarginWidth,      1,
                           XmNshadowThickness,  1,
                           XmNtopAttachment,    XmATTACH_FORM,
                           XmNbottomAttachment, XmATTACH_FORM,
                           XmNrightAttachment,  XmATTACH_FORM,
                           XmNleftAttachment,   XmATTACH_WIDGET,
                           XmNleftWidget,       label_w,
                           XmNcolumns,          8,
                           XmNmaxLength,        8,
                           NULL);
   XtAddCallback(end_time_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)END_TIME_NO_ENTER);
   XtAddCallback(end_time_w, XmNactivateCallback, save_input,
                 (XtPointer)END_TIME);
   XtManageChild(block_w);
   XtManageChild(rowcol_w);
   XtManageChild(entertime_w);

/*-----------------------------------------------------------------------*/
/*                          Vertical Separator                           */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,      XmVERTICAL);
   argcount++;
   XtSetArg(args[argcount], XmNtopAttachment,    XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNleftWidget,       entertime_w);
   argcount++;
   separator_w = XmCreateSeparator(timebox_w, "separator", args, argcount);
   XtManageChild(separator_w);

/*-----------------------------------------------------------------------*/
/*                        Continues Toggle Box                           */
/*                        --------------------                           */
/* Let user select the if he wants toe run this dialog in continues mode.*/
/*-----------------------------------------------------------------------*/
   cont_togglebox_w = XtVaCreateWidget("cont_togglebox",
                                xmRowColumnWidgetClass, timebox_w,
                                XmNorientation,      XmHORIZONTAL,
                                XmNpacking,          XmPACK_TIGHT,
                                XmNnumColumns,       1,
                                XmNtopAttachment,    XmATTACH_FORM,
                                XmNleftAttachment,   XmATTACH_WIDGET,
                                XmNleftWidget,       separator_w,
                                XmNbottomAttachment, XmATTACH_FORM,
                                XmNresizable,        False,
                                NULL);

   toggle_w = XtVaCreateManagedWidget("Cont. ",
                                xmToggleButtonGadgetClass, cont_togglebox_w,
                                XmNfontList,               fontlist,
                                XmNset,                    False,
                                NULL);
   XtAddCallback(toggle_w, XmNvalueChangedCallback,
                 (XtCallbackProc)continues_toggle, NULL);
   continues_toggle_set = NO;
   XtManageChild(cont_togglebox_w);

/*-----------------------------------------------------------------------*/
/*                          Vertical Separator                           */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,      XmVERTICAL);
   argcount++;
   XtSetArg(args[argcount], XmNtopAttachment,    XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNleftWidget,       cont_togglebox_w);
   argcount++;
   separator_w = XmCreateSeparator(timebox_w, "separator", args, argcount);
   XtManageChild(separator_w);

   currenttime_w = XtVaCreateManagedWidget("",
                           xmLabelWidgetClass,  timebox_w,
                           XmNfontList,         fontlist,
                           XmNtopAttachment,    XmATTACH_FORM,
                           XmNbottomAttachment, XmATTACH_FORM,
                           XmNrightAttachment,  XmATTACH_FORM,
                           XmNrightOffset,      10,
                           NULL);
   XtManageChild(timebox_w);

/*-----------------------------------------------------------------------*/
/*                         Horizontal Separator                          */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,     XmHORIZONTAL);
   argcount++;
   XtSetArg(args[argcount], XmNtopAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNtopWidget,       timebox_w);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,  XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment, XmATTACH_FORM);
   argcount++;
   separator_w = XmCreateSeparator(mainform_w, "separator", args, argcount);
   XtManageChild(separator_w);

/*-----------------------------------------------------------------------*/
/*                          Criteria Box                                 */
/*                          ------------                                 */
/* Here more search parameters can be entered, such as: file name,       */
/* size of the file, directory from which the file had its origion,      */
/* recipient of the file.                                                */
/*-----------------------------------------------------------------------*/
   /* Create managing widget for other criteria. */
   argcount = 0;
   XtSetArg(args[argcount], XmNtopAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNtopWidget,       separator_w);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,  XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment, XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNfractionBase,    49);
   argcount++;
   criteriabox_w = XmCreateForm(mainform_w, "criteriabox", args, argcount);

   label_w = XtVaCreateManagedWidget("File name :",
                           xmLabelGadgetClass,  criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      1,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   24,
                           XmNleftAttachment,   XmATTACH_POSITION,
                           XmNleftPosition,     0,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    8,
                           XmNalignment,        XmALIGNMENT_END,
                           NULL);
   file_name_w = XtVaCreateManagedWidget("",
                           xmTextWidgetClass,   criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNmarginHeight,     1,
                           XmNmarginWidth,      1,
                           XmNshadowThickness,  1,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      1,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   24,
                           XmNleftAttachment,   XmATTACH_WIDGET,
                           XmNleftWidget,       label_w,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    29,
                           NULL);
   XtAddCallback(file_name_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)FILE_NAME_NO_ENTER);
   XtAddCallback(file_name_w, XmNactivateCallback, save_input,
                 (XtPointer)FILE_NAME);

   label_w = XtVaCreateManagedWidget("Directory :",
                           xmLabelGadgetClass,  criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNalignment,        XmALIGNMENT_END,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      25,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   48,
                           XmNleftAttachment,   XmATTACH_POSITION,
                           XmNleftPosition,     0,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    8,
                           NULL);
   directory_w = XtVaCreateManagedWidget("",
                           xmTextWidgetClass,   criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNmarginHeight,     1,
                           XmNmarginWidth,      1,
                           XmNshadowThickness,  1,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      25,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   48,
                           XmNleftAttachment,   XmATTACH_WIDGET,
                           XmNleftWidget,       label_w,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    29,
                           NULL);
   XtAddCallback(directory_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)DIRECTORY_NAME_NO_ENTER);
   XtAddCallback(directory_w, XmNactivateCallback, save_input,
                 (XtPointer)DIRECTORY_NAME);

   label_w = XtVaCreateManagedWidget("File size :",
                           xmLabelGadgetClass,  criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNalignment,        XmALIGNMENT_END,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      1,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   24,
                           XmNleftAttachment,   XmATTACH_POSITION,
                           XmNleftPosition,     26,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    36,
                           NULL);
   file_length_w = XtVaCreateManagedWidget("",
                           xmTextWidgetClass,   criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNmarginHeight,     1,
                           XmNmarginWidth,      1,
                           XmNshadowThickness,  1,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      1,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   24,
                           XmNleftAttachment,   XmATTACH_WIDGET,
                           XmNleftWidget,       label_w,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    48,
                           NULL);
   XtAddCallback(file_length_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)FILE_LENGTH_NO_ENTER);
   XtAddCallback(file_length_w, XmNactivateCallback, save_input,
                 (XtPointer)FILE_LENGTH);

   label_w = XtVaCreateManagedWidget("Recipient :",
                           xmLabelGadgetClass,  criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNalignment,        XmALIGNMENT_END,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      25,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   48,
                           XmNleftAttachment,   XmATTACH_POSITION,
                           XmNleftPosition,     26,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    36,
                           NULL);
   recipient_w = XtVaCreateManagedWidget("",
                           xmTextWidgetClass,   criteriabox_w,
                           XmNfontList,         fontlist,
                           XmNmarginHeight,     1,
                           XmNmarginWidth,      1,
                           XmNshadowThickness,  1,
                           XmNtopAttachment,    XmATTACH_POSITION,
                           XmNtopPosition,      25,
                           XmNbottomAttachment, XmATTACH_POSITION,
                           XmNbottomPosition,   48,
                           XmNleftAttachment,   XmATTACH_WIDGET,
                           XmNleftWidget,       label_w,
                           XmNrightAttachment,  XmATTACH_POSITION,
                           XmNrightPosition,    48,
                           NULL);
   XtAddCallback(recipient_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)RECIPIENT_NAME_NO_ENTER);
   XtAddCallback(recipient_w, XmNactivateCallback, save_input,
                 (XtPointer)RECIPIENT_NAME);
   XtManageChild(criteriabox_w);

/*-----------------------------------------------------------------------*/
/*                         Horizontal Separator                          */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,     XmHORIZONTAL);
   argcount++;
   XtSetArg(args[argcount], XmNtopAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNtopWidget,       criteriabox_w);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,  XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment, XmATTACH_FORM);
   argcount++;
   separator_w = XmCreateSeparator(mainform_w, "separator", args, argcount);
   XtManageChild(separator_w);

/*-----------------------------------------------------------------------*/
/*                             Radio Box                                 */
/*                             ---------                                 */
/* To select if the output in the list widget should be in long or short */
/* format. Default is short, since this is the fastest form.             */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNtopAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNtopWidget,       separator_w);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment, XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNorientation,     XmHORIZONTAL);
   argcount++;
   XtSetArg(args[argcount], XmNpacking,         XmPACK_COLUMN);
   argcount++;
   XtSetArg(args[argcount], XmNnumColumns,      1);
   argcount++;
   radiobox_w = XmCreateRadioBox(mainform_w, "radiobox", args, argcount);
   radio_w = XtVaCreateManagedWidget(radio_label[0],
                           xmToggleButtonGadgetClass, radiobox_w,
                           XmNfontList,               fontlist,
                           XmNset,                    True,
                           NULL);
   XtAddCallback(radio_w, XmNdisarmCallback,
                 (XtCallbackProc)radio_button, (XtPointer)SHOW_SHORT_FORMAT);
   radio_w = XtVaCreateManagedWidget(radio_label[1],
                           xmToggleButtonGadgetClass, radiobox_w,
                           XmNfontList,               fontlist,
                           XmNset,                    False,
                           NULL);
   XtAddCallback(radio_w, XmNdisarmCallback,
                 (XtCallbackProc)radio_button, (XtPointer)SHOW_LONG_FORMAT);
   XtManageChild(radiobox_w);
   file_name_length = SHOW_SHORT_FORMAT;

/*-----------------------------------------------------------------------*/
/*                         Horizontal Separator                          */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,     XmHORIZONTAL);
   argcount++;
   XtSetArg(args[argcount], XmNtopAttachment,   XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNtopWidget,       radiobox_w);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,  XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment, XmATTACH_FORM);
   argcount++;
   separator_w = XmCreateSeparator(mainform_w, "separator", args, argcount);
   XtManageChild(separator_w);

/*-----------------------------------------------------------------------*/
/*                           Heading Box                                 */
/*                           -----------                                 */
/* Shows a heading for the list box.                                     */
/*-----------------------------------------------------------------------*/
   headingbox_w = XtVaCreateWidget("headingbox",
                           xmTextWidgetClass,        mainform_w,
                           XmNfontList,              fontlist,
                           XmNleftAttachment,        XmATTACH_FORM,
                           XmNleftOffset,            2,
                           XmNrightAttachment,       XmATTACH_FORM,
                           XmNrightOffset,           20,
                           XmNtopAttachment,         XmATTACH_WIDGET,
                           XmNtopWidget,             separator_w,
                           XmNmarginHeight,          1,
                           XmNmarginWidth,           2,
                           XmNshadowThickness,       1,
                           XmNrows,                  1,
                           XmNeditable,              False,
                           XmNcursorPositionVisible, False,
                           XmNhighlightThickness,    0,
                           XmNcolumns,               MAX_OUTPUT_LINE_LENGTH + file_name_length + 1,
                           NULL);
   XtManageChild(headingbox_w);

/*-----------------------------------------------------------------------*/
/*                            Button Box                                 */
/*                            ----------                                 */
/* The status of the input log is shown here. If eg. no files are found  */
/* it will be shown here.                                                */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment,  XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNfractionBase, 41);
   argcount++;
   buttonbox_w = XmCreateForm(mainform_w, "buttonbox", args, argcount);
   special_button_w = XtVaCreateManagedWidget("Search",
                        xmPushButtonWidgetClass, buttonbox_w,
                        XmNfontList,             fontlist,
                        XmNtopAttachment,        XmATTACH_POSITION,
                        XmNtopPosition,          1,
                        XmNleftAttachment,       XmATTACH_POSITION,
                        XmNleftPosition,         1,
                        XmNrightAttachment,      XmATTACH_POSITION,
                        XmNrightPosition,        10,
                        XmNbottomAttachment,     XmATTACH_POSITION,
                        XmNbottomPosition,       40,
                        NULL);
   XtAddCallback(special_button_w, XmNactivateCallback,
                 (XtCallbackProc)search_button, (XtPointer)0);
   select_all_button_w = XtVaCreateManagedWidget("Select All",
                        xmPushButtonWidgetClass, buttonbox_w,
                        XmNfontList,             fontlist,
                        XmNtopAttachment,        XmATTACH_POSITION,
                        XmNtopPosition,          1,                
                        XmNleftAttachment,       XmATTACH_POSITION,
                        XmNleftPosition,         11,
                        XmNrightAttachment,      XmATTACH_POSITION,
                        XmNrightPosition,        20,
                        XmNbottomAttachment,     XmATTACH_POSITION,
                        XmNbottomPosition,       40,
                        NULL);
      XtAddCallback(select_all_button_w, XmNactivateCallback,
                    (XtCallbackProc)select_all_button, (XtPointer)0);
   print_button_w = XtVaCreateManagedWidget("Print",
                        xmPushButtonWidgetClass, buttonbox_w,
                        XmNfontList,             fontlist,
                        XmNtopAttachment,        XmATTACH_POSITION,
                        XmNtopPosition,          1,
                        XmNleftAttachment,       XmATTACH_POSITION,
                        XmNleftPosition,         21,
                        XmNrightAttachment,      XmATTACH_POSITION,
                        XmNrightPosition,        30,
                        XmNbottomAttachment,     XmATTACH_POSITION,
                        XmNbottomPosition,       40,
                        NULL);
   XtAddCallback(print_button_w, XmNactivateCallback,
                 (XtCallbackProc)print_button, (XtPointer)0);
   button_w = XtVaCreateManagedWidget("Close",
                        xmPushButtonWidgetClass, buttonbox_w,
                        XmNfontList,             fontlist,
                        XmNtopAttachment,        XmATTACH_POSITION,
                        XmNtopPosition,          1,
                        XmNleftAttachment,       XmATTACH_POSITION,
                        XmNleftPosition,         31,
                        XmNrightAttachment,      XmATTACH_POSITION,
                        XmNrightPosition,        40,
                        XmNbottomAttachment,     XmATTACH_POSITION,
                        XmNbottomPosition,       40,
                        NULL);
   XtAddCallback(button_w, XmNactivateCallback,
                 (XtCallbackProc)close_button, (XtPointer)0);
   XtManageChild(buttonbox_w);

/*-----------------------------------------------------------------------*/
/*                         Horizontal Separator                          */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,      XmHORIZONTAL);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNbottomWidget,     buttonbox_w);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment,  XmATTACH_FORM);
   argcount++;
   separator_w = XmCreateSeparator(mainform_w, "separator", args, argcount);
   XtManageChild(separator_w);

/*-----------------------------------------------------------------------*/
/*                            Status Box                                 */
/*                            ----------                                 */
/* The status of the input log is shown here. If eg. no files are found  */
/* it will be shown here.                                                */
/*-----------------------------------------------------------------------*/
   statusbox_w = XtVaCreateManagedWidget(" ",
                        xmLabelWidgetClass,  mainform_w,
                        XmNfontList,         fontlist,
                        XmNleftAttachment,   XmATTACH_FORM,
                        XmNrightAttachment,  XmATTACH_FORM,
                        XmNbottomAttachment, XmATTACH_WIDGET,
                        XmNbottomWidget,     separator_w,
                        NULL);

/*-----------------------------------------------------------------------*/
/*                         Horizontal Separator                          */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNorientation,      XmHORIZONTAL);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNbottomWidget,     statusbox_w);
   argcount++;
   XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment,  XmATTACH_FORM);
   argcount++;
   separator_w = XmCreateSeparator(mainform_w, "separator", args, argcount);
   XtManageChild(separator_w);

/*-----------------------------------------------------------------------*/
/*                           Summary Box                                 */
/*                           -----------                                 */
/* Summary of what has been selected. If none is slected in listbox a    */
/* summary of all items is made.                                         */
/*-----------------------------------------------------------------------*/
   summarybox_w = XtVaCreateManagedWidget(" ",
                           xmLabelWidgetClass,  mainform_w,
                           XmNfontList,         fontlist,
                           XmNleftAttachment,   XmATTACH_FORM,
                           XmNleftOffset,       3,
                           XmNrightAttachment,  XmATTACH_FORM,
                           XmNbottomAttachment, XmATTACH_WIDGET,
                           XmNbottomWidget,     separator_w,
                           NULL);

/*-----------------------------------------------------------------------*/
/*                             List Box                                  */
/*                             --------                                  */
/* This scrolled list widget shows the contents of the input log,        */
/* either in short or long form. Default is short.                       */
/*-----------------------------------------------------------------------*/
   argcount = 0;
   XtSetArg(args[argcount], XmNleftAttachment,         XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNrightAttachment,        XmATTACH_FORM);
   argcount++;
   XtSetArg(args[argcount], XmNtopAttachment,          XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNtopWidget,              headingbox_w);
   argcount++;
   XtSetArg(args[argcount], XmNbottomAttachment,       XmATTACH_WIDGET);
   argcount++;
   XtSetArg(args[argcount], XmNbottomWidget,           summarybox_w);
   argcount++;
   XtSetArg(args[argcount], XmNvisibleItemCount,       NO_OF_VISIBLE_LINES);
   argcount++;
   XtSetArg(args[argcount], XmNselectionPolicy,        XmEXTENDED_SELECT);
   argcount++;
   XtSetArg(args[argcount], XmNscrollBarDisplayPolicy, XmSTATIC);
   argcount++;
   XtSetArg(args[argcount], XmNfontList,               fontlist);
   argcount++;
   listbox_w = XmCreateScrolledList(mainform_w, "listbox", args, argcount);
   XtManageChild(listbox_w);
   XtAddEventHandler(listbox_w, ButtonPressMask, False,
                     (XtEventHandler)info_click, (XtPointer)NULL);
   XtAddCallback(listbox_w, XmNextendedSelectionCallback,
                 (XtCallbackProc)item_selection, (XtPointer)0);
   XtManageChild(mainform_w);

   /* Disallow user to change window width. */
   XtVaSetValues(appshell,
                 XmNminWidth, char_width * (MAX_OUTPUT_LINE_LENGTH + file_name_length + 6),
                 XmNmaxWidth, char_width * (MAX_OUTPUT_LINE_LENGTH + file_name_length + 6),
                 NULL);

#ifdef WITH_EDITRES
   XtAddEventHandler(appshell, (EventMask)0, True,
                     _XEditResCheckMessages, NULL);
#endif

   /* Start clock. */
   update_time((XtPointer)currenttime_w, (XtIntervalId)NULL);

   /* Realize all widgets. */
   XtRealizeWidget(appshell);

   /* Set some signal handlers. */
   if ((signal(SIGBUS, sig_bus) == SIG_ERR) ||
       (signal(SIGSEGV, sig_segv) == SIG_ERR))
   {
      (void)xrec(WARN_DIALOG, "Failed to set signal handler's for %s : %s",
                 SHOW_ILOG, strerror(errno));
   }

   /* We want the keyboard focus on the start time. */
   XmProcessTraversal(start_time_w, XmTRAVERSE_CURRENT);

#ifdef _WITH_FANCY_TRAVERSE
   /*
    * Only now my we activate the losing focus callback. If we
    * do it earlier, the start time will always be filled with
    * the current time. This is NOT what we want.
    */
   XtAddCallback(start_time_w, XmNlosingFocusCallback, save_input,
                 (XtPointer)START_TIME);
#endif

   /* Get widget ID of the scrollbar. */
   XtVaGetValues(XtParent(listbox_w), XmNverticalScrollBar,
                 &scrollbar_w, NULL);
   XtAddCallback(scrollbar_w, XmNdragCallback,
                 (XtCallbackProc)scrollbar_moved, (XtPointer)0);
   XtVaGetValues(buttonbox_w, XmNheight, &button_height, NULL);

   /* Write heading. */
   sum_line_length = sprintf(header_line, "%s%-*s %s",
                             DATE_TIME_HEADER, file_name_length,
                             FILE_NAME_HEADER, REST_HEADER);
   XmTextSetString(headingbox_w, header_line);

   if ((no_of_search_dirs > 0) || (no_of_search_dirids > 0))
   {
      size_t length;
      char   *str;

      length = (no_of_search_dirs * MAX_PATH_LENGTH) +
               (no_of_search_dirids * (1 + MAX_INT_HEX_LENGTH + 2)) +
               ((no_of_search_dirs + no_of_search_dirids) * 2) + 1;
      if ((str = malloc(length)) != NULL)
      {
         int  i;
         char *ptr;

         length = 0;
         for (i = 0; i < no_of_search_dirs; i++)
         {
            length += sprintf(&str[length], "%s, ", search_dir[i]);
            search_dir_filter[i] = NO;
            ptr = str;
            while (*ptr != '\0')
            {
               if (((*ptr == '?') || (*ptr == '*') || (*ptr == '[')) &&
                   ((ptr == str) || (*(ptr - 1) != '\\')))
               {
                  search_dir_filter[i] = YES;
                  break;
               }
               ptr++;
            }
            if (search_dir_filter[i] == YES)
            {
               search_dir_length[i] = 0;
            }
            else                                            
            {
               search_dir_length[i] = strlen(search_dir[i]);
            }
         }
         for (i = 0; i < no_of_search_dirids; i++)
         {
            length += sprintf(&str[length], "#%x, ", search_dirid[i]);
         }
         str[length - 2] = '\0';
         XtVaSetValues(directory_w, XmNvalue, str, NULL);
         free(str);
      }
   }
   if (no_of_search_hosts > 0)
   {
      char *str;

      if ((str = malloc(MAX_RECIPIENT_LENGTH * no_of_search_hosts)) != NULL)
      {
         int length,
             i;

         length = sprintf(str, "%s", search_recipient[0]);
         for (i = 1; i < no_of_search_hosts; i++)
         {
            length += sprintf(&str[length], ", %s", search_recipient[i]);
         }
         XtVaSetValues(recipient_w, XmNvalue, str, NULL);
         free(str);
      }
   }

   /* Get Window for resizing the main window. */
   main_window = XtWindow(appshell);

   /* Start the main event-handling loop. */
   XtAppMainLoop(app);

   exit(SUCCESS);
}


/*+++++++++++++++++++++++++++ init_show_ilog() ++++++++++++++++++++++++++*/
static void
init_show_ilog(int *argc, char *argv[], char *window_title)
{
   char fake_user[MAX_FULL_USER_ID_LENGTH],
        *perm_buffer,
        profile[MAX_PROFILE_NAME_LENGTH + 1];

   if ((get_arg(argc, argv, "-?", NULL, 0) == SUCCESS) ||
       (get_arg(argc, argv, "-help", NULL, 0) == SUCCESS) ||
       (get_arg(argc, argv, "--help", NULL, 0) == SUCCESS))
   {
      usage(argv[0]);
      exit(SUCCESS);
   }
   if (get_afd_path(argc, argv, p_work_dir) < 0)
   {
      (void)fprintf(stderr,
                    "Failed to get working directory of AFD. (%s %d)\n",
                    __FILE__, __LINE__);
      exit(INCORRECT);
   }

   /* Check if title is specified. */
   if (get_arg(argc, argv, "-t", font_name, 40) == INCORRECT)
   {
      (void)strcpy(window_title, "Input Log ");
      if (get_afd_name(&window_title[10]) == INCORRECT)
      {
         if (gethostname(&window_title[10], MAX_AFD_NAME_LENGTH) == 0)
         {
            window_title[10] = toupper((int)window_title[10]);
         }
      }
   }
   else
   {
      (void)snprintf(window_title, MAX_WNINDOW_TITLE_LENGTH,
                     "Input Log %s", font_name);
   }

   if (get_arg(argc, argv, "-p", profile, MAX_PROFILE_NAME_LENGTH) == INCORRECT)
   {
      profile[0] = '\0';
   }
   if (get_arg(argc, argv, "-f", font_name, 40) == INCORRECT)
   {
      (void)strcpy(font_name, DEFAULT_FONT);
   }
   if (get_arg_array(argc, argv, "-h", &search_recipient,
                     &no_of_search_hosts) == INCORRECT)
   {
      no_of_search_hosts = 0;
   }
   else
   {
      if (no_of_search_hosts > 0)
      {
         int i;

         RT_ARRAY(search_user, no_of_search_hosts,
                  (MAX_RECIPIENT_LENGTH + 1), char);
         for (i = 0; i < no_of_search_hosts; i++)
         {
            search_user[i][0] = '\0';
         }
      }
   }
   if (get_arg_int_array(argc, argv, "-d", &search_dirid,
                         &no_of_search_dirids) == INCORRECT)
   {
      no_of_search_dirids = 0;
   }
   if (get_arg_array(argc, argv, "-D", &search_dir,
                     &no_of_search_dirs) == INCORRECT)
   {
      no_of_search_dirs = 0;
   }
   else
   {
      if (no_of_search_dirs > 0)
      {
         if ((search_dir_filter = malloc(no_of_search_dirs)) == NULL)
         {
            (void)fprintf(stderr, "malloc() error : %s (%s %d)\n",
                          strerror(errno), __FILE__, __LINE__);
            exit(INCORRECT);
         }
         if ((search_dir_length = malloc((no_of_search_dirs * sizeof(int)))) == NULL)
         {
            (void)fprintf(stderr, "malloc() error : %s (%s %d)\n",
                          strerror(errno), __FILE__, __LINE__);
            exit(INCORRECT);
         }
      }
   }

   /* Now lets see if user may use this program. */
   check_fake_user(argc, argv, AFD_CONFIG_FILE, fake_user);
   switch (get_permissions(&perm_buffer, fake_user, profile))
   {
      case NO_ACCESS : /* Cannot access afd.users file. */
         {
            char afd_user_file[MAX_PATH_LENGTH];

            (void)strcpy(afd_user_file, p_work_dir);
            (void)strcat(afd_user_file, ETC_DIR);
            (void)strcat(afd_user_file, AFD_USER_FILE);

            (void)fprintf(stderr,
                          "Failed to access `%s', unable to determine users permissions.\n",
                          afd_user_file);
         }
         exit(INCORRECT);

      case NONE :
         (void)fprintf(stderr, "%s (%s %d)\n",
                       PERMISSION_DENIED_STR, __FILE__, __LINE__);
         exit(INCORRECT);

      case SUCCESS : /* Lets evaluate the permissions and see what */
                     /* the user may do.                           */
         eval_permissions(perm_buffer);
         free(perm_buffer);
         break;

      case INCORRECT: /* Hmm. Something did go wrong. Since we want to */
                      /* be able to disable permission checking let    */
                      /* the user have all permissions.                */
         perm.view_passwd  = NO;
         perm.list_limit   = NO_LIMIT;
         break;

      default :
         (void)fprintf(stderr, "Impossible!! Remove the programmer!\n");
         exit(INCORRECT);
   }

   if (no_of_search_hosts == 0)
   {
      /* Collect all hostnames if -h is not used. */
      no_of_search_hosts = *argc - 1;
      if (no_of_search_hosts > 0)
      {
         int i = 0;

         RT_ARRAY(search_recipient, no_of_search_hosts,
                  (MAX_RECIPIENT_LENGTH + 1), char);
         RT_ARRAY(search_user, no_of_search_hosts,
                  (MAX_RECIPIENT_LENGTH + 1), char);
         while (*argc > 1)
         {
            (void)my_strncpy(search_recipient[i], argv[1], MAX_RECIPIENT_LENGTH + 1);
            if (strlen(search_recipient[i]) == MAX_HOSTNAME_LENGTH)
            {
               (void)strcat(search_recipient[i], "*");
            }
            (*argc)--; argv++;
            i++;
         }
         for (i = 0; i < no_of_search_hosts; i++)
         {
            search_user[i][0] = '\0';
         }
      }
   }

   start_time_val = -1;
   end_time_val = -1;
   search_file_size = -1;
   search_file_name[0] = '\0';
   special_button_flag = SEARCH_BUTTON;
   no_of_log_files = 0;

   /* Get the maximum number of logfiles we keep for history. */
   get_max_log_values(&max_input_log_files, MAX_INPUT_LOG_FILES_DEF,
                      MAX_INPUT_LOG_FILES, NULL, NULL, 0);

   return;
}


#ifdef HAVE_SETPRIORITY
/*+++++++++++++++++++++++++ get_afd_config_value() ++++++++++++++++++++++*/
static void
get_afd_config_value(void)
{
   char *buffer,
        config_file[MAX_PATH_LENGTH];

   (void)sprintf(config_file, "%s%s%s",
                 p_work_dir, ETC_DIR, AFD_CONFIG_FILE);
   if ((eaccess(config_file, F_OK) == 0) &&
       (read_file_no_cr(config_file, &buffer, YES, __FILE__, __LINE__) != INCORRECT))
   {
      char value[MAX_INT_LENGTH];

      if (get_definition(buffer, SHOW_LOG_PRIORITY_DEF,
                         value, MAX_INT_LENGTH) != NULL)
      {
         if (setpriority(PRIO_PROCESS, 0, atoi(value)) == -1)
         {
            system_log(WARN_SIGN, __FILE__, __LINE__,
                       "Failed to set priority to %d : %s",
                       atoi(value), strerror(errno));
         }
      }
      free(buffer);
   }

   return;
}
#endif


/*---------------------------------- usage() ----------------------------*/
static void
usage(char *progname)
{
   (void)fprintf(stderr,
                 "Usage : %s [options] [host alias 1..n]\n",
                 progname);
   (void)fprintf(stderr,
                 "        Options:\n");
   (void)fprintf(stderr,
                 "           -d <dir identifier 1> ... <dir identifier n>\n");
   (void)fprintf(stderr,
                 "           -D <directory 1> ... <directory n>\n");
   (void)fprintf(stderr,
                 "           -h <host alias 1> ... <host alias n>\n");
   (void)fprintf(stderr,
                 "           -f <font name>\n");
   (void)fprintf(stderr,
                 "           -p <user profile>\n");
   (void)fprintf(stderr,
                 "           -u [<fake user>]\n");
   (void)fprintf(stderr,
                 "           -w <working directory>\n");
   (void)fprintf(stderr,
                 "           --version\n");
   return;
}


/*-------------------------- eval_permissions() -------------------------*/
static void
eval_permissions(char *perm_buffer)
{
   char *ptr;

   /*
    * If we find 'all' right at the beginning, no further evaluation
    * is needed, since the user has all permissions.
    */
   if ((perm_buffer[0] == 'a') && (perm_buffer[1] == 'l') &&
       (perm_buffer[2] == 'l') &&
       ((perm_buffer[3] == '\0') || (perm_buffer[3] == ',') ||
        (perm_buffer[3] == ' ') || (perm_buffer[3] == '\t')))
   {
      perm.list_limit   = NO_LIMIT;
      perm.view_passwd  = YES;
   }
   else
   {
      /*
       * First of all check if the user may use this program
       * at all.
       */
      if ((ptr = posi(perm_buffer, SHOW_ILOG_PERM)) == NULL)
      {
         (void)fprintf(stderr, "%s (%s %d)\n",
                       PERMISSION_DENIED_STR, __FILE__, __LINE__);
         free(perm_buffer);
         exit(INCORRECT);
      }
      else
      {
         /*
          * For future use. Allow to limit for host names
          * as well.
          */
         ptr--;
      }

      /* May he see the password when using info click? */
      if ((ptr = posi(perm_buffer, VIEW_PASSWD_PERM)) == NULL)
      {
         /* The user may NOT view the password. */
         perm.view_passwd = NO;
      }

      /* Is there a limit on how many items the user may view? */
      if ((ptr = posi(perm_buffer, LIST_LIMIT)) == NULL)
      {
         /* There is no limit. */
         perm.list_limit = NO_LIMIT;
      }
      else
      {
         ptr--;
         if ((*ptr == ' ') || (*ptr == '\t'))
         {
            char *p_start = ptr + 1,
                 tmp_char;

            ptr++;
            while ((*ptr != ',') && (*ptr != ' ') && (*ptr != '\t') &&
                   (*ptr != '\n') && (*ptr != '\0'))
            {
               ptr++;
            }
            tmp_char = *ptr;
            *ptr = '\0';
            perm.list_limit = atoi(p_start);
            *ptr = tmp_char;
         }
         else
         {
            perm.list_limit = NO_LIMIT;
         }
      }
   }

   return;
}


/*++++++++++++++++++++++++++++++ sig_segv() +++++++++++++++++++++++++++++*/
static void
sig_segv(int signo)
{
   (void)fprintf(stderr, "Aaarrrggh! Received SIGSEGV. (%s %d)\n",
                 __FILE__, __LINE__);
   abort();
}


/*++++++++++++++++++++++++++++++ sig_bus() ++++++++++++++++++++++++++++++*/
static void
sig_bus(int signo)
{
   (void)fprintf(stderr, "Uuurrrggh! Received SIGBUS. (%s %d)\n",
                 __FILE__, __LINE__);
   abort();
}
