/*
 * Standalone hardware test harness for the goodix533c driver.
 *
 * Not part of libfprint's public API or installed targets -- it reaches
 * straight into the driver's test-only entry point
 * (fpi_device_goodix533c_capture_test) because there is no enroll/verify
 * vfunc wired up yet (out of scope for this task). Builds only when
 * 'goodix533c' is in the enabled driver list (see libfprint/meson.build).
 *
 * Usage: goodix533c-capture-test [reference-output.pgm]
 *
 * Drives the full sequence: reset -> TLS -> config upload -> FDT baseline
 * -> no-finger reference frame -> arm finger detection -> wait for a
 * touch (up to GOODIX533C_FINGER_WAIT_TIMEOUT_MS) -> live frame -> flat
 * field. The reference frame's PGM is always written if captured, whether
 * or not a finger was ever touched to the sensor; the flat-fielded
 * "-live" PGM is only written if a touch was actually detected in time.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <glib.h>

#include "fp-context.h"
#include "fp-device.h"

#include "drivers/goodix533c/goodix533c.h"

typedef struct
{
  GMainLoop *loop;
  const char *output_path;
  int         exit_code;
} TestState;

static gboolean
write_pgm (const char *path, const guint8 *pixels, int width, int height)
{
  FILE *f = fopen (path, "wb");
  size_t n;

  if (!f)
    {
      g_print ("Failed to open %s for writing: %s\n", path, g_strerror (errno));
      return FALSE;
    }

  fprintf (f, "P5\n%d %d\n255\n", width, height);
  n = fwrite (pixels, 1, (size_t) (width * height), f);
  fclose (f);

  return n == (size_t) (width * height);
}

/* Derives "<stem>-live.pgm" from the reference-frame output path (e.g.
 * "capture.pgm" -> "capture-live.pgm"), so a single positional argument
 * on the command line still names both output files predictably. */
static gchar *
live_output_path (const char *reference_path)
{
  const char *dot = strrchr (reference_path, '.');

  if (dot)
    return g_strdup_printf ("%.*s-live%s", (int) (dot - reference_path),
                           reference_path, dot);

  return g_strdup_printf ("%s-live", reference_path);
}

static void
print_pixel_range (const char *label, const guint16 *pixels, int count)
{
  guint16 min = 0xffff;
  guint16 max = 0;
  int i;

  for (i = 0; i < count; i++)
    {
      if (pixels[i] < min)
        min = pixels[i];
      if (pixels[i] > max)
        max = pixels[i];
    }

  g_print ("%s: %dx%d, raw pixel range [%u, %u]\n", label,
           GOODIX533C_SENSOR_WIDTH, GOODIX533C_SENSOR_HEIGHT, min, max);
}

static void
on_closed (FpDevice *dev, GAsyncResult *res, TestState *ts)
{
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (dev, res, &error);
  if (error)
    g_print ("close() error: %s\n", error->message);
  else
    g_print ("close() OK\n");

  g_main_loop_quit (ts->loop);
}

static void
on_wait_for_finger (FpDevice *dev, gpointer user_data)
{
  g_print ("Touch the sensor now (%ds)...\n",
           GOODIX533C_FINGER_WAIT_TIMEOUT_MS / 1000);
}

static void
on_capture_done (FpDevice *dev, const guint16 *raw_pixels,
                 const guint8 *squashed, const guint16 *live_raw_pixels,
                 const guint8 *corrected, gpointer user_data, GError *error)
{
  TestState *ts = user_data;
  int count = GOODIX533C_SENSOR_WIDTH * GOODIX533C_SENSOR_HEIGHT;

  /* Write whatever frames actually came back before looking at @error --
   * a failure partway through (e.g. no finger touched within the
   * timeout) must not throw away a reference frame that was already
   * captured successfully earlier in the same sequence. */
  if (raw_pixels && squashed)
    {
      print_pixel_range ("Reference frame", raw_pixels, count);

      if (write_pgm (ts->output_path, squashed, GOODIX533C_SENSOR_WIDTH,
                     GOODIX533C_SENSOR_HEIGHT))
        g_print ("Wrote %s\n", ts->output_path);
      else
        {
          g_print ("Failed to write %s\n", ts->output_path);
          ts->exit_code = 1;
        }
    }
  else
    {
      g_print ("No reference frame captured.\n");
    }

  if (live_raw_pixels && corrected)
    {
      g_autofree gchar *live_path = live_output_path (ts->output_path);

      print_pixel_range ("Live frame", live_raw_pixels, count);

      if (write_pgm (live_path, corrected, GOODIX533C_SENSOR_WIDTH,
                     GOODIX533C_SENSOR_HEIGHT))
        g_print ("Wrote %s (flat-fielded fingerprint)\n", live_path);
      else
        {
          g_print ("Failed to write %s\n", live_path);
          ts->exit_code = 1;
        }
    }

  if (error)
    {
      g_print ("Capture sequence FAILED: %s\n", error->message);
      ts->exit_code = 1;
    }
  else
    {
      g_print ("Capture sequence completed successfully.\n");
    }

  fp_device_close (dev, NULL, (GAsyncReadyCallback) on_closed, ts);
}

static void
on_opened (FpDevice *dev, GAsyncResult *res, TestState *ts)
{
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (dev, res, &error))
    {
      g_print ("open() FAILED: %s\n", error ? error->message : "(no error set)");
      ts->exit_code = 1;
      g_main_loop_quit (ts->loop);
      return;
    }

  g_print ("open() SUCCEEDED\n");
  fpi_device_goodix533c_capture_test (dev, on_wait_for_finger,
                                      on_capture_done, ts);
}

int
main (int argc, char **argv)
{
  g_autoptr(FpContext) ctx = NULL;
  GPtrArray *devices;
  FpDevice *dev = NULL;
  TestState ts = { 0 };
  guint i;

  ts.output_path = argc > 1 ? argv[1] : "goodix533c-capture.pgm";

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);

  if (!devices || devices->len == 0)
    {
      g_print ("No fingerprint devices found at all.\n");
      return 1;
    }

  for (i = 0; i < devices->len; ++i)
    {
      FpDevice *d = g_ptr_array_index (devices, i);

      g_print ("Found: %s (%s) - driver %s\n",
               fp_device_get_device_id (d), fp_device_get_name (d),
               fp_device_get_driver (d));
      if (g_strcmp0 (fp_device_get_driver (d), "goodix533c") == 0)
        dev = d;
    }

  if (!dev)
    {
      g_print ("No goodix533c device found among the above.\n");
      return 1;
    }

  ts.loop = g_main_loop_new (NULL, FALSE);
  g_print ("Opening %s ...\n", fp_device_get_device_id (dev));
  fp_device_open (dev, NULL, (GAsyncReadyCallback) on_opened, &ts);
  g_main_loop_run (ts.loop);
  g_main_loop_unref (ts.loop);

  return ts.exit_code;
}
