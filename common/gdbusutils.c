#include <config.h>

#include <stdlib.h>
#include <string.h>

#include <glib/gi18n-lib.h>
#include <gdbusutils.h>
#include <gio/gcancellable.h>

void
_g_dbus_oom (void)
{
  g_error ("DBus failed with out of memory error");
  exit(1);
}

/* We use _ for escaping, so its not valid */
#define VALID_INITIAL_NAME_CHARACTER(c)         \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') )
#define VALID_NAME_CHARACTER(c)                 \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z'))


static void
append_escaped_name (GString *s,
		     const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first)
	{
	  if (VALID_INITIAL_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}

      first = FALSE;
      g_string_append_c (s, '_');
      g_string_append_c (s, hex[((guchar)c) >> 4]);
      g_string_append_c (s, hex[((guchar)c) & 0xf]);
    }
}

DBusMessage *
_dbus_message_new_error_from_gerror (DBusMessage *message,
				     GError *error)
{
  DBusMessage *reply;
  GString *str;

  str = g_string_new ("org.glib.GError.");
  append_escaped_name (str, g_quark_to_string (error->domain));
  g_string_append_printf (str, ".c%d", error->code);
  reply = dbus_message_new_error (message, str->str, error->message);
  g_string_free (str, TRUE);
  return reply;
}


static void
append_unescaped_dbus_name (GString *s,
			    const char *escaped,
			    const char *end)
{
  guchar c;

  while (escaped < end)
    {
      c = *escaped++;
      if (c == '_' &&
	  escaped < end)
	{
	  c = g_ascii_xdigit_value (*escaped++) << 4;

	  if (escaped < end)
	    c |= g_ascii_xdigit_value (*escaped++);
	}
      g_string_append_c (s, c);
    }
}

char *
_g_dbus_unescape_bus_name (const char *escaped, const char *end)
{
  GString *s = g_string_new ("");
  
  if (end == NULL)
    end = escaped + strlen (escaped);

  append_unescaped_dbus_name (s, escaped, end);
  return g_string_free (s, FALSE);
}

