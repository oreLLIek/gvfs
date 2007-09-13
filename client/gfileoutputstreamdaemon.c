#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <gio/gvfserror.h>
#include <gio/gseekable.h>
#include "gfileoutputstreamdaemon.h"
#include "gvfsdaemondbus.h"
#include <gio/gfileinfolocal.h>
#include <gio/ginputstreamsocket.h>
#include <gio/goutputstreamsocket.h>
#include <gvfsdaemonprotocol.h>

#define MAX_WRITE_SIZE (4*1024*1024)

typedef enum {
  STATE_OP_DONE,
  STATE_OP_READ,
  STATE_OP_WRITE,
  STATE_OP_SKIP,
} StateOp;

typedef enum {
  WRITE_STATE_INIT = 0,
  WRITE_STATE_WROTE_COMMAND,
  WRITE_STATE_SEND_DATA,
  WRITE_STATE_HANDLE_INPUT,
} WriteState;

typedef struct {
  WriteState state;

  /* Output */
  char *buffer;
  gsize buffer_size;
  gsize buffer_pos;
  
  /* Input */
  gssize ret_val;
  GError *ret_error;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} WriteOperation;

typedef enum {
  SEEK_STATE_INIT = 0,
  SEEK_STATE_WROTE_REQUEST,
  SEEK_STATE_HANDLE_INPUT,
} SeekState;

typedef struct {
  SeekState state;

  /* Output */
  goffset offset;
  GSeekType seek_type;
  /* Output */
  gboolean ret_val;
  GError *ret_error;
  goffset ret_offset;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} SeekOperation;

typedef enum {
  CLOSE_STATE_INIT = 0,
  CLOSE_STATE_WROTE_REQUEST,
  CLOSE_STATE_HANDLE_INPUT,
} CloseState;

typedef struct {
  CloseState state;

  /* Output */
  
  /* Output */
  gboolean ret_val;
  GError *ret_error;
  
  gboolean sent_cancel;
  
  guint32 seq_nr;
} CloseOperation;


typedef struct {
  gboolean cancelled;
  
  char *io_buffer;
  gsize io_size;
  gsize io_res;
  /* The operation always succeeds, or gets cancelled.
     If we get an error doing the i/o that is considered fatal */
  gboolean io_allow_cancel;
  gboolean io_cancelled;
} IOOperationData;

typedef StateOp (*state_machine_iterator) (GFileOutputStreamDaemon *file, IOOperationData *io_op, gpointer data);

struct _GFileOutputStreamDaemon {
  GFileOutputStream parent;

  GOutputStream *command_stream;
  GInputStream *data_stream;
  guint can_seek : 1;
  
  guint32 seq_nr;
  goffset current_offset;

  gsize input_block_size;
  GString *input_buffer;
  
  GString *output_buffer;
};

static gssize     g_file_output_stream_daemon_write         (GOutputStream              *stream,
							     void                       *buffer,
							     gsize                       count,
							     GCancellable               *cancellable,
							     GError                    **error);
static gboolean   g_file_output_stream_daemon_close         (GOutputStream              *stream,
							     GCancellable               *cancellable,
							     GError                    **error);
static GFileInfo *g_file_output_stream_daemon_get_file_info (GFileOutputStream          *stream,
							     GFileInfoRequestFlags       requested,
							     char                       *attributes,
							     GCancellable               *cancellable,
							     GError                    **error);
static goffset    g_file_output_stream_daemon_tell          (GFileOutputStream          *stream);
static gboolean   g_file_output_stream_daemon_can_seek      (GFileOutputStream          *stream);
static gboolean   g_file_output_stream_daemon_seek          (GFileOutputStream          *stream,
							     goffset                     offset,
							     GSeekType                   type,
							     GCancellable               *cancellable,
							     GError                    **error);
static void       g_file_output_stream_daemon_write_async   (GOutputStream              *stream,
							     void                       *buffer,
							     gsize                       count,
							     int                         io_priority,
							     GAsyncWriteCallback         callback,
							     gpointer                    data,
							     GCancellable               *cancellable);
static void       g_file_output_stream_daemon_close_async   (GOutputStream              *stream,
							     int                         io_priority,
							     GAsyncCloseOutputCallback   callback,
							     gpointer                    data,
							     GCancellable               *cancellable);

