/* -*- mode: C; c-file-style: "gnu" -*- */
/* expirelist.c  List of items that expire
 *
 * Copyright (C) 2003  Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.1
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "expirelist.h"
#include "test.h"
#include <dbus/dbus-internals.h>
#include <dbus/dbus-mainloop.h>
#include <dbus/dbus-timeout.h>

static dbus_bool_t expire_timeout_handler (void *data);

static void
call_timeout_callback (DBusTimeout   *timeout,
                       void          *data)
{
  /* can return FALSE on OOM but we just let it fire again later */
  dbus_timeout_handle (timeout);
}

BusExpireList*
bus_expire_list_new (DBusLoop      *loop,
                     int            expire_after,
                     BusExpireFunc  expire_func,
                     void          *data)
{
  BusExpireList *list;

  list = dbus_new0 (BusExpireList, 1);
  if (list == NULL)
    return NULL;

  list->expire_func = expire_func;
  list->data = data;
  list->loop = loop;
  list->expire_after = expire_after;

  list->timeout = _dbus_timeout_new (100, /* irrelevant */
                                     expire_timeout_handler,
                                     list, NULL);
  if (list->timeout == NULL)
    goto failed;

  _dbus_timeout_set_enabled (list->timeout, FALSE);

  if (!_dbus_loop_add_timeout (list->loop,
                               list->timeout,
                               call_timeout_callback, NULL, NULL))
    goto failed;

  return list;

 failed:
  if (list->timeout)
    _dbus_timeout_unref (list->timeout);

  dbus_free (list);

  return NULL;
}

void
bus_expire_list_free (BusExpireList *list)
{
  _dbus_assert (list->items == NULL);

  _dbus_loop_remove_timeout (list->loop, list->timeout,
                             call_timeout_callback, NULL);

  _dbus_timeout_unref (list->timeout);

  dbus_free (list);
}

void
bus_expire_timeout_set_interval (DBusTimeout *timeout,
                                 int          next_interval)
{
  if (next_interval >= 0)
    {
      _dbus_timeout_set_interval (timeout,
                                  next_interval);
      _dbus_timeout_set_enabled (timeout, TRUE);

      _dbus_verbose ("Enabled expire timeout with interval %d\n",
                     next_interval);
    }
  else if (dbus_timeout_get_enabled (timeout))
    {
      _dbus_timeout_set_enabled (timeout, FALSE);

      _dbus_verbose ("Disabled expire timeout\n");
    }
  else
    _dbus_verbose ("No need to disable expire timeout\n");
}

static int
do_expiration_with_current_time (BusExpireList *list,
                                 long           tv_sec,
                                 long           tv_usec)
{
  DBusList *link;
  int next_interval;

  next_interval = -1;
  
  link = _dbus_list_get_first_link (&list->items);
  while (link != NULL)
    {
      DBusList *next = _dbus_list_get_next_link (&list->items, link);
      double elapsed;
      BusExpireItem *item;

      item = link->data;

      elapsed = ELAPSED_MILLISECONDS_SINCE (item->added_tv_sec,
                                            item->added_tv_usec,
                                            tv_sec, tv_usec);

      if (elapsed >= (double) list->expire_after)
        {
          _dbus_verbose ("Expiring an item %p\n", item);

          /* If the expire function fails, we just end up expiring
           * this item next time we walk through the list. This would
           * be an indeterminate time normally, so we set up the
           * next_interval to be "shortly" (just enough to avoid
           * a busy loop)
           */
          if (!(* list->expire_func) (list, link, list->data))
            {
              next_interval = _dbus_get_oom_wait ();
              break;
            }
        }
      else
        {
          /* We can end the loop, since the connections are in oldest-first order */
          next_interval = ((double)list->expire_after) - elapsed;
          _dbus_verbose ("Item %p expires in %d milliseconds\n",
                         item, next_interval);

          break;
        }

      link = next;
    }

  return next_interval;
}