/* We use _ for escaping */
#define VALID_INITIAL_BUS_NAME_CHARACTER(c)     \
  ( ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_') || */((c) == '-'))
#define VALID_BUS_NAME_CHARACTER(c)             \
  ( ((c) >= '0' && (c) <= '9') ||               \
    ((c) >= 'A' && (c) <= 'Z') ||               \
    ((c) >= 'a' && (c) <= 'z') ||               \
   /*((c) == '_')||*/  ((c) == '-'))

void
_g_dbus_append_escaped_bus_name (GString *s,
				 gboolean at_start,
				 const char *unescaped)
{
  char c;
  gboolean first;
  static const gchar hex[16] = "0123456789ABCDEF";

  while ((c = *unescaped++) != 0)
    {
      if (first && at_start)
	{
	  if (VALID_INITIAL_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}
      else
	{
	  if (VALID_BUS_NAME_CHARACTER (c))
	    {
	      g_string_append_c (s, c);
	      continue;
	    }
	}

      first = FALSE;
      g_string_append_c (s, '_');
      g_string_append_c (s, hex[((guchar)c) >> 4]);
      g_string_append_c (s, hex[((guchar)c) & 0xf]);
    }
}

void
_g_dbus_message_iter_append_cstring (DBusMessageIter *iter, const char *str)
{
  DBusMessageIter array;

  if (!dbus_message_iter_open_container (iter,
					 DBUS_TYPE_ARRAY,
					 DBUS_TYPE_BYTE_AS_STRING,
					 &array))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_append_fixed_array (&array,
					     DBUS_TYPE_BYTE,
					     &str, strlen (str)))
    _g_dbus_oom ();
  
  if (!dbus_message_iter_close_container (iter, &array))
    _g_dbus_oom ();
}

void
_g_dbus_message_append_args_valist (DBusMessage *message,
				    int          first_arg_type,
				    va_list      var_args)
{
  int type;
  DBusMessageIter iter;

  g_return_if_fail (message != NULL);

  type = first_arg_type;

  dbus_message_iter_init_append (message, &iter);

  while (type != DBUS_TYPE_INVALID)
    {
      if (type == G_DBUS_TYPE_CSTRING)
	{
	  const char **value_p;
	  const char *value;

	  value_p = va_arg (var_args, const char**);
	  value = *value_p;

	  _g_dbus_message_iter_append_cstring (&iter, value);
	}
      else if (dbus_type_is_basic (type))
        {
          const void *value;
          value = va_arg (var_args, const void*);

          if (!dbus_message_iter_append_basic (&iter,
                                               type,
                                               value))
	    _g_dbus_oom ();
        }
      else if (type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          DBusMessageIter array;
          char buf[2];

          element_type = va_arg (var_args, int);
              
          buf[0] = element_type;
          buf[1] = '\0';
          if (!dbus_message_iter_open_container (&iter,
                                                 DBUS_TYPE_ARRAY,
                                                 buf,
                                                 &array))
	    _g_dbus_oom ();
          
          if (dbus_type_is_fixed (element_type))
            {
              const void **value;
              int n_elements;

              value = va_arg (var_args, const void**);
              n_elements = va_arg (var_args, int);
              
              if (!dbus_message_iter_append_fixed_array (&array,
                                                         element_type,
                                                         value,
                                                         n_elements))
		_g_dbus_oom ();
            }
          else if (element_type == DBUS_TYPE_STRING ||
                   element_type == DBUS_TYPE_SIGNATURE ||
                   element_type == DBUS_TYPE_OBJECT_PATH)
            {
              const char ***value_p;
              const char **value;
              int n_elements;
              int i;
              
              value_p = va_arg (var_args, const char***);
              n_elements = va_arg (var_args, int);

              value = *value_p;
              
              i = 0;
              while (i < n_elements)
                {
                  if (!dbus_message_iter_append_basic (&array,
                                                       element_type,
                                                       &value[i]))
		    _g_dbus_oom ();
                  ++i;
                }
            }
          else
            {
              g_error ("arrays of %d can't be appended with _g_dbus_message_append_args_valist for now\n",
		       element_type);
            }

          if (!dbus_message_iter_close_container (&iter, &array))
	    _g_dbus_oom ();
        }

      type = va_arg (var_args, int);
    }
}

dbus_bool_t
_g_dbus_message_iter_get_args_valist (DBusMessageIter *iter,
				      DBusError       *error,
				      int              first_arg_type,
				      va_list          var_args)
{
  int spec_type, msg_type, i;
  dbus_bool_t retval;

  retval = FALSE;

  spec_type = first_arg_type;
  i = 0;

  while (spec_type != DBUS_TYPE_INVALID)
    {
      msg_type = dbus_message_iter_get_arg_type (iter);

      if (msg_type != spec_type)
	{
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Argument %d is specified to be of type \"%d\", but "
                          "is actually of type \"%d\"\n", i,
                          spec_type,
                          msg_type);

          goto out;
	}

      if (dbus_type_is_basic (spec_type))
        {
          void *ptr;

          ptr = va_arg (var_args, void*);

          g_assert (ptr != NULL);

	  dbus_message_iter_get_basic (iter, ptr);
        }
      else if (spec_type == DBUS_TYPE_ARRAY)
        {
          int element_type;
          int spec_element_type;
          const void **ptr;
          int *n_elements_p;
          DBusMessageIter array;

          spec_element_type = va_arg (var_args, int);
          element_type = dbus_message_iter_get_element_type (iter);

          if (spec_element_type != element_type)
            {
              dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                              "Argument %d is specified to be an array of \"%d\", but "
                              "is actually an array of \"%d\"\n",
                              i,
                              spec_element_type,
                              element_type);

              goto out;
            }

          if (dbus_type_is_fixed (spec_element_type))
            {
              ptr = va_arg (var_args, const void**);
              n_elements_p = va_arg (var_args, int*);

              g_assert (ptr != NULL);
              g_assert (n_elements_p != NULL);

              dbus_message_iter_recurse (iter, &array);

              dbus_message_iter_get_fixed_array (&array,
						 ptr, n_elements_p);
            }
          else if (spec_element_type == DBUS_TYPE_STRING ||
                   spec_element_type == DBUS_TYPE_SIGNATURE ||
                   spec_element_type == DBUS_TYPE_OBJECT_PATH)
            {
              char ***str_array_p;
              int n_elements;
              char **str_array;

              str_array_p = va_arg (var_args, char***);
              n_elements_p = va_arg (var_args, int*);

              g_assert (str_array_p != NULL);
              g_assert (n_elements_p != NULL);

              /* Count elements in the array */
              dbus_message_iter_recurse (iter, &array);

              n_elements = 0;
              while (dbus_message_iter_get_arg_type (&array) != DBUS_TYPE_INVALID)
                {
                  ++n_elements;
                  dbus_message_iter_next (&array);
                }

              str_array = g_new0 (char*, n_elements + 1);
              if (str_array == NULL)
                {
                  _g_dbus_oom ();
                  goto out;
                }

              /* Now go through and dup each string */
              dbus_message_iter_recurse (iter, &array);

              i = 0;
              while (i < n_elements)
                {
                  const char *s;
                  dbus_message_iter_get_basic (&array, &s);
                  
                  str_array[i] = g_strdup (s);
                  if (str_array[i] == NULL)
                    {
                      g_strfreev (str_array);
		      _g_dbus_oom ();
                      goto out;
                    }
                  
                  ++i;
                  
                  if (!dbus_message_iter_next (&array))
                    g_assert (i == n_elements);
                }

              g_assert (dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_INVALID);
              g_assert (i == n_elements);
              g_assert (str_array[i] == NULL);

              *str_array_p = str_array;
              *n_elements_p = n_elements;
            }
        }

      spec_type = va_arg (var_args, int);
      if (!dbus_message_iter_next (iter) && spec_type != DBUS_TYPE_INVALID)
        {
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Message has only %d arguments, but more were expected", i);
          goto out;
        }

      i++;
    }

  retval = TRUE;

 out:
  return retval;
}