G_DEFINE_TYPE (GFileOutputStreamDaemon, g_file_output_stream_daemon,
	       G_TYPE_FILE_OUTPUT_STREAM)

static void
g_file_output_stream_daemon_finalize (GObject *object)
{
  GFileOutputStreamDaemon *file;
  
  file = G_FILE_OUTPUT_STREAM_DAEMON (object);

  if (file->command_stream)
    g_object_unref (file->command_stream);
  if (file->data_stream)
    g_object_unref (file->data_stream);
  
  if (G_OBJECT_CLASS (g_file_output_stream_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_file_output_stream_daemon_parent_class)->finalize) (object);
}

static void
g_file_output_stream_daemon_class_init (GFileOutputStreamDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GOutputStreamClass *stream_class = G_OUTPUT_STREAM_CLASS (klass);
  GFileOutputStreamClass *file_stream_class = G_FILE_OUTPUT_STREAM_CLASS (klass);
  
  gobject_class->finalize = g_file_output_stream_daemon_finalize;

  stream_class->write = g_file_output_stream_daemon_write;
  stream_class->close = g_file_output_stream_daemon_close;
  
  stream_class->write_async = g_file_output_stream_daemon_write_async;
  stream_class->close_async = g_file_output_stream_daemon_close_async;
  
  file_stream_class->tell = g_file_output_stream_daemon_tell;
  file_stream_class->can_seek = g_file_output_stream_daemon_can_seek;
  file_stream_class->seek = g_file_output_stream_daemon_seek;
  file_stream_class->get_file_info = g_file_output_stream_daemon_get_file_info;

}

static void
g_file_output_stream_daemon_init (GFileOutputStreamDaemon *info)
{
  info->output_buffer = g_string_new ("");
  info->output_buffer = g_string_new ("");
}

GFileOutputStream *
g_file_output_stream_daemon_new (int fd,
				 gboolean can_seek,
				 goffset initial_offset)
{
  GFileOutputStreamDaemon *stream;

  stream = g_object_new (G_TYPE_FILE_OUTPUT_STREAM_DAEMON, NULL);

  stream->command_stream = g_output_stream_socket_new (fd, FALSE);
  stream->data_stream = g_input_stream_socket_new (fd, TRUE);
  stream->can_seek = can_seek;
  stream->current_offset = initial_offset;
  
  return G_FILE_OUTPUT_STREAM (stream);
}

static gboolean
error_is_cancel (GError *error)
{
  return error != NULL &&
    error->domain == G_VFS_ERROR &&
    error->code == G_VFS_ERROR_CANCELLED;
}

