#include <config.h>
#include <glib/gi18n-lib.h>

#include "goutputstream.h"
#include "gioscheduler.h"
#include "gasynchelper.h"

G_DEFINE_TYPE (GOutputStream, g_output_stream, G_TYPE_OBJECT);

struct _GOutputStreamPrivate {
  guint closed : 1;
  guint pending : 1;
  guint cancelled : 1;
  gint io_job_id;
  gpointer outstanding_callback;
};

static void g_output_stream_real_write_async (GOutputStream             *stream,
					      void                      *buffer,
					      gsize                      count,
					      int                        io_priority,
					      GAsyncWriteCallback        callback,
					      gpointer                   data,
					      GCancellable              *cancellable);
static void g_output_stream_real_flush_async (GOutputStream             *stream,
					      int                        io_priority,
					      GAsyncFlushCallback        callback,
					      gpointer                   data,
					      GCancellable              *cancellable);
static void g_output_stream_real_close_async (GOutputStream             *stream,
					      int                        io_priority,
					      GAsyncCloseOutputCallback  callback,
					      gpointer                   data,
					      GCancellable              *cancellable);

static void
g_output_stream_finalize (GObject *object)
{
  GOutputStream *stream;

  stream = G_OUTPUT_STREAM (object);
  
  if (!stream->priv->closed)
    g_output_stream_close (stream, NULL, NULL);
  
  if (G_OBJECT_CLASS (g_output_stream_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_output_stream_parent_class)->finalize) (object);
}

static void
g_output_stream_class_init (GOutputStreamClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GOutputStreamPrivate));
  
  gobject_class->finalize = g_output_stream_finalize;
  
  klass->write_async = g_output_stream_real_write_async;
  klass->flush_async = g_output_stream_real_flush_async;
  klass->close_async = g_output_stream_real_close_async;
}

static void
g_output_stream_init (GOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
					      G_TYPE_OUTPUT_STREAM,
					      GOutputStreamPrivate);
}

/**
 * g_output_stream_write:
 * @stream: a #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to write @count bytes from @buffer into the stream. Will block
 * during the operation.
 * 
 * If count is zero returns zero and does nothing. A value of @count
 * larger than %G_MAXSSIZE will cause a %G_FILE_ERROR_INVAL error.
 *
 * On success, the number of bytes written to the stream is returned.
 * It is not an error if this is not the same as the requested size, as it
 * can happen e.g. on a partial i/o error, or if the there is not enough
 * storage in the stream. All writes either block until at least one byte
 * is written, so zero is never returned (unless @count is zero).
 * 
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_VFS_ERROR_CANCELLED will be returned. If an
 * operation was partially finished when the operation was cancelled the
 * partial result will be returned, without an error.
 *
 * On error -1 is returned and @error is set accordingly.
 * 
 * Return value: Number of bytes written, or -1 on error
 **/
gssize
g_output_stream_write (GOutputStream *stream,
		       void          *buffer,
		       gsize          count,
		       GCancellable  *cancellable,
		       GError       **error)
{
  GOutputStreamClass *class;
  gssize res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);
  g_return_val_if_fail (buffer != NULL, 0);

  if (count == 0)
    return 0;
  
  if (((gssize) count) < 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
		   _("Too large count value passed to g_output_stream_write"));
      return -1;
    }

  if (stream->priv->closed)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return -1;
    }
  
  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return -1;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  if (class->write == NULL) 
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_INTERNAL_ERROR,
		   _("Output stream doesn't implement write"));
      return -1;
    }
  
  if (cancellable)
    g_push_current_cancellable (cancellable);
  
  stream->priv->pending = TRUE;
  res = class->write (stream, buffer, count, cancellable, error);
  stream->priv->pending = FALSE;
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  return res; 
}

/**
 * g_output_stream_write_all:
 * @stream: a #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @bytes_written: location to store the number of bytes that was written to the stream
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Tries to write @count bytes from @buffer into the stream. Will block
 * during the operation.
 * 
 * This function is similar to g_output_stream_write(), except it tries to
 * read as many bytes as requested, only stopping on an error.
 *
 * On a successful write of @count bytes, TRUE is returned, and @bytes_written
 * is set to @count.
 * 
 * If there is an error during the operation FALSE is returned and @error
 * is set to indicate the error status, @bytes_written is updated to contain
 * the number of bytes written into the stream before the error occured.
 *
 * Return value: TRUE on success, FALSE if there was an error
 **/