dbus_bool_t
_g_dbus_message_iter_get_args (DBusMessageIter *iter,
			       DBusError       *error,
			       int              first_arg_type,
			       ...)
{
  va_list var_args;
  dbus_bool_t res;

  va_start (var_args, first_arg_type);
  res = _g_dbus_message_iter_get_args (iter, error,
				       first_arg_type,
				       var_args);
  va_end (var_args);
  return res;
}

/* Same as the dbus one, except doesn't give OOM and handles
   G_DBUS_TYPE_CSTRING
*/
void
_g_dbus_message_append_args (DBusMessage *message,
			     int          first_arg_type,
			     ...)
{
  va_list var_args;

  g_return_if_fail (message != NULL);

  va_start (var_args, first_arg_type);
  _g_dbus_message_append_args_valist (message,
				      first_arg_type,
				      var_args);
  va_end (var_args);
}


void
_g_error_from_dbus (DBusError *derror, 
		    GError **error)
{
  const char *name, *end;;
  char *m;
  GString *str;
  GQuark domain;
  int code;

  if (g_str_has_prefix (derror->name, "org.glib.GError."))
    {
      domain = 0;
      code = 0;

      name = derror->name + strlen ("org.glib.GError.");
      end = strchr (name, '.');
      if (end)
	{
	  str = g_string_new (NULL);
	  append_unescaped_dbus_name (str, name, end);
	  domain = g_quark_from_string (str->str);
	  g_string_free (str, TRUE);

	  end++; /* skip . */
	  if (*end++ == 'c')
	    code = atoi (end);
	}
      
      g_set_error (error, domain, code, "%s", derror->message);
    }
  /* TODO: Special case other types, like DBUS_ERROR_NO_MEMORY etc? */
  else
    {
      m = g_strdup_printf ("DBus error %s: %s", derror->name, derror->message);
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO, "%s", m);
      g_free (m);
    }
}

GList *
_g_dbus_bus_list_names_with_prefix (DBusConnection *connection,
				    const char *prefix,
				    DBusError *error)
{
  DBusMessage *message, *reply;
  DBusMessageIter iter, array;
  GList *names;

  g_return_val_if_fail (connection != NULL, NULL);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_DBUS,
                                          DBUS_PATH_DBUS,
                                          DBUS_INTERFACE_DBUS,
                                          "ListNames");
  if (message == NULL)
    return NULL;
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1, error);
  dbus_message_unref (message);
  
  if (reply == NULL)
    return NULL;
  
  names = NULL;
  
  if (!dbus_message_iter_init (reply, &iter) ||
      (dbus_message_iter_get_arg_type (&iter) != DBUS_TYPE_ARRAY) ||
      (dbus_message_iter_get_element_type (&iter) != DBUS_TYPE_STRING))
    goto out;

  for (dbus_message_iter_recurse (&iter, &array);  
       dbus_message_iter_get_arg_type (&array) == DBUS_TYPE_STRING;
       dbus_message_iter_next (&array))
    {
      char *name;
      dbus_message_iter_get_basic (&array, &name);
      if (g_str_has_prefix (name, prefix))
	names = g_list_prepend (names, g_strdup (name));
    }

  names = g_list_reverse (names);
  
 out:
  dbus_message_unref (reply);
  return names;
}

