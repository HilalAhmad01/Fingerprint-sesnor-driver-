/*
 * Unit tests for the elanpress keypoint matcher and print format
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
#include <math.h>
#include <string.h>

#include "elanpress-match.h"

#define W 80
#define H 64

static guint8 *
make_canvas (int cw, int ch, guint32 seed)
{
  GRand *rand = g_rand_new_with_seed (seed);
  g_autofree gdouble *acc = g_new0 (gdouble, cw * ch);
  guint8 *img = g_malloc (cw * ch);
  int n_blobs = cw * ch / 25;

  /* random dark and light blobs of a few sizes: distinctive local structure
   * everywhere, which is all the matcher needs to be exercised */
  for (int i = 0; i < n_blobs; i++)
    {
      gdouble bx = g_rand_double_range (rand, -4, cw + 4);
      gdouble by = g_rand_double_range (rand, -4, ch + 4);
      gdouble s = g_rand_double_range (rand, 1.2, 3.0);
      gdouble a = g_rand_double_range (rand, -1, 1);

      for (int y = MAX (0, (int) (by - 3 * s)); y < MIN (ch, (int) (by + 3 * s) + 1); y++)
        for (int x = MAX (0, (int) (bx - 3 * s)); x < MIN (cw, (int) (bx + 3 * s) + 1); x++)
          acc[y * cw + x] += a * exp (-((x - bx) * (x - bx) + (y - by) * (y - by)) / (2 * s * s));
    }

  for (int i = 0; i < cw * ch; i++)
    img[i] = (guint8) CLAMP (128 + 70 * acc[i], 0, 255);

  g_rand_free (rand);
  return img;
}

static guint8 *
crop (const guint8 *canvas, int cw, int x0, int y0)
{
  guint8 *img = g_malloc (W * H);

  for (int y = 0; y < H; y++)
    memcpy (img + y * W, canvas + (y + y0) * cw + x0, W);
  return img;
}

static void
test_genuine_overlap_matches (void)
{
  g_autofree guint8 *canvas = make_canvas (W + 20, H + 20, 1);
  g_autofree guint8 *a = crop (canvas, W + 20, 0, 0);
  g_autofree guint8 *b = crop (canvas, W + 20, 12, 9);
  g_autoptr(ElanpressFeatures) fa = elanpress_image_features (a, W, H);
  g_autoptr(ElanpressFeatures) fb = elanpress_image_features (b, W, H);
  guint pairs = elanpress_match_features (fa, fb);

  g_test_message ("overlapping windows: %u/%u keypoints, %u pairs", fa->n, fb->n, pairs);
  g_assert_cmpuint (pairs, >=, ELANPRESS_MATCH_MIN_PAIRS);
}

static void
test_impostor_rejected (void)
{
  for (guint32 seed = 2; seed < 12; seed++)
    {
      g_autofree guint8 *ca = make_canvas (W, H, 1);
      g_autofree guint8 *cb = make_canvas (W, H, seed);
      g_autoptr(ElanpressFeatures) fa = elanpress_image_features (ca, W, H);
      g_autoptr(ElanpressFeatures) fb = elanpress_image_features (cb, W, H);
      guint pairs = elanpress_match_features (fa, fb);

      g_test_message ("seed %u: %u pairs", seed, pairs);
      g_assert_cmpuint (pairs, <, ELANPRESS_MATCH_MIN_PAIRS);
    }
}

/* n keypoints with unique random descriptors, laid out on a grid of the
 * given pitch; the enrolled set is the same keypoints shifted by (dx, dy) */
static void
make_keypoint_sets (guint n, gfloat pitch, gfloat dx, gfloat dy,
                    ElanpressFeatures **probe, ElanpressFeatures **enrolled)
{
  GRand *rand = g_rand_new_with_seed (42);

  *probe = g_new0 (ElanpressFeatures, 1);
  *enrolled = g_new0 (ElanpressFeatures, 1);
  (*probe)->kp = g_new0 (ElanpressKeypoint, n);
  (*enrolled)->kp = g_new0 (ElanpressKeypoint, n);
  (*probe)->n = (*enrolled)->n = n;

  for (guint i = 0; i < n; i++)
    {
      ElanpressKeypoint *p = &(*probe)->kp[i], *e = &(*enrolled)->kp[i];

      p->x = 10 + (i % 4) * pitch;
      p->y = 10 + (i / 4) * pitch;
      p->angle = 1.0f;
      for (int k = 0; k < ELANPRESS_SIFT_DESC_LEN; k++)
        p->desc[k] = g_rand_int_range (rand, 0, 256);
      *e = *p;
      e->x += dx;
      e->y += dy;
    }

  g_rand_free (rand);
}