gssize
g_output_stream_write_all (GOutputStream *stream,
			   void          *buffer,
			   gsize          count,
			   gsize         *bytes_written,
			   GCancellable  *cancellable,
			   GError       **error)
{
  gsize _bytes_written;
  gssize res;

  _bytes_written = 0;
  while (_bytes_written < count)
    {
      res = g_output_stream_write (stream, (char *)buffer + _bytes_written, count - _bytes_written,
				   cancellable, error);
      if (res == -1)
	{
	  *bytes_written = _bytes_written;
	  return FALSE;
	}
      
      if (res == 0)
	g_warning ("Write returned zero without error");

      _bytes_written += res;
    }
  
  *bytes_written = _bytes_written;
  return TRUE;
}

/**
 * g_output_stream_flush:
 * @stream: a #GOutputStream.
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Flushed any outstanding buffers in the stream. Will block during the operation.
 * Closing the stream will implicitly cause a flush.
 *
 * This function is optional for inherited classes.
 * 
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_VFS_ERROR_CANCELLED will be returned.
 *
 * Return value: TRUE on success, FALSE on error
 **/
gboolean
g_output_stream_flush (GOutputStream    *stream,
		       GCancellable  *cancellable,
		       GError          **error)
{
  GOutputStreamClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), FALSE);
  g_return_val_if_fail (stream != NULL, FALSE);

  if (stream->priv->closed)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      return FALSE;
    }

  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  res = TRUE;
  if (class->flush)
    {
      if (cancellable)
	g_push_current_cancellable (cancellable);
      
      stream->priv->pending = TRUE;
      res = class->flush (stream, cancellable, error);
      stream->priv->pending = FALSE;
      
      if (cancellable)
	g_pop_current_cancellable (cancellable);
    }
  
  return res;
}

/**
 * g_output_stream_close:
 * @stream: A #GOutputStream.
 * @cancellable: optional cancellable object
 * @error: location to store the error occuring, or %NULL to ignore
 *
 * Closes the stream, releasing resources related to it.
 *
 * Once the stream is closed, all other operations will return %G_VFS_ERROR_CLOSED.
 * Closing a stream multiple times will not return an error.
 *
 * Closing a stream will automatically flush any outstanding buffers in the
 * stream.
 *
 * Streams will be automatically closed when the last reference
 * is dropped, but you might want to call make sure resources
 * are released as early as possible.
 *
 * Some streams might keep the backing store of the stream (e.g. a file descriptor)
 * open after the stream is closed. See the documentation for the individual
 * stream for details.
 *
 * On failure the first error that happened will be reported, but the close
 * operation will finish as much as possible. A stream that failed to
 * close will still return %G_VFS_ERROR_CLOSED all operations. Still, it
 * is important to check and report the error to the user, otherwise
 * there might be a loss of data as all data might not be written.
 * 
 * If @cancellable is not NULL, then the operation can be cancelled by
 * triggering the cancellable object from another thread. If the operation
 * was cancelled, the error G_VFS_ERROR_CANCELLED will be returned.
 * Cancelling a close will still leave the stream closed, but there some streams
 * can use a faster close that doesn't block to e.g. check errors. On
 * cancellation (as with any error) there is no guarantee that all written
 * data will reach the target. 
 *
 * Return value: %TRUE on success, %FALSE on failure
 **/
gboolean
g_output_stream_close (GOutputStream  *stream,
		       GCancellable   *cancellable,
		       GError        **error)
{
  GOutputStreamClass *class;
  gboolean res;

  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), -1);
  g_return_val_if_fail (stream != NULL, -1);

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  if (stream->priv->closed)
    return TRUE;

  if (stream->priv->pending)
    {
      g_set_error (error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      return FALSE;
    }

  res = g_output_stream_flush (stream, cancellable, error);

  stream->priv->pending = TRUE;
  
  if (cancellable)
    g_push_current_cancellable (cancellable);

  if (!res)
    {
      /* flushing caused the error that we want to return,
       * but we still want to close the underlying stream if possible
       */
      if (class->close)
	class->close (stream, cancellable, NULL);
    }
  else
    {
      res = TRUE;
      if (class->close)
	res = class->close (stream, cancellable, error);
    }
  
  if (cancellable)
    g_pop_current_cancellable (cancellable);
  
  stream->priv->closed = TRUE;
  stream->priv->pending = FALSE;
  
  return res;
}

typedef struct {
  GAsyncResult       generic;
  void               *buffer;
  gsize               bytes_requested;
  gssize              bytes_written;
  GAsyncWriteCallback callback;
} WriteAsyncResult;