static void
append_request (GFileOutputStreamDaemon *stream, guint32 command,
		guint32 arg1, guint32 arg2, guint32 data_len, guint32 *seq_nr)
{
  GVfsDaemonSocketProtocolRequest cmd;

  g_assert (sizeof (cmd) == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
  
  if (seq_nr)
    *seq_nr = stream->seq_nr;
  
  cmd.command = g_htonl (command);
  cmd.seq_nr = g_htonl (stream->seq_nr++);
  cmd.arg1 = g_htonl (arg1);
  cmd.arg2 = g_htonl (arg2);
  cmd.data_len = data_len;

  g_string_append_len (stream->output_buffer,
		       (char *)&cmd, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE);
}

static gsize
get_reply_header_missing_bytes (GString *buffer)
{
  GVfsDaemonSocketProtocolReply *reply;
  guint32 type;
  guint32 arg2;
  
  if (buffer->len < G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE)
    return G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE - buffer->len;

  reply = (GVfsDaemonSocketProtocolReply *)buffer->str;

  type = g_ntohl (reply->type);
  arg2 = g_ntohl (reply->arg2);
  
  if (type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR)
    return G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE + arg2 - buffer->len;
  return 0;
}

static char *
decode_reply (GString *buffer, GVfsDaemonSocketProtocolReply *reply_out)
{
  GVfsDaemonSocketProtocolReply *reply;
  reply = (GVfsDaemonSocketProtocolReply *)buffer->str;
  reply_out->type = g_ntohl (reply->type);
  reply_out->seq_nr = g_ntohl (reply->seq_nr);
  reply_out->arg1 = g_ntohl (reply->arg1);
  reply_out->arg2 = g_ntohl (reply->arg2);
  
  return buffer->str + G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE;
}

static void
decode_error (GVfsDaemonSocketProtocolReply *reply, char *data, GError **error)
{
  g_set_error (error,
	       g_quark_from_string (data),
	       reply->arg1,
	       data + strlen (data) + 1);
}


static gboolean
run_sync_state_machine (GFileOutputStreamDaemon *file,
			state_machine_iterator iterator,
			gpointer data,
			GCancellable *cancellable,
			GError **error)
{
  gssize res;
  StateOp io_op;
  IOOperationData io_data;
  GError *io_error;

  memset (&io_data, 0, sizeof (io_data));
  
  while (TRUE)
    {
      if (cancellable)
	io_data.cancelled = g_cancellable_is_cancelled (cancellable);
      
      io_op = iterator (file, &io_data, data);

      if (io_op == STATE_OP_DONE)
	return TRUE;
      
      io_error = NULL;
      if (io_op == STATE_OP_READ)
	{
	  res = g_input_stream_read (file->data_stream,
				     io_data.io_buffer, io_data.io_size,
				     io_data.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_SKIP)
	{
	  res = g_input_stream_skip (file->data_stream,
				     io_data.io_size,
				     io_data.io_allow_cancel ? cancellable : NULL,
				     &io_error);
	}
      else if (io_op == STATE_OP_WRITE)
	{
	  res = g_output_stream_write (file->command_stream,
				       io_data.io_buffer, io_data.io_size,
				       io_data.io_allow_cancel ? cancellable : NULL,
				       &io_error);
	}
      else
	g_assert_not_reached ();

      if (res == -1)
	{
	  if (error_is_cancel (io_error))
	    {
	      io_data.io_res = 0;
	      io_data.io_cancelled = TRUE;
	      g_error_free (io_error);
	    }
	  else
	    {
	      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
			   _("Error in stream protocol: %s"), io_error->message);
	      g_error_free (io_error);
	      return FALSE;
	    }
	}
      else if (res == 0 && io_data.io_size != 0)
	{
	  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       _("Error in stream protocol: %s"), _("End of stream"));
	  return FALSE;
	}
      else
	{
	  io_data.io_res = res;
	  io_data.io_cancelled = FALSE;
	}
    }
}

/* read cycle:

   if we know of a (partially read) matching outstanding block, read from it
   create packet, append to outgoing
   flush outgoing
   start processing output, looking for a data block with same seek gen,
    or an error with same seq nr
   on cancel, send cancel command and go back to loop
 */

static StateOp
iterate_write_state_machine (GFileOutputStreamDaemon *file, IOOperationData *io_op, WriteOperation *op)
{
  gsize len;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case WRITE_STATE_INIT:
	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_WRITE,
			  op->buffer_size, 0, op->buffer_size, &op->seq_nr);
	  op->state = WRITE_STATE_WROTE_COMMAND;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case WRITE_STATE_WROTE_COMMAND:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }
	  
	  if (io_op->io_res < file->output_buffer->len)
	    {
	      memcpy (file->output_buffer->str,
		      file->output_buffer->str + io_op->io_res,
		      file->output_buffer->len - io_op->io_res);
	      g_string_truncate (file->output_buffer,
				 file->output_buffer->len - io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->buffer_pos = 0;
	  if (op->sent_cancel)
	    op->state = WRITE_STATE_HANDLE_INPUT;
	  else
	    op->state = WRITE_STATE_SEND_DATA;
	  break;

	  /* No op */
	case WRITE_STATE_SEND_DATA:
	  op->buffer_pos += io_op->io_res;
	  
	  if (op->buffer_pos < op->buffer_size)
	    {
	      io_op->io_buffer = op->buffer + op->buffer_pos;
	      io_op->io_size = op->buffer_size - op->buffer_pos;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  op->state = WRITE_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case WRITE_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = WRITE_STATE_WROTE_COMMAND;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = -1;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_WRITTEN)
	      {
		op->ret_val = reply.arg1;
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = WRITE_STATE_HANDLE_INPUT;
	  break;
	  
	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
 
    }
}

static gssize
g_file_output_stream_daemon_write (GOutputStream *stream,
				   void         *buffer,
				   gsize         count,
				   GCancellable *cancellable,
				   GError      **error)
{
  GFileOutputStreamDaemon *file;
  WriteOperation op;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);

  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return -1;
    }
  
  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_WRITE_SIZE)
    count = MAX_WRITE_SIZE;

  memset (&op, 0, sizeof (op));
  op.state = WRITE_STATE_INIT;
  op.buffer = buffer;
  op.buffer_size = count;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_write_state_machine,
			       &op, cancellable, error))
    return -1; /* IO Error */

  if (op.ret_val == -1)
    g_propagate_error (error, op.ret_error);
  else
    file->current_offset += op.ret_val;
  
  return op.ret_val;
}