/*************************************************************************
 *             Helper fd source                                          *
 ************************************************************************/

typedef struct 
{
  GSource source;
  GPollFD pollfd;
  GCancellable *cancellable;
  gulong cancelled_tag;
} FDSource;

static gboolean 
fd_source_prepare (GSource  *source,
		   gint     *timeout)
{
  FDSource *fd_source = (FDSource *)source;
  *timeout = -1;
  
  return g_cancellable_is_cancelled (fd_source->cancellable);
}

static gboolean 
fd_source_check (GSource  *source)
{
  FDSource *fd_source = (FDSource *)source;

  return
    g_cancellable_is_cancelled  (fd_source->cancellable) ||
    fd_source->pollfd.revents != 0;
}

static gboolean
fd_source_dispatch (GSource     *source,
		    GSourceFunc  callback,
		    gpointer     user_data)

{
  GFDSourceFunc func = (GFDSourceFunc)callback;
  FDSource *fd_source = (FDSource *)source;

  g_assert (func != NULL);

  return (*func) (user_data, fd_source->pollfd.revents, fd_source->pollfd.fd);
}

static void 
fd_source_finalize (GSource *source)
{
  FDSource *fd_source = (FDSource *)source;

  if (fd_source->cancelled_tag)
    g_signal_handler_disconnect (fd_source->cancellable,
				 fd_source->cancelled_tag);

  if (fd_source->cancellable)
    g_object_unref (fd_source->cancellable);
}

static GSourceFuncs fd_source_funcs = {
  fd_source_prepare,
  fd_source_check,
  fd_source_dispatch,
  fd_source_finalize
};

/* Might be called on another thread */
static void
fd_source_cancelled_cb (GCancellable *cancellable,
			gpointer data)
{
  /* Wake up the mainloop in case we're waiting on async calls with FDSource */
  g_main_context_wakeup (NULL);
}

/* Two __ to avoid conflict with gio version */
GSource *
__g_fd_source_new (int fd,
		   gushort events,
		   GCancellable *cancellable)
{
  GSource *source;
  FDSource *fd_source;

  source = g_source_new (&fd_source_funcs, sizeof (FDSource));
  fd_source = (FDSource *)source;

  if (cancellable)
    fd_source->cancellable = g_object_ref (cancellable);
  
  fd_source->pollfd.fd = fd;
  fd_source->pollfd.events = events;
  g_source_add_poll (source, &fd_source->pollfd);

  if (cancellable)
    fd_source->cancelled_tag =
      g_signal_connect_data (cancellable, "cancelled",
			     (GCallback)fd_source_cancelled_cb,
			     NULL, NULL,
			     0);
  
  return source;
}


/*************************************************************************
 *                                                                       *
 *      dbus mainloop integration for async ops                          *
 *                                                                       *
 *************************************************************************/

static gint32 main_integration_data_slot = -1;
static GOnce once_init_main_integration = G_ONCE_INIT;

/**
 * A GSource subclass for dispatching DBusConnection messages.
 * We need this on top of the IO handlers, because sometimes
 * there are messages to dispatch queued up but no IO pending.
 * 
 * The source is owned by the connection (and the main context
 * while that is alive)
 */
typedef struct 
{
  GSource source;
  
  DBusConnection *connection;
  GSList *ios;
  GSList *timeouts;
} DBusSource;

typedef struct
{
  DBusSource *dbus_source;
  GSource *source;
  DBusWatch *watch;
} IOHandler;

typedef struct
{
  DBusSource *dbus_source;
  GSource *source;
  DBusTimeout *timeout;
} TimeoutHandler;

static gpointer
main_integration_init (gpointer arg)
{
  if (!dbus_connection_allocate_data_slot (&main_integration_data_slot))
    g_error ("Unable to allocate data slot");

  return NULL;
}

static gboolean
dbus_source_prepare (GSource *source,
		     gint    *timeout)
{
  DBusConnection *connection = ((DBusSource *)source)->connection;
  
  *timeout = -1;

  return (dbus_connection_get_dispatch_status (connection) == DBUS_DISPATCH_DATA_REMAINS);  
}