static gboolean
call_write_async_result (gpointer data)
{
  WriteAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->buffer,
		   res->bytes_requested,
		   res->bytes_written,
		   res->generic.data,
		   res->generic.error);

  return FALSE;
}

static void
queue_write_async_result (GOutputStream      *stream,
			  void               *buffer,
			  gsize               bytes_requested,
			  gssize              bytes_written,
			  GError             *error,
			  GAsyncWriteCallback callback,
			  gpointer            data)
{
  WriteAsyncResult *res;

  res = g_new0 (WriteAsyncResult, 1);

  res->buffer = buffer;
  res->bytes_requested = bytes_requested;
  res->bytes_written = bytes_written;
  res->callback = callback;

  _g_queue_async_result ((GAsyncResult *)res, stream,
			 error, data,
			 call_write_async_result);
}

static void
write_async_callback_wrapper (GOutputStream *stream,
			      void          *buffer,
			      gsize          count_requested,
			      gssize         count_written,
			      gpointer       data,
			      GError        *error)
{
  GAsyncWriteCallback real_callback = stream->priv->outstanding_callback;

  stream->priv->pending = FALSE;
  (*real_callback) (stream, buffer, count_requested, count_written, data, error);
  g_object_unref (stream);
}


/**
 * g_output_stream_write_async:
 * @stream: A #GOutputStream.
 * @buffer: the buffer containing the data to write. 
 * @count: the number of bytes to write
 * @io_priority: the io priority of the request
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @cancellable: optional cancellable object
 *
 * Request an asynchronous write of @count bytes from @buffer into the stream.
 * When the operation is finished @callback will be called, giving the results.
 *
 * During an async request no other sync and async calls are allowed, and will
 * result in %G_VFS_ERROR_PENDING errors. 
 *
 * A value of @count larger than %G_MAXSSIZE will cause a %G_FILE_ERROR_INVAL error.
 *
 * On success, the number of bytes written will be passed to the
 * callback. It is not an error if this is not the same as the requested size, as it
 * can happen e.g. on a partial i/o error, but generally we try to write
 * as many bytes as requested. 
 *
 * Any outstanding i/o request with higher priority (lower numerical value) will
 * be executed before an outstanding request with lower priority. Default
 * priority is G_%PRIORITY_DEFAULT.
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_output_stream_write_async (GOutputStream        *stream,
			     void                *buffer,
			     gsize                count,
			     int                  io_priority,
			     GAsyncWriteCallback  callback,
			     gpointer             data,
			     GCancellable        *cancellable)
{
  GOutputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  g_return_if_fail (buffer != NULL);

  if (count == 0)
    {
      queue_write_async_result (stream, buffer, count, 0, NULL,
				callback, data);
      return;
    }

  if (((gssize) count) < 0)
    {
      error = NULL;
      g_set_error (&error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
		   _("Too large count value passed to g_input_stream_read_async"));
      queue_write_async_result (stream, buffer, count, -1, error,
				callback, data);
      return;
    }

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_write_async_result (stream, buffer, count, -1, error,
				callback, data);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_write_async_result (stream, buffer, count, -1, error,
				callback, data);
      return;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->write_async (stream, buffer, count, io_priority, write_async_callback_wrapper, data, cancellable);
}

typedef struct {
  GAsyncResult        generic;
  gboolean            result;
  GAsyncFlushCallback callback;
} FlushAsyncResult;

static gboolean
call_flush_async_result (gpointer data)
{
  FlushAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->result,
		   res->generic.data,
		   res->generic.error);

  return FALSE;
}

static void
queue_flush_async_result (GOutputStream      *stream,
			  gboolean            result,
			  GError             *error,
			  GAsyncFlushCallback callback,
			  gpointer            data)
{
  FlushAsyncResult *res;

  res = g_new0 (FlushAsyncResult, 1);

  res->result = result;
  res->callback = callback;
  
  _g_queue_async_result ((GAsyncResult *)res, stream,
			 error, data,
			 call_flush_async_result);
}

static void
flush_async_callback_wrapper (GOutputStream *stream,
			      gboolean       result,
			      gpointer       data,
			      GError        *error)
{
  GAsyncFlushCallback real_callback = stream->priv->outstanding_callback;

  stream->priv->pending = FALSE;
  (*real_callback) (stream, result, data, error);
  g_object_unref (stream);
}

void
g_output_stream_flush_async (GOutputStream       *stream,
			     int                  io_priority,
			     GAsyncFlushCallback  callback,
			     gpointer             data,
			     GCancellable        *cancellable)
{
  GOutputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);

  if (stream->priv->closed)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_CLOSED,
		   _("Stream is already closed"));
      queue_flush_async_result (stream, FALSE, error,
				callback, data);
      return;
    }
  
  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_flush_async_result (stream, FALSE, error,
				callback, data);
      return;
    }

  class = G_OUTPUT_STREAM_GET_CLASS (stream);

  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->flush_async (stream, io_priority, flush_async_callback_wrapper, data, cancellable);
}

typedef struct {
  GAsyncResult       generic;
  gboolean            result;
  GAsyncCloseOutputCallback callback;
} CloseAsyncResult;

static gboolean
call_close_async_result (gpointer data)
{
  CloseAsyncResult *res = data;

  if (res->callback)
    res->callback (res->generic.async_object,
		   res->result,
		   res->generic.data,
		   res->generic.error);

  return FALSE;
}

static void
queue_close_async_result (GOutputStream       *stream,
			  gboolean            result,
			  GError             *error,
			  GAsyncCloseOutputCallback callback,
			  gpointer            data)
{
  CloseAsyncResult *res;

  res = g_new0 (CloseAsyncResult, 1);

  res->result = result;
  res->callback = callback;

  _g_queue_async_result ((GAsyncResult *)res, stream,
			 error, data,
			 call_close_async_result);
}

static void
close_async_callback_wrapper (GOutputStream *stream,
			      gboolean      result,
			      gpointer      data,
			      GError       *error)
{
  GAsyncCloseOutputCallback real_callback = stream->priv->outstanding_callback;

  stream->priv->pending = FALSE;
  stream->priv->closed = TRUE;
  (*real_callback) (stream, result, data, error);
  g_object_unref (stream);
}

/**
 * g_output_stream_close_async:
 * @stream: A #GOutputStream.
 * @callback: callback to call when the request is satisfied
 * @data: the data to pass to callback function
 * @cancellable: optional cancellable object
 *
 * Requests an asynchronous closes of the stream, releasing resources related to it.
 * When the operation is finished @callback will be called, giving the results.
 *
 * For behaviour details see g_output_stream_close().
 *
 * The asyncronous methods have a default fallback that uses threads to implement
 * asynchronicity, so they are optional for inheriting classes. However, if you
 * override one you must override all.
 **/