static StateOp
iterate_close_state_machine (GFileOutputStreamDaemon *file, IOOperationData *io_op, CloseOperation *op)
{
  gsize len;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case CLOSE_STATE_INIT:
	  append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE,
			  0, 0, 0, &op->seq_nr);
	  op->state = CLOSE_STATE_WROTE_REQUEST;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case CLOSE_STATE_WROTE_REQUEST:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = FALSE;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }

	  if (io_op->io_res < file->output_buffer->len)
	    {
	      memcpy (file->output_buffer->str,
		      file->output_buffer->str + io_op->io_res,
		      file->output_buffer->len - io_op->io_res);
	      g_string_truncate (file->output_buffer,
				 file->output_buffer->len - io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->state = CLOSE_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case CLOSE_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = CLOSE_STATE_WROTE_REQUEST;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = FALSE;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED)
	      {
		op->ret_val = TRUE;
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = CLOSE_STATE_HANDLE_INPUT;
	  break;

	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
    }
}


static gboolean
g_file_output_stream_daemon_close (GOutputStream *stream,
				  GCancellable *cancellable,
				  GError      **error)
{
  GFileOutputStreamDaemon *file;
  CloseOperation op;
  gboolean res;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);

  /* We need to do a full roundtrip to guarantee that the writes have
     reached the disk. */

  memset (&op, 0, sizeof (op));
  op.state = CLOSE_STATE_INIT;

  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_close_state_machine,
			       &op, cancellable, error))
    res = FALSE;
  else
    {
      if (!op.ret_val)
	g_propagate_error (error, op.ret_error);
      res = op.ret_val;
    }

  /* Return the first error, but close all streams */
  if (res)
    res = g_output_stream_close (file->command_stream, cancellable, error);
  else
    g_output_stream_close (file->command_stream, cancellable, NULL);
    
  if (res)
    res = g_input_stream_close (file->data_stream, cancellable, error);
  else
    g_input_stream_close (file->data_stream, cancellable, NULL);
  
  return res;
}

static goffset
g_file_output_stream_daemon_tell (GFileOutputStream *stream)
{
  GFileOutputStreamDaemon *file;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);
  
  return file->current_offset;
}

static gboolean
g_file_output_stream_daemon_can_seek (GFileOutputStream *stream)
{
  GFileOutputStreamDaemon *file;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);

  return file->can_seek;
}