static void
bus_expirelist_expire (BusExpireList *list)
{
  int next_interval;

  next_interval = -1;

  if (list->items != NULL)
    {
      long tv_sec, tv_usec;

      _dbus_get_current_time (&tv_sec, &tv_usec);

      next_interval = do_expiration_with_current_time (list, tv_sec, tv_usec);
    }

  bus_expire_timeout_set_interval (list->timeout, next_interval);
}

static dbus_bool_t
expire_timeout_handler (void *data)
{
  BusExpireList *list = data;

  _dbus_verbose ("Running %s\n", _DBUS_FUNCTION_NAME);

  /* note that this may remove the timeout */
  bus_expirelist_expire (list);

  return TRUE;
}

#ifdef DBUS_BUILD_TESTS

typedef struct
{
  BusExpireItem item;
  int expire_count;
} TestExpireItem;

static dbus_bool_t
test_expire_func (BusExpireList *list,
                  DBusList      *link,
                  void          *data)
{
  TestExpireItem *t;

  t = (TestExpireItem*) link->data;

  t->expire_count += 1;

  return TRUE;
}

static void
time_add_milliseconds (long *tv_sec,
                       long *tv_usec,
                       int   milliseconds)
{
  *tv_sec = *tv_sec + milliseconds / 1000;
  *tv_usec = *tv_usec + milliseconds * 1000;
  if (*tv_usec >= 1000000)
    {
      *tv_usec -= 1000000;
      *tv_sec += 1;
    }
}

dbus_bool_t
bus_expire_list_test (const DBusString *test_data_dir)
{
  DBusLoop *loop;
  BusExpireList *list;
  long tv_sec, tv_usec;
  long tv_sec_not_expired, tv_usec_not_expired;
  long tv_sec_expired, tv_usec_expired;
  long tv_sec_past, tv_usec_past;
  TestExpireItem *item;
  int next_interval;
  dbus_bool_t result = FALSE;


  loop = _dbus_loop_new ();
  _dbus_assert (loop != NULL);

#define EXPIRE_AFTER 100
  
  list = bus_expire_list_new (loop, EXPIRE_AFTER,
                              test_expire_func, NULL);
  _dbus_assert (list != NULL);

  _dbus_get_current_time (&tv_sec, &tv_usec);

  tv_sec_not_expired = tv_sec;
  tv_usec_not_expired = tv_usec;
  time_add_milliseconds (&tv_sec_not_expired,
                         &tv_usec_not_expired, EXPIRE_AFTER - 1);

  tv_sec_expired = tv_sec;
  tv_usec_expired = tv_usec;
  time_add_milliseconds (&tv_sec_expired,
                         &tv_usec_expired, EXPIRE_AFTER);
  

  tv_sec_past = tv_sec - 1;
  tv_usec_past = tv_usec;

  item = dbus_new0 (TestExpireItem, 1);

  if (item == NULL)
    goto oom;

  item->item.added_tv_sec = tv_sec;
  item->item.added_tv_usec = tv_usec;
  if (!_dbus_list_append (&list->items, item))
    _dbus_assert_not_reached ("out of memory");

  next_interval =
    do_expiration_with_current_time (list, tv_sec_not_expired,
                                     tv_usec_not_expired);
  _dbus_assert (item->expire_count == 0);
  _dbus_verbose ("next_interval = %d\n", next_interval);
  _dbus_assert (next_interval == 1);
  
  next_interval =
    do_expiration_with_current_time (list, tv_sec_expired,
                                     tv_usec_expired);
  _dbus_assert (item->expire_count == 1);
  _dbus_verbose ("next_interval = %d\n", next_interval);
  _dbus_assert (next_interval == -1);

  next_interval =
    do_expiration_with_current_time (list, tv_sec_past,
                                     tv_usec_past);
  _dbus_assert (item->expire_count == 1);
  _dbus_verbose ("next_interval = %d\n", next_interval);
  _dbus_assert (next_interval == 1000 + EXPIRE_AFTER);

  _dbus_list_clear (&list->items);
  dbus_free (item);
  
  bus_expire_list_free (list);
  _dbus_loop_unref (loop);
  
  result = TRUE;

 oom:
  return result;
}

#endif /* DBUS_BUILD_TESTS */