void
g_output_stream_close_async (GOutputStream       *stream,
			     int                  io_priority,
			     GAsyncCloseOutputCallback callback,
			     gpointer            data,
			     GCancellable       *cancellable)
{
  GOutputStreamClass *class;
  GError *error;

  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  if (stream->priv->closed)
    {
      queue_close_async_result (stream, TRUE, NULL,
				callback, data);
      return;
    }

  if (stream->priv->pending)
    {
      error = NULL;
      g_set_error (&error, G_VFS_ERROR, G_VFS_ERROR_PENDING,
		   _("Stream has outstanding operation"));
      queue_close_async_result (stream, FALSE, error,
				callback, data);
      return;
    }
  
  class = G_OUTPUT_STREAM_GET_CLASS (stream);
  stream->priv->pending = TRUE;
  stream->priv->outstanding_callback = callback;
  g_object_ref (stream);
  class->close_async (stream, io_priority, close_async_callback_wrapper, data, cancellable);
}

gboolean
g_output_stream_is_closed (GOutputStream *stream)
{
  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->closed;
}
  
gboolean
g_output_stream_has_pending (GOutputStream *stream)
{
  g_return_val_if_fail (G_IS_OUTPUT_STREAM (stream), TRUE);
  g_return_val_if_fail (stream != NULL, TRUE);
  
  return stream->priv->pending;
}

void
g_output_stream_set_pending (GOutputStream              *stream,
			    gboolean                   pending)
{
  g_return_if_fail (G_IS_OUTPUT_STREAM (stream));
  g_return_if_fail (stream != NULL);
  
  stream->priv->pending = pending;
}


/********************************************
 *   Default implementation of async ops    *
 ********************************************/

typedef struct {
  GOutputStream     *stream;
  GError            *error;
  gpointer           data;
} OutputStreamOp;

static void
output_stream_op_free (gpointer data)
{
  OutputStreamOp *op = data;

  if (op->error)
    g_error_free (op->error);

  g_free (op);
}

