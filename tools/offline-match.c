/*
 * Offline matcher experiment tool: loads saved PGM captures, runs the same
 * minutiae detection + bozorth3 matching pipeline fprintd uses, entirely
 * offline. Lets us iterate on image-processing ideas against real saved
 * captures without needing a live swipe per test.
 *
 * Usage: offline-match [-f flags] a.pgm b.pgm [c.pgm ...]
 *   flags: bitmask; 1=PARTIAL 2=COLORS_INVERTED 4=H_FLIPPED 8=V_FLIPPED
 *          (default 3 = PARTIAL|COLORS_INVERTED, matching the elanspi driver)
 *
 * Prints minutiae count per image and the pairwise bozorth3 score of every
 * image against the first one.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "fpi-image.h"
#include <nbis.h>

static FpImage *
load_pgm (const char *path, guint flags)
{
  FILE *fd = fopen (path, "rb");
  char magic[3] = { 0 };
  int w, h, maxv;
  FpImage *img;
  gsize len;

  if (!fd)
    {
      fprintf (stderr, "cannot open %s: %s\n", path, strerror (errno));
      exit (1);
    }
  if (fscanf (fd, "%2s %d %d %d", magic, &w, &h, &maxv) != 4 || strcmp (magic, "P5") != 0)
    {
      fprintf (stderr, "%s: not a binary PGM\n", path);
      exit (1);
    }
  fgetc (fd); /* single whitespace after header */

  img = fp_image_new (w, h);
  img->flags = flags;
  len = (gsize) w * h;
  if (fread ((void *) img->data, 1, len, fd) != len)
    {
      fprintf (stderr, "%s: short read\n", path);
      exit (1);
    }
  fclose (fd);
  return img;
}

static void
detect_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(GError) error = NULL;
  GMainLoop *loop = user_data;

  if (!fp_image_detect_minutiae_finish (FP_IMAGE (source), res, &error))
    fprintf (stderr, "minutiae detection failed: %s\n", error->message);
  g_main_loop_quit (loop);
}

/* mirror of fpi-print.c's minutiae_to_xyt */
static void
image_to_xyt (FpImage *img, struct xyt_struct *xyt)
{
  GPtrArray *minutiae = fp_image_get_minutiae (img);
  struct minutiae_struct c[MAX_FILE_MINUTIAE];
  int nmin, i;

  if (!minutiae)
    {
      xyt->nrows = 0;
      return;
    }

  nmin = MIN ((int) minutiae->len, MAX_BOZORTH_MINUTIAE);

  for (i = 0; i < nmin; i++)
    {
      struct fp_minutia *minutia = g_ptr_array_index (minutiae, i);

      lfs2nist_minutia_XYT (&c[i].col[0], &c[i].col[1], &c[i].col[2],
                            minutia, img->width, img->height);
      c[i].col[3] = sround (minutia->reliability * 100.0);
      if (c[i].col[2] > 180)
        c[i].col[2] -= 360;
    }

  qsort ((void *) &c, (size_t) nmin, sizeof (struct minutiae_struct), sort_x_y);

  for (i = 0; i < nmin; i++)
    {
      xyt->xcol[i] = c[i].col[0];
      xyt->ycol[i] = c[i].col[1];
      xyt->thetacol[i] = c[i].col[2];
    }
  xyt->nrows = nmin;
}

static FpImage *
detect (const char *path, guint flags)
{
  FpImage *img = load_pgm (path, flags);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  GPtrArray *min;

  fp_image_detect_minutiae (img, NULL, detect_cb, loop);
  g_main_loop_run (loop);
  g_main_loop_unref (loop);

  min = fp_image_get_minutiae (img);
  printf ("%-28s %3ux%-3u flags=0x%x minutiae=%u\n", path,
          fp_image_get_width (img), fp_image_get_height (img),
          flags, min ? min->len : 0);
  return img;
}

int
main (int argc, char **argv)
{
  guint flags = FPI_IMAGE_PARTIAL | FPI_IMAGE_COLORS_INVERTED;
  int argi = 1;

  if (argc > 2 && strcmp (argv[1], "-f") == 0)
    {
      flags = (guint) atoi (argv[2]);
      argi = 3;
    }
  if (argc - argi < 2)
    {
      fprintf (stderr, "usage: %s [-f flags] a.pgm b.pgm...\n", argv[0]);
      return 1;
    }

  g_autoptr(FpImage) timg = detect (argv[argi], flags);
  g_autofree struct xyt_struct *txyt = g_new0 (struct xyt_struct, 1);
  image_to_xyt (timg, txyt);

  for (int i = argi + 1; i < argc; i++)
    {
      g_autoptr(FpImage) simg = detect (argv[i], flags);
      g_autofree struct xyt_struct *sxyt = g_new0 (struct xyt_struct, 1);
      gint probe_len, score;

      image_to_xyt (simg, sxyt);
      probe_len = bozorth_probe_init (sxyt);
      score = bozorth_to_gallery (probe_len, sxyt, txyt);
      printf ("bz3 score  %s vs %s = %d  (elanspi threshold: 24)\n",
              argv[argi], argv[i], score);
    }
  return 0;
}
