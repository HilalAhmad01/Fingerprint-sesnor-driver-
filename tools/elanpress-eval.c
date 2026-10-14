/*
 * elanpress-eval: genuine/impostor score distributions for the elanpress
 * matcher, from touches recorded with ELANPRESS_DUMP_DIR
 *
 * Layout: DIR/<finger-label>/<anything>.pgm, one directory per finger (a
 * different person's finger is just another label). For every label, the
 * first --gallery touches play the enrolled print, exactly as the driver
 * scores it (best over the enrolled touches), and every remaining touch of
 * every label is scored against it. Same label: genuine. Different: impostor.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elanpress-match.h"

#define MAX_SCORE 400

typedef struct
{
  char              *label;
  GPtrArray         *features;  /* ElanpressFeatures*, in file name order */
} Finger;

static int width = 0, height = 0;

/* binary PGM, 8 bit, all touches the same size */
static guint8 *
load_pgm (const char *path)
{
  g_autofree char *data = NULL;
  gsize len = 0;
  int w, h, maxval, off = 0;

  if (!g_file_get_contents (path, &data, &len, NULL))
    return NULL;
  if (sscanf (data, "P5 %d %d %d%n", &w, &h, &maxval, &off) != 3 ||
      maxval != 255 || w <= 0 || h <= 0)
    return NULL;
  off++; /* the single whitespace after maxval */
  if ((gsize) off + (gsize) w * h > len)
    return NULL;
  if (width == 0)
    {
      width = w;
      height = h;
    }
  if (w != width || h != height)
    {
      g_printerr ("%s: %dx%d, expected %dx%d\n", path, w, h, width, height);
      return NULL;
    }
  return g_memdup2 (data + off, (gsize) w * h);
}

static gint
cmp_str (gconstpointer a, gconstpointer b)
{
  return strcmp (*(char * const *) a, *(char * const *) b);
}

static Finger *
load_finger (const char *dir, const char *label)
{
  g_autofree char *path = g_build_filename (dir, label, NULL);
  g_autoptr(GDir) d = g_dir_open (path, 0, NULL);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func (g_free);
  const char *name;
  Finger *f;

  if (!d)
    return NULL;
  while ((name = g_dir_read_name (d)))
    if (g_str_has_suffix (name, ".pgm"))
      g_ptr_array_add (names, g_strdup (name));
  g_ptr_array_sort (names, cmp_str);

  f = g_new0 (Finger, 1);
  f->label = g_strdup (label);
  f->features = g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_features_free);
  for (guint i = 0; i < names->len; i++)
    {
      g_autofree char *file = g_build_filename (path, g_ptr_array_index (names, i), NULL);
      g_autofree guint8 *img = load_pgm (file);

      if (img)
        g_ptr_array_add (f->features, elanpress_image_features (img, width, height));
    }
  return f;
}

static guint
template_score (Finger *f, guint gallery, const ElanpressFeatures *probe)
{
  guint best = 0;

  for (guint i = 0; i < gallery && i < f->features->len; i++)
    best = MAX (best, elanpress_match_features (probe, g_ptr_array_index (f->features, i)));
  return best;
}

int
main (int argc, char *argv[])
{
  int gallery = 12;
  g_autofree char **rest = NULL;
  GOptionEntry entries[] = {
    { "gallery", 'g', 0, G_OPTION_ARG_INT, &gallery,
      "touches per finger used as the enrolled print (default 12)", "N" },
    { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, &rest, NULL, "DIR" },
    { NULL }
  };
  g_autoptr(GOptionContext) ctx = g_option_context_new ("- elanpress score distributions");
  g_autoptr(GError) error = NULL;
  g_autoptr(GPtrArray) fingers = g_ptr_array_new ();
  g_autoptr(GDir) d = NULL;
  guint64 genuine[MAX_SCORE + 1] = { 0 }, impostor[MAX_SCORE + 1] = { 0 };
  guint64 n_gen = 0, n_imp = 0;
  guint max_imp = 0;
  const char *name;

  g_option_context_add_main_entries (ctx, entries, NULL);
  if (!g_option_context_parse (ctx, &argc, &argv, &error) || !rest || !rest[0] ||
      gallery < 1)
    {
      g_printerr ("usage: elanpress-eval [--gallery N] DIR\n");
      return 2;
    }

  d = g_dir_open (rest[0], 0, &error);
  if (!d)
    {
      g_printerr ("%s\n", error->message);
      return 1;
    }
  while ((name = g_dir_read_name (d)))
    {
      Finger *f = load_finger (rest[0], name);

      if (!f)
        continue;
      if (f->features->len <= (guint) gallery)
        g_printerr ("%s: only %u touches, all used as gallery, none as probes\n",
                    f->label, f->features->len);
      g_ptr_array_add (fingers, f);
      printf ("%-24s %u touches\n", f->label, f->features->len);
    }
  if (fingers->len == 0)
    {
      g_printerr ("no DIR/<label>/*.pgm found\n");
      return 1;
    }

  for (guint t = 0; t < fingers->len; t++)
    {
      Finger *tf = g_ptr_array_index (fingers, t);

      for (guint p = 0; p < fingers->len; p++)
        {
          Finger *pf = g_ptr_array_index (fingers, p);
          /* a finger's own gallery touches are never its probes */
          guint start = (p == t) ? (guint) gallery : 0;

          for (guint i = start; i < pf->features->len; i++)
            {
              guint s = MIN (template_score (tf, gallery,
                                             g_ptr_array_index (pf->features, i)),
                             MAX_SCORE);

              if (p == t)
                {
                  genuine[s]++;
                  n_gen++;
                }
              else
                {
                  impostor[s]++;
                  n_imp++;
                  max_imp = MAX (max_imp, s);
                }
            }
        }
    }

  printf ("\n%" G_GUINT64_FORMAT " genuine and %" G_GUINT64_FORMAT
          " impostor comparisons; highest impostor score %u\n\n",
          n_gen, n_imp, max_imp);
  printf ("threshold   FRR (genuine rejected)   FAR (impostors accepted)\n");
  for (guint thr = 4; thr <= 40; thr += (thr < 20 ? 1 : 4))
    {
      guint64 rej = 0, acc = 0;

      for (guint s = 0; s < thr; s++)
        rej += genuine[s];
      for (guint s = thr; s <= MAX_SCORE; s++)
        acc += impostor[s];
      printf ("%6u%s     %6.2f%% (%" G_GUINT64_FORMAT ")            %8.4f%% (%" G_GUINT64_FORMAT ")\n",
              thr, thr == ELANPRESS_MATCH_MIN_PAIRS ? " *" : "  ",
              n_gen ? 100.0 * rej / n_gen : 0, rej,
              n_imp ? 100.0 * acc / n_imp : 0, acc);
    }
  printf ("\n* current ELANPRESS_MATCH_MIN_PAIRS\n");
  if (n_imp)
    printf ("With 0 impostors accepted, the false accept rate is below %.2g "
            "with 95%% confidence (3/N); a lock screen wants 1e-4 or better,\n"
            "i.e. at least ~30000 impostor comparisons with none accepted.\n",
            3.0 / n_imp);

  return 0;
}