typedef struct {
  OutputStreamOp      op;
  void               *buffer;
  gsize               count_requested;
  gssize              count_written;
  GAsyncWriteCallback callback;
} WriteAsyncOp;

static void
write_op_report (gpointer data)
{
  WriteAsyncOp *op = data;

  op->callback (op->op.stream,
		op->buffer,
		op->count_requested,
		op->count_written,
		op->op.data,
		op->op.error);

}


static void
write_op_func (GIOJob *job,
	       GCancellable *c,
	       gpointer data)
{
  WriteAsyncOp *op = data;
  GOutputStreamClass *class;

  if (g_cancellable_is_cancelled (c))
    {
      op->count_written = -1;
      g_set_error (&op->op.error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_OUTPUT_STREAM_GET_CLASS (op->op.stream);
      op->count_written = class->write (op->op.stream, op->buffer, op->count_requested,
					c, &op->op.error);
    }

  g_io_job_send_to_mainloop (job, write_op_report,
			     op, output_stream_op_free,
			     FALSE);
}

static void
g_output_stream_real_write_async (GOutputStream       *stream,
				  void                *buffer,
				  gsize                count,
				  int                  io_priority,
				  GAsyncWriteCallback  callback,
				  gpointer             data,
				  GCancellable        *cancellable)
{
  WriteAsyncOp *op;

  op = g_new0 (WriteAsyncOp, 1);

  op->op.stream = stream;
  op->buffer = buffer;
  op->count_requested = count;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (write_op_func,
		     op,
		     NULL,
		     io_priority,
		     cancellable);
}

typedef struct {
  OutputStreamOp      op;
  gboolean            result;
  GAsyncFlushCallback callback;
} FlushAsyncOp;

static void
flush_op_report (gpointer data)
{
  FlushAsyncOp *op = data;

  op->callback (op->op.stream,
		op->result,
		op->op.data,
		op->op.error);

}

static void
flush_op_func (GIOJob *job,
	       GCancellable *c,
	       gpointer data)
{
  FlushAsyncOp *op = data;
  GOutputStreamClass *class;

  if (g_cancellable_is_cancelled (c))
    {
      op->result = FALSE;
      g_set_error (&op->op.error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_OUTPUT_STREAM_GET_CLASS (op->op.stream);
      op->result = TRUE;
      if (class->flush)
	op->result = class->flush (op->op.stream, c, &op->op.error);
    }

  g_io_job_send_to_mainloop (job, flush_op_report,
			     op, output_stream_op_free,
			     FALSE);
}

static void
g_output_stream_real_flush_async (GOutputStream       *stream,
				  int                  io_priority,
				  GAsyncFlushCallback  callback,
				  gpointer             data,
				  GCancellable        *cancellable)
{
  FlushAsyncOp *op;

  op = g_new0 (FlushAsyncOp, 1);

  op->op.stream = stream;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (flush_op_func,
		     op,
		     NULL,
		     io_priority,
		     cancellable);
}

typedef struct {
  OutputStreamOp     op;
  gboolean           res;
  GAsyncCloseOutputCallback callback;
} CloseAsyncOp;

static void
close_op_report (gpointer data)
{
  CloseAsyncOp *op = data;

  op->callback (op->op.stream,
		op->res,
		op->op.data,
		op->op.error);
}

static void
close_op_func (GIOJob *job,
	       GCancellable *c,
	       gpointer data)
{
  CloseAsyncOp *op = data;
  GOutputStreamClass *class;

  if (g_cancellable_is_cancelled (c))
    {
      op->res = FALSE;
      g_set_error (&op->op.error,
		   G_VFS_ERROR,
		   G_VFS_ERROR_CANCELLED,
		   _("Operation was cancelled"));
    }
  else
    {
      class = G_OUTPUT_STREAM_GET_CLASS (op->op.stream);
      op->res = class->close (op->op.stream, c, &op->op.error);
    }

 g_io_job_send_to_mainloop (job, close_op_report,
			     op, output_stream_op_free,
			     FALSE);
}

static void
g_output_stream_real_close_async (GOutputStream       *stream,
				  int                  io_priority,
				  GAsyncCloseOutputCallback callback,
				  gpointer            data,
				  GCancellable       *cancellable)
{
  CloseAsyncOp *op;

  op = g_new0 (CloseAsyncOp, 1);

  op->op.stream = stream;
  op->callback = callback;
  op->op.data = data;
  
  g_schedule_io_job (close_op_func,
		     op,
		     NULL,
		     io_priority,
		     cancellable);
}