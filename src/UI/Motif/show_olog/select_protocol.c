/*
 *  select_protocol.c - Part of AFD, an automatic file distribution program.
 *  Copyright (c) 2015 Holger Kiehl <Holger.Kiehl@dwd.de>
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
 **   select_protocol - dialog to allow user to choose protocol
 **
 ** SYNOPSIS
 **   void select_protocol(Widget    w,
 **                       XtPointer client_data,
 **                       XtPointer call_data)
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
 **   26.09.2015 H.Kiehl Created
 **
 */
DESCR__E_M3

#include <stdio.h>
#include <stdlib.h>
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/LabelG.h>
#include <Xm/Frame.h>
#include <Xm/Separator.h>
#include <Xm/PushB.h>
#include <Xm/ToggleBG.h>
#include <X11/Intrinsic.h>
#ifdef WITH_EDITRES
# include <X11/Xmu/Editres.h>
#endif
#include <errno.h>
#include "p_str.h"
#include "show_olog.h"

#define NO_OF_COLUMNS 3

/* Global variables. */
unsigned int        p_toggles_set = ~0u;
int                 toggle_counter;
Widget              selectshell = (Widget)NULL,
                    *toggle_w = NULL;

/* External global variables. */
extern XT_PTR_TYPE  toggles_set;
extern Display      *display;
extern Widget       appshell;
extern char         font_name[];

/* Local function prototypes. */
static void         done_button(Widget, XtPointer, XtPointer),
                    p_toggle_all(Widget, XtPointer, XtPointer),
                    p_toggled(Widget, XtPointer, XtPointer);