static StateOp
iterate_seek_state_machine (GFileOutputStreamDaemon *file, IOOperationData *io_op, SeekOperation *op)
{
  gsize len;
  guint32 request;

  while (TRUE)
    {
      switch (op->state)
	{
	  /* Initial state for read op */
	case SEEK_STATE_INIT:
	  request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET;
	  if (op->seek_type == G_SEEK_CUR)
	    request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR;
	  else if (op->seek_type == G_SEEK_END)
	    request = G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END;
	  append_request (file, request,
			  op->offset & 0xffffffff,
			  op->offset >> 32,
			  0,
			  &op->seq_nr);
	  op->state = SEEK_STATE_WROTE_REQUEST;
	  io_op->io_buffer = file->output_buffer->str;
	  io_op->io_size = file->output_buffer->len;
	  io_op->io_allow_cancel = TRUE; /* Allow cancel before first byte of request sent */
	  return STATE_OP_WRITE;

	  /* wrote parts of output_buffer */
	case SEEK_STATE_WROTE_REQUEST:
	  if (io_op->io_cancelled)
	    {
	      op->ret_val = -1;
	      g_set_error (&op->ret_error,
			   G_VFS_ERROR,
			   G_VFS_ERROR_CANCELLED,
			   _("Operation was cancelled"));
	      return STATE_OP_DONE;
	    }

	  if (io_op->io_res < file->output_buffer->len)
	    {
	      memcpy (file->output_buffer->str,
		      file->output_buffer->str + io_op->io_res,
		      file->output_buffer->len - io_op->io_res);
	      g_string_truncate (file->output_buffer,
				 file->output_buffer->len - io_op->io_res);
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }
	  g_string_truncate (file->output_buffer, 0);

	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;

	  /* No op */
	case SEEK_STATE_HANDLE_INPUT:
	  if (io_op->cancelled && !op->sent_cancel)
	    {
	      op->sent_cancel = TRUE;
	      append_request (file, G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL,
			      op->seq_nr, 0, 0, NULL);
	      op->state = SEEK_STATE_WROTE_REQUEST;
	      io_op->io_buffer = file->output_buffer->str;
	      io_op->io_size = file->output_buffer->len;
	      io_op->io_allow_cancel = FALSE;
	      return STATE_OP_WRITE;
	    }

	  if (io_op->io_res > 0)
	    {
	      gsize unread_size = io_op->io_size - io_op->io_res;
	      g_string_set_size (file->input_buffer,
				 file->input_buffer->len - unread_size);
	    }
	  
	  len = get_reply_header_missing_bytes (file->input_buffer);
	  if (len > 0)
	    {
	      gsize current_len = file->input_buffer->len;
	      g_string_set_size (file->input_buffer,
				 current_len + len);
	      io_op->io_buffer = file->input_buffer->str + current_len;
	      io_op->io_size = len;
	      io_op->io_allow_cancel = !op->sent_cancel;
	      return STATE_OP_READ;
	    }

	  /* Got full header */

	  {
	    GVfsDaemonSocketProtocolReply reply;
	    char *data;
	    data = decode_reply (file->input_buffer, &reply);

	    if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR &&
		reply.seq_nr == op->seq_nr)
	      {
		op->ret_val = FALSE;
		decode_error (&reply, data, &op->ret_error);
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    else if (reply.type == G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS)
	      {
		op->ret_val = TRUE;
		op->ret_offset = ((goffset)reply.arg2) << 32 | (goffset)reply.arg1;
		g_string_truncate (file->input_buffer, 0);
		return STATE_OP_DONE;
	      }
	    /* Ignore other reply types */
	  }

	  g_string_truncate (file->input_buffer, 0);
	  
	  /* This wasn't interesting, read next reply */
	  op->state = SEEK_STATE_HANDLE_INPUT;
	  break;

	default:
	  g_assert_not_reached ();
	}
      
      /* Clear io_op between non-op state switches */
      io_op->io_size = 0;
      io_op->io_res = 0;
      io_op->io_cancelled = FALSE;
    }
}

static gboolean
g_file_output_stream_daemon_seek (GFileOutputStream *stream,
				 goffset offset,
				 GSeekType type,
				 GCancellable *cancellable,
				 GError **error)
{
  GFileOutputStreamDaemon *file;
  SeekOperation op;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);

  if (!file->can_seek)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_NOT_SUPPORTED,
		   _("Seek not supported on stream"));
      return FALSE;
    }
  
  if (g_cancellable_is_cancelled (cancellable))
    {
      g_set_error (error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
      return FALSE;
    }
  
  memset (&op, 0, sizeof (op));
  op.state = SEEK_STATE_INIT;
  op.offset = offset;
  op.seek_type = type;
  
  if (!run_sync_state_machine (file, (state_machine_iterator)iterate_seek_state_machine,
			       &op, cancellable, error))
    return -1; /* IO Error */

  if (!op.ret_val)
    g_propagate_error (error, op.ret_error);
  else
    file->current_offset = op.ret_offset;
  
  return op.ret_val;
}

static GFileInfo *
g_file_output_stream_daemon_get_file_info (GFileOutputStream     *stream,
					  GFileInfoRequestFlags requested,
					  char                 *attributes,
					  GCancellable         *cancellable,
					  GError              **error)
{
  GFileOutputStreamDaemon *file;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);

  return NULL;
}

