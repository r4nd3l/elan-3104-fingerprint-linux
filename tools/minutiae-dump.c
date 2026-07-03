/*
 * Diagnostic tool: capture one image, run minutiae detection, dump the
 * binarized (ridge/valley) image and minutiae count/positions so we can
 * see what the matcher actually extracts, instead of guessing from the
 * enroll/verify score alone.
 */

#define FP_COMPONENT "minutiae-dump"

#include <stdio.h>
#include <errno.h>
#include <libfprint/fprint.h>
#include <glib-unix.h>

#include "storage.h"
#include "utilities.h"

typedef struct CaptureData
{
  GMainLoop    *loop;
  GCancellable *cancellable;
  unsigned int  sigint_handler;
  int           ret_value;
  const char   *filename;
  FpDevice     *dev;
} CaptureData;

static void
capture_data_free (CaptureData *capture_data)
{
  g_clear_handle_id (&capture_data->sigint_handler, g_source_remove);
  g_clear_object (&capture_data->cancellable);
  g_main_loop_unref (capture_data->loop);
  g_free (capture_data);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (CaptureData, capture_data_free)

static void
on_device_closed (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  CaptureData *capture_data = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    g_warning ("Failed closing device %s", error->message);

  g_main_loop_quit (capture_data->loop);
}

static void
capture_quit (FpDevice *dev, CaptureData *capture_data)
{
  if (!fp_device_is_open (dev))
    {
      g_main_loop_quit (capture_data->loop);
      return;
    }
  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_device_closed, capture_data);
}

static gboolean
save_buf_to_pgm (const guchar *data, gsize len, int width, int height, const char *path)
{
  FILE *fd = fopen (path, "w");
  guchar max = 0;

  if (!fd)
    {
      g_warning ("could not open '%s' for writing: %d", path, errno);
      return FALSE;
    }

  for (gsize i = 0; i < len; i++)
    if (data[i] > max)
      max = data[i];

  fprintf (fd, "P5 %d %d 255\n", width, height);

  if (max > 0 && max < 255)
    {
      /* scale up so low-range encodings (e.g. 0/1) are actually visible */
      for (gsize i = 0; i < len; i++)
        {
          guchar v = (guchar) ((data[i] * 255) / max);
          fputc (v, fd);
        }
    }
  else
    {
      fwrite (data, 1, len, fd);
    }

  fclose (fd);
  g_print ("written '%s' (%dx%d, max raw value seen: %u)\n", path, width, height, max);
  return TRUE;
}

static void
on_minutiae_detected (GObject *source, GAsyncResult *res, void *user_data)
{
  FpImage *image = FP_IMAGE (source);
  CaptureData *capture_data = user_data;
  g_autoptr(GError) error = NULL;
  GPtrArray *minutiae;
  gsize bin_len = 0;
  const guchar *binarized;
  g_autofree char *bin_path = NULL;

  if (!fp_image_detect_minutiae_finish (image, res, &error))
    {
      g_warning ("minutiae detection failed: %s", error->message);
      g_object_unref (image);
      capture_quit (capture_data->dev, capture_data);
      return;
    }

  minutiae = fp_image_get_minutiae (image);
  g_print ("minutiae found: %u\n", minutiae ? minutiae->len : 0);

  if (minutiae)
    {
      for (guint i = 0; i < minutiae->len && i < 40; i++)
        {
          FpMinutia *m = g_ptr_array_index (minutiae, i);
          gint x, y;
          fp_minutia_get_coords (m, &x, &y);
          g_print ("  #%02u  x=%4d y=%4d\n", i, x, y);
        }
      if (minutiae->len > 40)
        g_print ("  ... (%u more)\n", minutiae->len - 40);
    }

  binarized = fp_image_get_binarized (image, &bin_len);
  if (binarized)
    {
      bin_path = g_strdup_printf ("%s.binarized.pgm", capture_data->filename);
      save_buf_to_pgm (binarized, bin_len,
                        fp_image_get_width (image), fp_image_get_height (image),
                        bin_path);
    }
  else
    {
      g_warning ("no binarized data available");
    }

  g_object_unref (image);
  capture_quit (capture_data->dev, capture_data);
}

static void
dev_capture_cb (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  g_autoptr(GError) error = NULL;
  CaptureData *capture_data = user_data;
  FpImage *image = NULL;

  g_clear_object (&capture_data->cancellable);

  image = fp_device_capture_finish (dev, res, &error);
  if (!image)
    {
      g_warning ("Error capturing data: %s", error->message);
      capture_quit (dev, capture_data);
      return;
    }

  save_image_to_pgm (image, capture_data->filename);

  /* image ownership: detect_minutiae works on the FpImage directly, and we
   * unref it ourselves in the callback once done. */
  fp_image_detect_minutiae (image, NULL, on_minutiae_detected, capture_data);
}

static void
start_capture (FpDevice *dev, CaptureData *capture_data)
{
  fp_device_capture (dev, TRUE, capture_data->cancellable, (GAsyncReadyCallback) dev_capture_cb, capture_data);
}

static void
on_device_opened (FpDevice *dev, GAsyncResult *res, void *user_data)
{
  CaptureData *capture_data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_warning ("Failed to open device: %s", error->message);
      capture_quit (dev, capture_data);
      return;
    }

  g_print ("Opened device. ");
  capture_data->dev = dev;
  start_capture (dev, capture_data);
}

static gboolean
sigint_cb (void *user_data)
{
  CaptureData *capture_data = user_data;
  g_cancellable_cancel (capture_data->cancellable);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, const char *argv[])
{
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(CaptureData) capture_data = NULL;
  GPtrArray *devices;
  FpDevice *dev;

  setenv ("G_MESSAGES_DEBUG", "all", 0);

  ctx = fp_context_new ();

  devices = fp_context_get_devices (ctx);
  if (!devices)
    {
      g_warning ("Impossible to get devices");
      return EXIT_FAILURE;
    }

  dev = discover_device (devices);
  if (!dev)
    {
      g_warning ("No devices detected.");
      return EXIT_FAILURE;
    }

  if (!fp_device_has_feature (dev, FP_DEVICE_FEATURE_CAPTURE))
    {
      g_warning ("Device %s doesn't support capture", fp_device_get_name (dev));
      return EXIT_FAILURE;
    }

  capture_data = g_new0 (CaptureData, 1);
  capture_data->ret_value = EXIT_FAILURE;
  capture_data->loop = g_main_loop_new (NULL, FALSE);
  capture_data->cancellable = g_cancellable_new ();
  capture_data->sigint_handler = g_unix_signal_add_full (G_PRIORITY_HIGH,
                                                         SIGINT,
                                                         sigint_cb,
                                                         capture_data,
                                                         NULL);
  capture_data->filename = (argc == 2) ? argv[1] : "finger.pgm";

  fp_device_open (dev, capture_data->cancellable,
                  (GAsyncReadyCallback) on_device_opened,
                  capture_data);

  g_main_loop_run (capture_data->loop);

  return capture_data->ret_value;
}