/*######################### select_protocol() ###########################*/
void
select_protocol(Widget w, XtPointer client_data, XtPointer call_data)
{
   /*
    * First, see if the window has already been created. If
    * no create a new window.
    */
   if ((selectshell == (Widget)NULL) || (XtIsRealized(selectshell) == False) ||
       (XtIsSensitive(selectshell) != True))
   {
      int             column_width,
                      i,
                      j,
                      no_of_rows;
      XT_PTR_TYPE     pos;
      Widget          buttonbox_w,
                      button_w,
                      criteriabox_w,
                      frame_w,
                      main_form_w,
                      separator_w;
      Arg             args[6];
      Cardinal        argcount;
      XmFontList      p_fontlist;
      XmFontListEntry entry;

      selectshell = XtVaCreatePopupShell("Select Protocol",
                                         topLevelShellWidgetClass,
                                         appshell, NULL);

      /* Create managing widget. */
      main_form_w = XmCreateForm(selectshell, "main_form", NULL, 0);

      /* Prepare font. */
      if ((entry = XmFontListEntryLoad(XtDisplay(main_form_w), font_name,
                                       XmFONT_IS_FONT, "TAG1")) == NULL)
      {
         if ((entry = XmFontListEntryLoad(XtDisplay(main_form_w), DEFAULT_FONT,
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
      p_fontlist = XmFontListAppendEntry(NULL, entry);
      XmFontListEntryFree(&entry);

      /*---------------------------------------------------------------*/
      /*                         Button Box                            */
      /*---------------------------------------------------------------*/
      argcount = 0;
      XtSetArg(args[argcount], XmNbottomAttachment, XmATTACH_FORM);
      argcount++;
      XtSetArg(args[argcount], XmNleftAttachment,   XmATTACH_FORM);
      argcount++;
      XtSetArg(args[argcount], XmNrightAttachment,  XmATTACH_FORM);
      argcount++;
      XtSetArg(args[argcount], XmNfractionBase,     21);
      argcount++;
      buttonbox_w = XmCreateForm(main_form_w, "buttonbox", args, argcount);

      /* Create Toggle All Button. */
      button_w = XtVaCreateManagedWidget("Toggle all",
                        xmPushButtonWidgetClass, buttonbox_w,
                        XmNfontList,             p_fontlist,
                        XmNtopAttachment,        XmATTACH_POSITION,
                        XmNtopPosition,          1,
                        XmNleftAttachment,       XmATTACH_POSITION,
                        XmNleftPosition,         1,
                        XmNrightAttachment,      XmATTACH_POSITION,
                        XmNrightPosition,        10,
                        XmNbottomAttachment,     XmATTACH_POSITION,
                        XmNbottomPosition,       20,
                        NULL);
      XtAddCallback(button_w, XmNactivateCallback,
                    (XtCallbackProc)p_toggle_all, (XtPointer)0);

      /* Create Close Button. */
      button_w = XtVaCreateManagedWidget("Close",
                        xmPushButtonWidgetClass, buttonbox_w,
                        XmNfontList,             p_fontlist,
                        XmNtopAttachment,        XmATTACH_POSITION,
                        XmNtopPosition,          1,
                        XmNleftAttachment,       XmATTACH_POSITION,
                        XmNleftPosition,         10,
                        XmNrightAttachment,      XmATTACH_POSITION,
                        XmNrightPosition,        20,
                        XmNbottomAttachment,     XmATTACH_POSITION,
                        XmNbottomPosition,       20,
                        NULL);
      XtAddCallback(button_w, XmNactivateCallback,
                    (XtCallbackProc)done_button, (XtPointer)0);
      XtManageChild(buttonbox_w);

      /*---------------------------------------------------------------*/
      /*                      Horizontal Separator                     */
      /*---------------------------------------------------------------*/
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
      separator_w = XmCreateSeparator(main_form_w, "separator", args, argcount);
      XtManageChild(separator_w);

      /*---------------------------------------------------------------*/
      /*                          Frame Box                            */
      /*---------------------------------------------------------------*/
      frame_w = XtVaCreateManagedWidget("protocol_frame",
                        xmFrameWidgetClass,  main_form_w,
                        XmNshadowType,       XmSHADOW_ETCHED_IN,
                        XmNtopAttachment,    XmATTACH_FORM,
                        XmNtopOffset,        5,
                        XmNleftAttachment,   XmATTACH_FORM,
                        XmNleftOffset,       5,
                        XmNrightAttachment,  XmATTACH_FORM,
                        XmNrightOffset,      5,
                        XmNbottomAttachment, XmATTACH_WIDGET,
                        XmNbottomWidget,     separator_w,
                        XmNbottomOffset,     5,
                        NULL);
      XtVaCreateManagedWidget("Protocols",
                        xmLabelGadgetClass,        frame_w,
                        XmNchildType,              XmFRAME_TITLE_CHILD,
                        XmNchildVerticalAlignment, XmALIGNMENT_CENTER,
                        NULL);

      /*---------------------------------------------------------------*/
      /*                        Criteria Box                           */
      /*---------------------------------------------------------------*/
      no_of_rows = (sizeof(pstr) / sizeof(*pstr)) / NO_OF_COLUMNS;
      if ((sizeof(pstr) / sizeof(*pstr)) % NO_OF_COLUMNS)
      {
         no_of_rows++;
      }
      column_width = (10 * no_of_rows) / NO_OF_COLUMNS;
      criteriabox_w = XtVaCreateWidget("criteriabox",
                        xmFormWidgetClass,   frame_w,
                        XmNtopAttachment,    XmATTACH_FORM,
                        XmNtopOffset,        5,
                        XmNleftAttachment,   XmATTACH_FORM,
                        XmNleftOffset,       5,
                        XmNrightAttachment,  XmATTACH_FORM,
                        XmNrightOffset,      5,
                        XmNbottomAttachment, XmATTACH_FORM,
                        XmNbottomOffset,     5,
                        XmNfractionBase,     (10 * no_of_rows),
                        NULL);

      /*---------------------------------------------------------------*/
      /*                    All toggle protocols                       */
      /*---------------------------------------------------------------*/
      if ((toggle_w == NULL) &&
          ((toggle_w = malloc(((sizeof(pstr) / sizeof(*pstr)) * sizeof(Widget)))) == NULL))
      {
         (void)fprintf(stderr, "malloc() error : %s (%s %d)\n",
                       strerror(errno), __FILE__, __LINE__);
         exit(INCORRECT);
      }
      toggle_counter = 0;
      for (i = 0; i < NO_OF_COLUMNS; i++)
      {
         for (j = 0; j < no_of_rows; j++)
         {
            pos = (i * no_of_rows) + j;
            if (pos < (sizeof(pstr) / sizeof(*pstr)))
            {
               toggle_w[toggle_counter] = XtVaCreateManagedWidget(pstr[pos],
                           xmToggleButtonGadgetClass, criteriabox_w,
                           XmNfontList,               p_fontlist,
                           XmNset,                    True,
                           XmNalignment,              XmALIGNMENT_BEGINNING,
                           XmNtopAttachment,          XmATTACH_POSITION,
                           XmNtopPosition,            (j * 10),
                           XmNbottomAttachment,       XmATTACH_POSITION,
                           XmNbottomPosition,         ((j + 1) * 10),
                           XmNleftAttachment,         XmATTACH_POSITION,
                           XmNleftPosition,           (i * column_width),
                           XmNrightAttachment,        XmATTACH_POSITION,
                           XmNrightPosition,          ((i + 1) * column_width),
                           NULL);
               XtAddCallback(toggle_w[toggle_counter], XmNvalueChangedCallback,
                             (XtCallbackProc)p_toggled, (XtPointer)pos);
               toggle_counter++;
            }
         }
      }

      XtManageChild(criteriabox_w);
      XtManageChild(main_form_w);

#ifdef WITH_EDITRES
      XtAddEventHandler(selectshell, (EventMask)0, True, _XEditResCheckMessages, NULL);
#endif
   }
   XtPopup(selectshell, XtGrabNone);

   return;
}


/*+++++++++++++++++++++++++++ p_toggle_all() ++++++++++++++++++++++++++++*/
static void
p_toggle_all(Widget w, XtPointer client_data, XtPointer call_data)
{
   int     i;
   Boolean set_toggle;

   toggles_set = ~toggles_set;
   p_toggles_set = ~p_toggles_set;
   for (i = 0; i < toggle_counter; i++)
   {
      if (p_toggles_set & (1 << i))
      {
         set_toggle = True;
      }
      else
      {
         set_toggle = False;
      }
      XmToggleButtonGadgetSetState(toggle_w[i], set_toggle, False);
   }

   return;
}


/*++++++++++++++++++++++++++++++ p_toggled() ++++++++++++++++++++++++++++*/
static void
p_toggled(Widget w, XtPointer client_data, XtPointer call_data)
{
   p_toggles_set ^= (1 << (XT_PTR_TYPE)client_data);
   toggles_set ^= pflag[(XT_PTR_TYPE)client_data];

   return;
}


/*++++++++++++++++++++++++++++ done_button() ++++++++++++++++++++++++++++*/
static void
done_button(Widget w, XtPointer client_data, XtPointer call_data)
{
   XtPopdown(selectshell);

   return;
}