/************************************************************************
 *         Async I/O Code                                               *
 ************************************************************************/

typedef struct AsyncIterator AsyncIterator;

typedef void (*AsyncIteratorDone) (GOutputStream *stream,
				   gpointer op_data,
				   gpointer callback,
				   gpointer callback_data,
				   GError *io_error);

struct AsyncIterator {
  AsyncIteratorDone done_cb;
  GFileOutputStreamDaemon *file;
  GCancellable *cancellable;
  IOOperationData io_data;
  state_machine_iterator iterator;
  gpointer iterator_data;
  int io_priority;
  gpointer callback;
  gpointer callback_data;
};

static void async_iterate (AsyncIterator *iterator);

static void
async_iterator_done (AsyncIterator *iterator, GError *io_error)
{
  iterator->done_cb (G_OUTPUT_STREAM (iterator->file),
		     iterator->iterator_data,
		     iterator->callback,
		     iterator->callback_data,
		     io_error);

  g_free (iterator);
  
}

static void
async_op_handle (AsyncIterator *iterator,
		 gssize res,
		 GError *io_error)
{
  IOOperationData *io_data = &iterator->io_data;
  GError *error;
  
  if (io_error != NULL)
    {
      if (error_is_cancel (io_error))
	{
	  io_data->io_res = 0;
	  io_data->io_cancelled = TRUE;
	}
      else
	{
	  error = NULL;
	  g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		       _("Error in stream protocol: %s"), io_error->message);
	  async_iterator_done (iterator, error);
	  g_error_free (error);
	  return;
	}
    }
  else if (res == 0 && io_data->io_size != 0)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_IO,
		   _("Error in stream protocol: %s"), _("End of stream"));
      async_iterator_done (iterator, error);
      g_error_free (error);
      return;
    }
  else
    {
      io_data->io_res = res;
      io_data->io_cancelled = FALSE;
    }
  
  async_iterate (iterator);
}

static void
async_read_op_callback (GInputStream *stream,
			void         *buffer,
			gsize         count_requested,
			gssize        count_read,
			gpointer      data,
			GError       *error)
{
  async_op_handle ((AsyncIterator *)data, count_read, error);
}

static void
async_skip_op_callback (GInputStream *stream,
			gsize         count_requested,
			gssize        count_skipped,
			gpointer      data,
			GError       *error)
{
  async_op_handle ((AsyncIterator *)data, count_skipped, error);
}

static void
async_write_op_callback (GOutputStream *stream,
			 void          *buffer,
			 gsize          bytes_requested,
			 gssize         bytes_written,
			 gpointer       data,
			 GError        *error)
{
  async_op_handle ((AsyncIterator *)data, bytes_written, error);
}

static void
async_iterate (AsyncIterator *iterator)
{
  IOOperationData *io_data = &iterator->io_data;
  GFileOutputStreamDaemon *file = iterator->file;
  StateOp io_op;
  
  io_data->cancelled =
    g_cancellable_is_cancelled (iterator->cancellable);

  io_op = iterator->iterator (file, io_data, iterator->iterator_data);

  if (io_op == STATE_OP_DONE)
    {
      async_iterator_done (iterator, NULL);
      return;
    }

  /* TODO: Handle allow_cancel... */
  
  if (io_op == STATE_OP_READ)
    {
      g_input_stream_read_async (file->data_stream,
				 io_data->io_buffer, io_data->io_size,
				 iterator->io_priority,
				 async_read_op_callback, iterator,
				 io_data->io_allow_cancel ? iterator->cancellable : NULL);
    }
  else if (io_op == STATE_OP_SKIP)
    {
      g_input_stream_skip_async (file->data_stream,
				 io_data->io_size,
				 iterator->io_priority,
				 async_skip_op_callback, iterator,
				 io_data->io_allow_cancel ? iterator->cancellable : NULL);
    }
  else if (io_op == STATE_OP_WRITE)
    {
      g_output_stream_write_async (file->command_stream,
				   io_data->io_buffer, io_data->io_size,
				   iterator->io_priority,
				   async_write_op_callback, iterator,
				   io_data->io_allow_cancel ? iterator->cancellable : NULL);
    }
  else
    g_assert_not_reached ();
}