static gboolean
dbus_source_check (GSource *source)
{
  return FALSE;
}

static gboolean
dbus_source_dispatch (GSource     *source,
		      GSourceFunc  callback,
		      gpointer     user_data)
{
  DBusConnection *connection = ((DBusSource *)source)->connection;

  dbus_connection_ref (connection);

  /* Only dispatch once - we don't want to starve other GSource */
  dbus_connection_dispatch (connection);
  
  dbus_connection_unref (connection);

  return TRUE;
}

static gboolean
io_handler_dispatch (gpointer data,
                     GIOCondition condition,
                     int fd)
{
  IOHandler *handler = data;
  guint dbus_condition = 0;
  DBusConnection *connection;

  connection = handler->dbus_source->connection;
  
  if (connection)
    dbus_connection_ref (connection);
  
  if (condition & G_IO_IN)
    dbus_condition |= DBUS_WATCH_READABLE;
  if (condition & G_IO_OUT)
    dbus_condition |= DBUS_WATCH_WRITABLE;
  if (condition & G_IO_ERR)
    dbus_condition |= DBUS_WATCH_ERROR;
  if (condition & G_IO_HUP)
    dbus_condition |= DBUS_WATCH_HANGUP;

  /* Note that we don't touch the handler after this, because
   * dbus may have disabled the watch and thus killed the
   * handler.
   */
  dbus_watch_handle (handler->watch, dbus_condition);
  handler = NULL;

  if (connection)
    dbus_connection_unref (connection);
  
  return TRUE;
}

static void
io_handler_free (IOHandler *handler)
{
  DBusSource *dbus_source;
  
  dbus_source = handler->dbus_source;
  dbus_source->ios = g_slist_remove (dbus_source->ios, handler);
  
  g_source_destroy (handler->source);
  g_source_unref (handler->source);
  g_free (handler);
}

static void
dbus_source_add_watch (DBusSource *dbus_source,
		       DBusWatch *watch)
{
  guint flags;
  GIOCondition condition;
  IOHandler *handler;

  if (!dbus_watch_get_enabled (watch))
    return;
  
  g_assert (dbus_watch_get_data (watch) == NULL);
  
  flags = dbus_watch_get_flags (watch);

  condition = G_IO_ERR | G_IO_HUP;
  if (flags & DBUS_WATCH_READABLE)
    condition |= G_IO_IN;
  if (flags & DBUS_WATCH_WRITABLE)
    condition |= G_IO_OUT;

  handler = g_new0 (IOHandler, 1);
  handler->dbus_source = dbus_source;
  handler->watch = watch;

  handler->source = __g_fd_source_new (dbus_watch_get_fd (watch),
				       condition, NULL);
  g_source_set_callback (handler->source,
			 (GSourceFunc) io_handler_dispatch, handler,
                         NULL);
  g_source_attach (handler->source, NULL);
 
  dbus_source->ios = g_slist_prepend (dbus_source->ios, handler);
  dbus_watch_set_data (watch, handler,
		       (DBusFreeFunction)io_handler_free);
}

static void
dbus_source_remove_watch (DBusSource *dbus_source,
			  DBusWatch *watch)
{
  dbus_watch_set_data (watch, NULL, NULL);
}

static void
timeout_handler_free (TimeoutHandler *handler)
{
  DBusSource *dbus_source;

  dbus_source = handler->dbus_source;
  dbus_source->timeouts = g_slist_remove (dbus_source->timeouts, handler);

  g_source_destroy (handler->source);
  g_source_unref (handler->source);
  g_free (handler);
}

static gboolean
timeout_handler_dispatch (gpointer      data)
{
  TimeoutHandler *handler = data;

  dbus_timeout_handle (handler->timeout);
  
  return TRUE;
}

static void
dbus_source_add_timeout (DBusSource *dbus_source,
			 DBusTimeout *timeout)
{
  TimeoutHandler *handler;
  
  if (!dbus_timeout_get_enabled (timeout))
    return;
  
  g_assert (dbus_timeout_get_data (timeout) == NULL);

  handler = g_new0 (TimeoutHandler, 1);
  handler->dbus_source = dbus_source;
  handler->timeout = timeout;

  handler->source = g_timeout_source_new (dbus_timeout_get_interval (timeout));
  g_source_set_callback (handler->source,
			 timeout_handler_dispatch, handler,
                         NULL);
  g_source_attach (handler->source, NULL);

  /* handler->source is owned by the context here */
  dbus_source->timeouts = g_slist_prepend (dbus_source->timeouts, handler);

  dbus_timeout_set_data (timeout, handler,
			 (DBusFreeFunction)timeout_handler_free);
}