static void
test_spread_guard (void)
{
  ElanpressFeatures *probe, *enrolled;

  /* 16 consistent pairs spread over a 30 px square: a match */
  make_keypoint_sets (16, 10.0f, 5, 3, &probe, &enrolled);
  g_assert_cmpuint (elanpress_match_features (probe, enrolled), >=,
                    ELANPRESS_MATCH_MIN_PAIRS);
  elanpress_features_free (probe);
  elanpress_features_free (enrolled);

  /* the same 16 pairs packed into a 3 px square: an artefact */
  make_keypoint_sets (16, 1.0f, 5, 3, &probe, &enrolled);
  g_assert_cmpuint (elanpress_match_features (probe, enrolled), ==, 0);
  elanpress_features_free (probe);
  elanpress_features_free (enrolled);
}

static void
test_pack_roundtrip (void)
{
  g_autofree guint8 *img = make_canvas (W, H, 7);
  g_autoptr(ElanpressFeatures) f = elanpress_image_features (img, W, H);
  g_autoptr(ElanpressFeatures) g = NULL;
  g_autofree guint8 *packed = NULL;
  gsize len = 0;

  g_assert_cmpuint (f->n, >, 0);
  packed = elanpress_features_pack (f, &len);
  g_assert_cmpuint (len, ==, f->n * ELANPRESS_PACKED_KP_LEN);

  g = elanpress_features_unpack (packed, len, W, H);
  g_assert_nonnull (g);
  g_assert_cmpuint (g->n, ==, f->n);
  for (guint i = 0; i < f->n; i++)
    {
      g_assert_cmpfloat (g->kp[i].x, ==, f->kp[i].x);
      g_assert_cmpfloat (g->kp[i].y, ==, f->kp[i].y);
      g_assert_cmpfloat (g->kp[i].angle, ==, f->kp[i].angle);
      g_assert_cmpmem (g->kp[i].desc, ELANPRESS_SIFT_DESC_LEN,
                       f->kp[i].desc, ELANPRESS_SIFT_DESC_LEN);
    }

  /* matching from the stored form scores exactly like the live one */
  g_assert_cmpuint (elanpress_match_features (f, g), ==,
                    elanpress_match_features (f, f));
}

static void
test_unpack_rejects_garbage (void)
{
  guint8 buf[2 * ELANPRESS_PACKED_KP_LEN] = { 0 };
  gfloat nan_v = NAN, far_v = 1e6f;
  g_autoptr(ElanpressFeatures) empty = NULL;

  /* truncated */
  g_assert_null (elanpress_features_unpack (buf, sizeof (buf) - 1, W, H));

  /* too many keypoints */
  g_assert_null (elanpress_features_unpack (buf,
                                            (ELANPRESS_MAX_KEYPOINTS + 1) *
                                            ELANPRESS_PACKED_KP_LEN, W, H));

  /* non-finite and out-of-image coordinates */
  memcpy (buf, &nan_v, 4);
  g_assert_null (elanpress_features_unpack (buf, sizeof (buf), W, H));
  memcpy (buf, &far_v, 4);
  g_assert_null (elanpress_features_unpack (buf, sizeof (buf), W, H));

  /* no keypoints at all is valid (and matches nothing) */
  empty = elanpress_features_unpack (buf, 0, W, H);
  g_assert_nonnull (empty);
  g_assert_cmpuint (empty->n, ==, 0);
}

static void
test_touch_detection (void)
{
  const unsigned int size = W * H;
  g_autofree unsigned short *bg = g_new (unsigned short, size);
  g_autofree unsigned short *frame = g_new (unsigned short, size);
  ElanpressTouchStats stats;

  for (unsigned int i = 0; i < size; i++)
    bg[i] = frame[i] = 4000;

  /* untouched */
  g_assert_false (elanpress_frame_has_touch (frame, bg, size, &stats));
  g_assert_cmpfloat (stats.coverage, ==, 0);

  /* a light touch on a fifth of the pad is still a touch */
  for (unsigned int i = 0; i < size / 5; i++)
    frame[i] = 4000 + ELANPRESS_TOUCH_PIXEL_DELTA + 100;
  g_assert_true (elanpress_frame_has_touch (frame, bg, size, &stats));
  g_assert_cmpfloat (stats.neg_coverage, ==, 0);

  /* the same frame used as background: lifting the finger reads darker */
  g_assert_false (elanpress_frame_has_touch (bg, frame, size, &stats));
  g_assert_cmpfloat (stats.neg_coverage, >=, ELANPRESS_TOUCH_MIN_COVERAGE);

  /* a touch darker than the background yields no image */
  g_assert_null (elanpress_process_frame (bg, frame, size));
  g_assert_null (elanpress_process_frame (NULL, bg, size));
}

int
main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/elanpress/match/genuine-overlap", test_genuine_overlap_matches);
  g_test_add_func ("/elanpress/match/impostor", test_impostor_rejected);
  g_test_add_func ("/elanpress/match/spread-guard", test_spread_guard);
  g_test_add_func ("/elanpress/print/pack-roundtrip", test_pack_roundtrip);
  g_test_add_func ("/elanpress/print/unpack-garbage", test_unpack_rejects_garbage);
  g_test_add_func ("/elanpress/touch/detection", test_touch_detection);

  return g_test_run ();
}