static void
run_async_state_machine (GFileOutputStreamDaemon *file,
			 state_machine_iterator iterator_cb,
			 gpointer iterator_data,
			 int io_priority,
			 gpointer callback,
			 gpointer data,
			 GCancellable *cancellable,
			 AsyncIteratorDone done_cb)
{
  AsyncIterator *iterator;

  iterator = g_new0 (AsyncIterator, 1);
  iterator->file = file;
  iterator->iterator = iterator_cb;
  iterator->iterator_data = iterator_data;
  iterator->io_priority = io_priority;
  iterator->cancellable = cancellable;
  iterator->callback = callback;
  iterator->callback_data = data;
  iterator->done_cb = done_cb;

  async_iterate (iterator);
}

static void
async_write_done (GOutputStream *stream,
		  gpointer op_data,
		  gpointer callback,
		  gpointer callback_data,
		  GError *io_error)
{
  WriteOperation *op;
  gssize count_written;
  GError *error;

  op = op_data;

  if (io_error)
    {
      count_written = -1;
      error = io_error;
    }
  else
    {
      count_written = op->ret_val;
      error = op->ret_error;
    }

  if (callback)
    ((GAsyncWriteCallback)callback) (stream,
				     op->buffer,
				     op->buffer_size,
				     count_written,
				     callback_data,
				     error);

  if (op->ret_error)
    g_error_free (op->ret_error);
  g_free (op);
}

static void
g_file_output_stream_daemon_write_async  (GOutputStream      *stream,
					  void               *buffer,
					  gsize               count,
					  int                 io_priority,
					  GAsyncWriteCallback callback,
					  gpointer            data,
					  GCancellable       *cancellable)
{
  GFileOutputStreamDaemon *file;
  AsyncIterator *iterator;
  WriteOperation *op;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);
  
  /* Limit for sanity and to avoid 32bit overflow */
  if (count > MAX_WRITE_SIZE)
    count = MAX_WRITE_SIZE;

  op = g_new0 (WriteOperation, 1);
  op->state = WRITE_STATE_INIT;
  op->buffer = buffer;
  op->buffer_size = count;

  iterator = g_new0 (AsyncIterator, 1);
  
  run_async_state_machine (file,
			   (state_machine_iterator)iterate_write_state_machine,
			   op,
			   io_priority,
			   callback, data,
			   cancellable,
			   async_write_done);
}

static void
async_close_done (GOutputStream *stream,
		  gpointer op_data,
		  gpointer callback,
		  gpointer callback_data,
		  GError *io_error)
{
  GFileOutputStreamDaemon *file;
  CloseOperation *op;
  gboolean result;
  GError *error;
  GCancellable *cancellable = NULL; /* TODO: get cancellable */

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);
  
  op = op_data;

  if (io_error)
    {
      result = FALSE;
      error = io_error;
    }
  else
    {
      result = op->ret_val;
      error = op->ret_error;
    }

  if (result)
    result = g_output_stream_close (file->command_stream, cancellable, &error);
  else
    g_output_stream_close (file->command_stream, cancellable, NULL);
    
  if (result)
    result = g_input_stream_close (file->data_stream, cancellable, &error);
  else
    g_input_stream_close (file->data_stream, cancellable, NULL);
  
  if (callback)
    ((GAsyncCloseOutputCallback)callback) (stream,
					  result,
					  callback_data,
					  error);
  
  if (op->ret_error)
    g_error_free (op->ret_error);
  g_free (op);
}

static void
g_file_output_stream_daemon_close_async (GOutputStream        *stream,
					int                  io_priority,
					GAsyncCloseOutputCallback callback,
					gpointer            data,
					GCancellable       *cancellable)
{
  GFileOutputStreamDaemon *file;
  AsyncIterator *iterator;
  CloseOperation *op;

  file = G_FILE_OUTPUT_STREAM_DAEMON (stream);
  
  op = g_new0 (CloseOperation, 1);
  op->state = CLOSE_STATE_INIT;

  iterator = g_new0 (AsyncIterator, 1);
  
  run_async_state_machine (file,
			   (state_machine_iterator)iterate_close_state_machine,
			   op, io_priority,
			   callback, data,
			   cancellable,
			   async_close_done);
}