static void
dbus_source_remove_timeout (DBusSource *source,
			    DBusTimeout *timeout)
{
  dbus_timeout_set_data (timeout, NULL, NULL);
}

static dbus_bool_t
add_watch (DBusWatch *watch,
	   gpointer   data)
{
  DBusSource *dbus_source = data;

  dbus_source_add_watch (dbus_source, watch);
  
  return TRUE;
}

static void
remove_watch (DBusWatch *watch,
	      gpointer   data)
{
  DBusSource *dbus_source = data;

  dbus_source_remove_watch (dbus_source, watch);
}

static void
watch_toggled (DBusWatch *watch,
               void      *data)
{
  /* Because we just exit on OOM, enable/disable is
   * no different from add/remove */
  if (dbus_watch_get_enabled (watch))
    add_watch (watch, data);
  else
    remove_watch (watch, data);
}

static dbus_bool_t
add_timeout (DBusTimeout *timeout,
	     void        *data)
{
  DBusSource *source = data;
  
  if (!dbus_timeout_get_enabled (timeout))
    return TRUE;

  dbus_source_add_timeout (source, timeout);

  return TRUE;
}

static void
remove_timeout (DBusTimeout *timeout,
		void        *data)
{
  DBusSource *source = data;

  dbus_source_remove_timeout (source, timeout);
}

static void
timeout_toggled (DBusTimeout *timeout,
                 void        *data)
{
  /* Because we just exit on OOM, enable/disable is
   * no different from add/remove
   */
  if (dbus_timeout_get_enabled (timeout))
    add_timeout (timeout, data);
  else
    remove_timeout (timeout, data);
}

static void
wakeup_main (void *data)
{
  g_main_context_wakeup (NULL);
}

static const GSourceFuncs dbus_source_funcs = {
  dbus_source_prepare,
  dbus_source_check,
  dbus_source_dispatch,
};

/* Called when the connection dies or when we're unintegrating from mainloop */
static void
dbus_source_free (DBusSource *dbus_source)
{
  while (dbus_source->ios)
    {
      IOHandler *handler = dbus_source->ios->data;
      
      dbus_watch_set_data (handler->watch, NULL, NULL);
    }

  while (dbus_source->timeouts)
    {
      TimeoutHandler *handler = dbus_source->timeouts->data;
      
      dbus_timeout_set_data (handler->timeout, NULL, NULL);
    }

  /* Remove from mainloop */
  g_source_destroy ((GSource *)dbus_source);

  g_source_unref ((GSource *)dbus_source);
}

void
_g_dbus_connection_integrate_with_main  (DBusConnection *connection)
{
  DBusSource *dbus_source;

  g_once (&once_init_main_integration, main_integration_init, NULL);
  
  g_assert (connection != NULL);

  _g_dbus_connection_remove_from_main (connection);

  dbus_source = (DBusSource *)
    g_source_new ((GSourceFuncs*)&dbus_source_funcs,
		  sizeof (DBusSource));
  
  dbus_source->connection = connection;
  
  if (!dbus_connection_set_watch_functions (connection,
                                            add_watch,
                                            remove_watch,
                                            watch_toggled,
                                            dbus_source, NULL))
    _g_dbus_oom ();

  if (!dbus_connection_set_timeout_functions (connection,
                                              add_timeout,
                                              remove_timeout,
                                              timeout_toggled,
                                              dbus_source, NULL))
    _g_dbus_oom ();
    
  dbus_connection_set_wakeup_main_function (connection,
					    wakeup_main,
					    dbus_source, NULL);

  /* Owned by both connection and mainloop (until destroy) */
  g_source_attach ((GSource *)dbus_source, NULL);

  if (!dbus_connection_set_data (connection,
				 main_integration_data_slot,
				 dbus_source, (DBusFreeFunction)dbus_source_free))
    _g_dbus_oom ();
}

void
_g_dbus_connection_remove_from_main (DBusConnection *connection)
{
  g_once (&once_init_main_integration, main_integration_init, NULL);

  if (!dbus_connection_set_data (connection,
				 main_integration_data_slot,
				 NULL, NULL))
    _g_dbus_oom ();
}
