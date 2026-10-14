/*
 * Driver for ELAN press-type (touch) fingerprint sensors
 * Copyright (C) 2026 Filip Spanne
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

#define FP_COMPONENT "elanpress"

#include "drivers_api.h"
#include "elanpress.h"
#include "elanpress-match.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

static const guint8 cmd_led_on[ELANPRESS_CMD_LEN] = {0x40, 0x31};
static const guint8 cmd_get_image[ELANPRESS_CMD_LEN] = {0x00, 0x09};
static const guint8 cmd_stop[ELANPRESS_CMD_LEN] = {0x00, 0x0b};
static const guint8 cmd_get_sensor_dim[ELANPRESS_CMD_LEN] = {0x00, 0x0c};

struct _FpiDeviceElanPress
{
  FpDevice        parent;

  unsigned short  frame_width;
  unsigned short  frame_height;

  /* raw background frame (no finger), rotated to row-major. It outlives
   * close: fprintd opens the device for every authentication, and retaking
   * it each time costs a frame and risks catching a finger that is already
   * landing for the very press the user meant to unlock with. */
  unsigned short *background;

  /* most recent frame polled from the sensor, not yet classified as a
   * touch or a clear reading */
  unsigned short *last_frame;

  /* the touch being captured: when it was first seen, the covered share of
   * the sensor in its previous frame, and its fullest frame so far, which
   * stands in for the touch if the finger lifts before it settles */
  gboolean        in_touch;
  gint64          touch_start_us;
  guint           touch_frames;
  gdouble         prev_coverage;
  unsigned short *best_frame;
  gdouble         best_coverage;

  /* the frame the completed touch was imaged with */
  unsigned short *touch_frame;

  /* hold capture off until the sensor has read clear for a while, so that
   * one finger left resting cannot stand in for several separate presses.
   * It carries over from one action to the next: a touch is imaged as soon
   * as it settles, well before the finger lifts, so without it a verify
   * retried after a no-match would image the same resting finger again
   * rather than wait for the next press. */
  gboolean        wait_for_finger_off;
  guint           finger_off_frames;
  gint64          hold_start_us;

  /* when the device was last closed. A client that gets a no-match back
   * typically closes and reopens the device right away (pam_fprintd with
   * max-tries=1 under a lock screen that restarts it), and the finger that
   * was just rejected is usually still resting. It must not be imaged again
   * as a fresh press, or a single press would count as several failures. */
  gint64          closed_at_us;

  /* consecutive clear polls while waiting for a press: idle_frames decides
   * when the background is worth retaking (and bg_refresh requests it),
   * idle_polls when to back off to the slower poll rate */
  guint           idle_frames;
  guint           idle_polls;
  gboolean        bg_refresh;

  /* per verify/identify/enroll */
  int             match_retries;
  int             recoveries;

  /* keypoints of the touches collected during enrollment */
  GPtrArray      *enroll_features;
  int             enroll_stage;

  /* a legacy (image-storing) print was seen; warn only once */
  gboolean        warned_legacy_print;
};

G_DEFINE_TYPE (FpiDeviceElanPress, fpi_device_elanpress, FP_TYPE_DEVICE);

/* === image processing === */

static unsigned int
elanpress_frame_size (FpiDeviceElanPress *self)
{
  return (unsigned int) self->frame_width * self->frame_height;
}

static guint8 *
elanpress_process_touch (FpiDeviceElanPress *self)
{
  return elanpress_process_frame (self->touch_frame, self->background,
                                  elanpress_frame_size (self));
}

/* write a processed touch to ELANPRESS_DUMP_DIR, if set, for offline
 * measurement of the score distributions */
static void
elanpress_dump_image (FpiDeviceElanPress *self, const guint8 *img,
                      const char *label, gdouble quality)
{
  const char *dir = g_getenv (ELANPRESS_DUMP_DIR_ENV);
  g_autofree char *name = NULL;
  g_autofree char *path = NULL;
  g_autofree char *header = NULL;
  int fd;

  if (!dir || !*dir || !img)
    return;

  name = g_strdup_printf ("%s-%" G_GINT64_FORMAT "-q%02d.pgm", label,
                          g_get_real_time (), (int) quality);
  path = g_build_filename (dir, name, NULL);
  fd = open (path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    {
      fp_warn ("cannot write %s: %s", path, g_strerror (errno));
      return;
    }

  header = g_strdup_printf ("P5\n%d %d\n255\n", self->frame_width,
                            self->frame_height);
  if (write (fd, header, strlen (header)) < 0 ||
      write (fd, img, elanpress_frame_size (self)) < 0)
    fp_warn ("cannot write %s: %s", path, g_strerror (errno));
  close (fd);
}

/* === print (de)serialization === */

static void
elanpress_print_set_data (FpiDeviceElanPress *self, FpPrint *print,
                          GPtrArray *features)
{
  GVariantBuilder builder = G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE ("aay"));
  GVariant *fpi_data;

  for (guint i = 0; i < features->len; i++)
    {
      gsize len = 0;
      guint8 *packed = elanpress_features_pack (g_ptr_array_index (features, i),
                                                &len);

      g_variant_builder_add_value (
        &builder,
        g_variant_new_from_data (G_VARIANT_TYPE_BYTESTRING, packed, len,
                                 TRUE, g_free, packed));
    }

  fpi_data = g_variant_new ("(qqqaay)",
                            (guint16) ELANPRESS_PRINT_VERSION,
                            (guint16) self->frame_width,
                            (guint16) self->frame_height,
                            &builder);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  g_object_set (print, "fpi-data", fpi_data, NULL);
}

/* the keypoints of every enrolled touch of the print, or NULL if the print
 * is not usable with this sensor */
static GPtrArray *
elanpress_print_get_features (FpiDeviceElanPress *self, FpPrint *print)
{
  g_autoptr(GVariant) fpi_data = NULL;
  g_autoptr(GVariant) items = NULL;
  g_autoptr(GPtrArray) features = NULL;
  guint16 version = 0, width = 0, height = 0;
  gsize n_items;

  g_object_get (print, "fpi-data", &fpi_data, NULL);
  if (!fpi_data || !g_variant_check_format_string (fpi_data, "(qqq@aay)", FALSE))
    return NULL;

  g_variant_get (fpi_data, "(qqq@aay)", &version, &width, &height, &items);
  if (width != self->frame_width || height != self->frame_height)
    return NULL;
  if (version != ELANPRESS_PRINT_VERSION &&
      version != ELANPRESS_PRINT_VERSION_IMAGES)
    return NULL;

  n_items = g_variant_n_children (items);
  if (n_items == 0 || n_items > 4 * ELANPRESS_ENROLL_STAGES)
    return NULL;

  if (version == ELANPRESS_PRINT_VERSION_IMAGES && !self->warned_legacy_print)
    {
      fp_warn ("print stores enrolled fingerprint images (format %d); "
               "re-enroll to replace it with keypoints only",
               ELANPRESS_PRINT_VERSION_IMAGES);
      self->warned_legacy_print = TRUE;
    }

  features = g_ptr_array_new_full (n_items,
                                   (GDestroyNotify) elanpress_features_free);
  for (gsize i = 0; i < n_items; i++)
    {
      g_autoptr(GVariant) item = g_variant_get_child_value (items, i);
      gsize len = 0;
      const guint8 *data = g_variant_get_fixed_array (item, &len, sizeof (guint8));
      ElanpressFeatures *f;

      if (version == ELANPRESS_PRINT_VERSION_IMAGES)
        {
          if (len != elanpress_frame_size (self))
            return NULL;
          f = elanpress_image_features (data, self->frame_width,
                                        self->frame_height);
        }
      else
        {
          f = elanpress_features_unpack (data, len, self->frame_width,
                                         self->frame_height);
          if (!f)
            return NULL;
        }
      g_ptr_array_add (features, f);
    }

  return g_steal_pointer (&features);
}

/* the most keypoint pairs the probe shares with any one touch of the print,
 * or -1 if the print is unusable */
static gint
elanpress_match_print (FpiDeviceElanPress *self, const ElanpressFeatures *probe,
                       FpPrint *print)
{
  g_autoptr(GPtrArray) enrolled = elanpress_print_get_features (self, print);
  gint best = -1;

  if (!enrolled)
    return -1;

  for (guint i = 0; i < enrolled->len; i++)
    {
      guint pairs = elanpress_match_features (probe,
                                              g_ptr_array_index (enrolled, i));

      fp_dbg ("  enrolled touch %u/%u: %u pairs", i + 1, enrolled->len, pairs);
      best = MAX (best, (gint) pairs);
      /* one confident hit is enough; spare the remaining comparisons */
      if (best >= ELANPRESS_MATCH_MIN_PAIRS)
        break;
    }

  return best;
}

/* === USB helpers ===
 *
 * Transfers are never submitted with the action's cancellable. Cancelling a
 * bulk read halfway through a frame leaves the rest of that frame queued on
 * the sensor, and the next open then found it wedged: on the lock screen,
 * whose fingerprint session times out and restarts every 30 seconds, the
 * sensor dropped off the bus and re-enumerated on most restarts, taking any
 * tap made in that window with it. Instead every transfer runs to
 * completion (they take milliseconds) and the capture state machine checks
 * for cancellation between them. */

static void
elanpress_send_cmd (FpiSsm *ssm, FpDevice *dev, const guint8 *cmd)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk_full (transfer, ELANPRESS_EP_CMD_OUT,
                                   (guint8 *) cmd, ELANPRESS_CMD_LEN, NULL);
  fpi_usb_transfer_submit (transfer, ELANPRESS_CMD_TIMEOUT, NULL,
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
elanpress_read (FpiSsm *ssm, FpDevice *dev, guint8 ep, gsize len,
                guint timeout, FpiUsbTransferCallback callback)
{
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->ssm = ssm;
  transfer->short_is_error = TRUE;
  fpi_usb_transfer_fill_bulk (transfer, ep, len);
  fpi_usb_transfer_submit (transfer, timeout, NULL, callback, NULL);
}

/* turn the sensor off after an action */
static void
elanpress_send_stop (FpDevice *dev)
{
  g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (dev);
  g_autoptr(GError) error = NULL;

  fpi_usb_transfer_fill_bulk_full (transfer, ELANPRESS_EP_CMD_OUT,
                                   (guint8 *) cmd_stop, ELANPRESS_CMD_LEN,
                                   NULL);
  if (!fpi_usb_transfer_submit_sync (transfer, ELANPRESS_STOP_TIMEOUT, &error))
    fp_warn ("failed to send stop command: %s", error->message);
}

/* read off whatever image data is still queued on the sensor */
static void
elanpress_drain (FpDevice *dev)
{
  for (int i = 0; i < ELANPRESS_DRAIN_MAX_READS; i++)
    {
      g_autoptr(FpiUsbTransfer) transfer = fpi_usb_transfer_new (dev);
      g_autoptr(GError) error = NULL;

      fpi_usb_transfer_fill_bulk (transfer, ELANPRESS_EP_IMG_IN, 0x10000);
      if (!fpi_usb_transfer_submit_sync (transfer, ELANPRESS_DRAIN_TIMEOUT,
                                         &error))
        return;
      fp_dbg ("drained %zd stale bytes from the image endpoint",
              transfer->actual_length);
    }
}

/* clear a halt on every endpoint the driver uses */
static void
elanpress_clear_halts (FpDevice *dev)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (dev);
  const guint8 eps[] = {
    ELANPRESS_EP_CMD_OUT, ELANPRESS_EP_CMD_IN, ELANPRESS_EP_IMG_IN
  };

  for (guint i = 0; i < G_N_ELEMENTS (eps); i++)
    {
      g_autoptr(GError) error = NULL;

      if (!g_usb_device_control_transfer (usb_dev,
                                          G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                          G_USB_DEVICE_REQUEST_TYPE_STANDARD,
                                          G_USB_DEVICE_RECIPIENT_ENDPOINT,
                                          0x01, /* CLEAR_FEATURE */
                                          0x00, /* ENDPOINT_HALT */
                                          eps[i], NULL, 0, NULL,
                                          ELANPRESS_STOP_TIMEOUT, NULL, &error))
        fp_dbg ("failed to clear halt on ep 0x%02x: %s", eps[i], error->message);
    }
}

/* === touch capture state machine === */

/* Finger presence is inferred from the image rather than polled with
 * cmd_pre_scan: that command's status byte reports correctly once per
 * power-up and then answers "finger present" forever, which left capture
 * spinning in its wait-for-lift loop until the operation timed out, with
 * only a full power cycle to clear it. Comparing each frame against a
 * background taken with the sensor clear tells presence apart without it. */

enum capture_states {
  CAPTURE_LED_ON,
  CAPTURE_BG_REQUEST,
  CAPTURE_BG_READ,
  CAPTURE_POLL_REQUEST,
  CAPTURE_POLL_READ,
  CAPTURE_DECIDE,
  CAPTURE_NUM_STATES,
};

static void
elanpress_reset_capture (FpiDeviceElanPress *self)
{
  self->in_touch = FALSE;
  self->touch_frames = 0;
  self->best_coverage = 0;
  self->prev_coverage = 0;
  self->idle_frames = 0;
  self->idle_polls = 0;
  self->bg_refresh = FALSE;
  g_clear_pointer (&self->last_frame, g_free);
  g_clear_pointer (&self->best_frame, g_free);
  g_clear_pointer (&self->touch_frame, g_free);
}

static guint
elanpress_idle_delay (FpiDeviceElanPress *self)
{
  return self->idle_polls >= ELANPRESS_IDLE_FAST_POLLS ?
         ELANPRESS_IDLE_SLOW_POLL_MS : ELANPRESS_IDLE_POLL_INTERVAL_MS;
}

static void
elanpress_frame_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                    gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  unsigned int size = elanpress_frame_size (self);
  unsigned short *frame;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  frame = g_malloc (size * sizeof (unsigned short));
  elanpress_rotate_frame (transfer->buffer, frame,
                          self->frame_width, self->frame_height);

  if (fpi_ssm_get_cur_state (transfer->ssm) == CAPTURE_BG_READ)
    {
      self->bg_refresh = FALSE;
      self->idle_frames = 0;

      if (!self->background)
        {
          self->background = frame;
          fp_dbg ("captured background frame");
        }
      else if (!elanpress_frame_has_touch (frame, self->background, size, NULL))
        {
          /* follows drift either way, as long as the candidate does not
           * look like a finger landed on the sensor since the last one */
          g_free (self->background);
          self->background = frame;
          fp_dbg ("background refreshed");
        }
      else
        {
          /* adopting it would write that finger into the reference and
           * hide it for good */
          fp_dbg ("background candidate looks touched, keeping the old one");
          g_free (frame);
        }
    }
  else
    {
      g_free (self->last_frame);
      self->last_frame = frame;
    }

  fpi_ssm_next_state (transfer->ssm);
}

/* the frame just polled while waiting for the previous press to lift */
static void
capture_decide_wait_off (FpiSsm *ssm, FpiDeviceElanPress *self, gboolean has_touch)
{
  FpDevice *dev = FP_DEVICE (self);

  if (has_touch)
    {
      gint64 now = g_get_monotonic_time ();

      self->finger_off_frames = 0;
      if (self->hold_start_us == 0)
        self->hold_start_us = now;

      if (now - self->hold_start_us >= ELANPRESS_STUCK_TOUCH_MS * 1000)
        {
          fp_dbg ("sensor read touched for %d ms, taking it as the new background",
                  ELANPRESS_STUCK_TOUCH_MS);
          g_free (self->background);
          self->background = g_steal_pointer (&self->last_frame);
          self->hold_start_us = 0;
          self->wait_for_finger_off = FALSE;
          fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
        }
    }
  else if (++self->finger_off_frames >= ELANPRESS_FINGER_OFF_FRAMES)
    {
      fp_dbg ("sensor clear, ready for the next press");
      self->wait_for_finger_off = FALSE;
      self->finger_off_frames = 0;
      self->hold_start_us = 0;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
    }

  g_clear_pointer (&self->last_frame, g_free);
  fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_POLL_REQUEST,
                                 ELANPRESS_IDLE_POLL_INTERVAL_MS);
}

/* a touched frame: image the touch once it has settled */
static void
capture_decide_touch (FpiSsm *ssm, FpiDeviceElanPress *self,
                      const ElanpressTouchStats *stats)
{
  gint64 now = g_get_monotonic_time ();
  gint64 elapsed_ms;
  gboolean settled;

  fpi_device_report_finger_status (FP_DEVICE (self),
                                   FP_FINGER_STATUS_NEEDED |
                                   FP_FINGER_STATUS_PRESENT);
  self->idle_frames = 0;
  self->idle_polls = 0;
  self->touch_frames++;

  if (!self->in_touch)
    {
      /* the finger has just landed */
      self->in_touch = TRUE;
      self->touch_start_us = now;
      self->prev_coverage = stats->coverage;
      self->best_coverage = stats->coverage;
      g_clear_pointer (&self->best_frame, g_free);
      self->best_frame = g_steal_pointer (&self->last_frame);
      fpi_ssm_jump_to_state (ssm, CAPTURE_POLL_REQUEST);
      return;
    }

  elapsed_ms = (now - self->touch_start_us) / 1000;
  settled = elapsed_ms >= ELANPRESS_SETTLE_MIN_MS &&
            fabs (stats->coverage - self->prev_coverage) <=
            ELANPRESS_SETTLE_COVERAGE_DELTA;
  self->prev_coverage = stats->coverage;

  if (settled || elapsed_ms >= ELANPRESS_SETTLE_MAX_MS)
    {
      /* the newest frame is the most settled one, unless the finger is
       * already lifting off and the contact area has shrunk */
      if (stats->coverage >= 0.8 * self->best_coverage)
        self->touch_frame = g_steal_pointer (&self->last_frame);
      else
        self->touch_frame = g_steal_pointer (&self->best_frame);

      fp_dbg ("touch imaged after %" G_GINT64_FORMAT " ms and %u frames (%s)",
              elapsed_ms, self->touch_frames, settled ? "settled" : "time limit");
      fpi_ssm_mark_completed (ssm);
      return;
    }

  if (stats->coverage >= self->best_coverage)
    {
      g_free (self->best_frame);
      self->best_frame = g_steal_pointer (&self->last_frame);
      self->best_coverage = stats->coverage;
    }
  else
    {
      g_clear_pointer (&self->last_frame, g_free);
    }

  fpi_ssm_jump_to_state (ssm, CAPTURE_POLL_REQUEST);
}

static void
capture_decide (FpiSsm *ssm, FpiDeviceElanPress *self)
{
  ElanpressTouchStats stats = { 0, 0, 0 };
  gboolean has_touch;

  has_touch = elanpress_frame_has_touch (self->last_frame, self->background,
                                         elanpress_frame_size (self), &stats);

  fp_dbg ("poll: %s (coverage %.1f%%, below bg %.1f%%, mean delta %.0f)",
          has_touch ? "touch" : "clear", stats.coverage * 100,
          stats.neg_coverage * 100, stats.mean_delta);

  /* a finger only ever adds signal, so a frame reading this far below the
   * background means the background was taken with a finger on the sensor,
   * which would hide that part of every later touch. This frame is clear by
   * construction: make it the background straight away. */
  if (!has_touch && stats.neg_coverage >= ELANPRESS_TOUCH_MIN_COVERAGE)
    {
      fp_dbg ("frame reads darker than the background, retaking it");
      g_free (self->background);
      self->background = g_steal_pointer (&self->last_frame);
      self->idle_frames = 0;
    }

  if (self->wait_for_finger_off)
    {
      capture_decide_wait_off (ssm, self, has_touch);
      return;
    }

  if (has_touch)
    {
      capture_decide_touch (ssm, self, &stats);
      return;
    }

  g_clear_pointer (&self->last_frame, g_free);

  if (self->in_touch)
    {
      /* the finger lifted before the touch settled: a quick tap. Use the
       * fullest frame it produced rather than dropping it. */
      self->touch_frame = g_steal_pointer (&self->best_frame);
      fp_dbg ("tap lifted after %u frames, using its fullest frame (coverage %.1f%%)",
              self->touch_frames, self->best_coverage * 100);
      fpi_ssm_mark_completed (ssm);
      return;
    }

  self->idle_polls++;
  if (++self->idle_frames >= ELANPRESS_BG_REFRESH_FRAMES)
    {
      fp_dbg ("sensor clear for %u polls, retaking the background",
              self->idle_frames);
      self->bg_refresh = TRUE;
      fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_BG_REQUEST,
                                     elanpress_idle_delay (self));
      return;
    }

  fpi_ssm_jump_to_state_delayed (ssm, CAPTURE_POLL_REQUEST,
                                 elanpress_idle_delay (self));
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  gsize frame_bytes = elanpress_frame_size (self) * 2;

  /* transfers are not cancellable (see the USB helpers), so this is where
   * cancellation takes effect: between two complete transfers */
  if (fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm, g_error_new_literal (G_IO_ERROR,
                                                     G_IO_ERROR_CANCELLED,
                                                     "Action cancelled"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_LED_ON:
      elanpress_send_cmd (ssm, dev, cmd_led_on);
      break;

    case CAPTURE_BG_REQUEST:
      if (self->background && !self->bg_refresh)
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_POLL_REQUEST);
          break;
        }
      elanpress_send_cmd (ssm, dev, cmd_get_image);
      break;

    case CAPTURE_BG_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    case CAPTURE_POLL_REQUEST:
      if (!self->wait_for_finger_off && !self->in_touch)
        fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
      elanpress_send_cmd (ssm, dev, cmd_get_image);
      break;

    case CAPTURE_POLL_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_IMG_IN, frame_bytes,
                      ELANPRESS_FRAME_TIMEOUT, elanpress_frame_cb);
      break;

    case CAPTURE_DECIDE:
      capture_decide (ssm, self);
      break;
    }
}

static void
elanpress_capture_touch (FpiDeviceElanPress *self, FpiSsmCompletedCallback done)
{
  elanpress_reset_capture (self);
  fpi_ssm_start (fpi_ssm_new (FP_DEVICE (self), capture_run_state,
                              CAPTURE_NUM_STATES),
                 done);
}

/* After a USB error mid-capture, reset the endpoints and start the capture
 * over instead of failing the action, a bounded number of times. Returns
 * FALSE when the error has to be reported instead. */
static gboolean
elanpress_recover (FpiDeviceElanPress *self, const GError *error,
                   FpiSsmCompletedCallback done)
{
  FpDevice *dev = FP_DEVICE (self);

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
      g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_NO_DEVICE) ||
      fpi_device_action_is_cancelled (dev) ||
      self->recoveries >= ELANPRESS_MAX_RECOVERIES)
    return FALSE;

  self->recoveries++;
  fp_warn ("capture failed (%s), resetting the sensor (attempt %d of %d)",
           error->message, self->recoveries, ELANPRESS_MAX_RECOVERIES);

  elanpress_clear_halts (dev);
  elanpress_send_stop (dev);
  elanpress_drain (dev);
  /* whatever state the sensor comes back in, the old reference may not fit */
  g_clear_pointer (&self->background, g_free);

  elanpress_capture_touch (self, done);
  return TRUE;
}

/* === enroll === */

static void
elanpress_enroll_touch_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  g_autoptr(ElanpressFeatures) features = NULL;
  FpPrint *print = NULL;
  g_autofree guint8 *image = NULL;
  gdouble quality = 0;

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (error)
    {
      if (elanpress_recover (self, error, elanpress_enroll_touch_done))
        {
          g_error_free (error);
          return;
        }
      elanpress_reset_capture (self);
      g_clear_pointer (&self->enroll_features, g_ptr_array_unref);
      elanpress_send_stop (dev);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  image = elanpress_process_touch (self);
  elanpress_reset_capture (self);

  if (image)
    {
      quality = elanpress_image_quality (image, elanpress_frame_size (self));
      elanpress_dump_image (self, image, "enroll", quality);

      /* A light or partial press yields an image that correlates weakly
       * against everything, so storing one both loses a stage and drags the
       * template towards matching anyone. Ask for the press again instead. */
      if (quality < ELANPRESS_MIN_QUALITY_STDDEV)
        {
          fp_dbg ("touch too low-contrast to enroll (std-dev %.1f < %.1f)",
                  quality, ELANPRESS_MIN_QUALITY_STDDEV);
          g_clear_pointer (&image, g_free);
        }
    }

  if (image)
    {
      features = elanpress_image_features (image, self->frame_width,
                                           self->frame_height);
      /* a stored touch with fewer keypoints than a match needs pairs can
       * never be matched against, so it would only waste a stage */
      if (features->n < ELANPRESS_MATCH_MIN_PAIRS)
        {
          fp_dbg ("touch has only %u keypoints, asking again", features->n);
          g_clear_pointer (&features, elanpress_features_free);
        }
    }

  /* each stage has to be its own press, not a continuation of this one */
  self->wait_for_finger_off = TRUE;
  self->finger_off_frames = 0;

  if (!features)
    {
      fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  fp_dbg ("enroll stage %d: %u keypoints, std-dev %.1f",
          self->enroll_stage + 1, features->n, quality);
  g_ptr_array_add (self->enroll_features, g_steal_pointer (&features));
  self->enroll_stage++;
  fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);

  if (self->enroll_stage < ELANPRESS_ENROLL_STAGES)
    {
      elanpress_capture_touch (self, elanpress_enroll_touch_done);
      return;
    }

  elanpress_send_stop (dev);

  fpi_device_get_enroll_data (dev, &print);
  elanpress_print_set_data (self, print, self->enroll_features);
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);

  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

static void
elanpress_enroll (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  self->enroll_stage = 0;
  self->recoveries = 0;
  self->finger_off_frames = 0;
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);
  self->enroll_features =
    g_ptr_array_new_with_free_func ((GDestroyNotify) elanpress_features_free);

  elanpress_capture_touch (self, elanpress_enroll_touch_done);
}

/* === verify and identify ===
 *
 * Each press is scored once and decided on its own. Resampling until some
 * press cleared the threshold made the verdict the best of everything tried,
 * and a maximum over that many chances lifts a stranger's score about as
 * readily as the owner's, which is the likely reason an earlier version was
 * reported to accept any finger. A false reject costs one more press. */

static void
elanpress_match_complete (FpDevice *dev, FpPrint *match, gboolean matched,
                          GError *retry, GError *error)
{
  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      if (!error)
        fpi_device_identify_report (dev, match, NULL, retry);
      fpi_device_identify_complete (dev, error);
    }
  else
    {
      if (!error)
        fpi_device_verify_report (dev,
                                  retry ? FPI_MATCH_ERROR :
                                  matched ? FPI_MATCH_SUCCESS : FPI_MATCH_FAIL,
                                  NULL, retry);
      fpi_device_verify_complete (dev, error);
    }
}

static void
elanpress_match_touch_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  g_autofree guint8 *probe = NULL;
  g_autoptr(ElanpressFeatures) features = NULL;
  FpPrint *matched_print = NULL;
  gdouble quality = 0.0;
  gint best = -1;

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);

  if (error)
    {
      if (elanpress_recover (self, error, elanpress_match_touch_done))
        {
          g_error_free (error);
          return;
        }
      elanpress_reset_capture (self);
      elanpress_send_stop (dev);
      elanpress_match_complete (dev, NULL, FALSE, NULL, error);
      return;
    }

  probe = elanpress_process_touch (self);
  elanpress_reset_capture (self);

  if (probe)
    {
      quality = elanpress_image_quality (probe, elanpress_frame_size (self));
      elanpress_dump_image (self, probe, "verify", quality);

      /* too little ridge detail to say anything about whose finger this is */
      if (quality < ELANPRESS_MIN_QUALITY_STDDEV)
        {
          fp_dbg ("press too low-contrast to score (std-dev %.1f < %.1f)",
                  quality, ELANPRESS_MIN_QUALITY_STDDEV);
          g_clear_pointer (&probe, g_free);
        }
    }

  /* whatever happens next needs its own press */
  self->wait_for_finger_off = TRUE;
  self->finger_off_frames = 0;

  if (!probe)
    {
      /* a press that produced nothing is not evidence either way, so ask
       * again rather than scoring it */
      if (self->match_retries < ELANPRESS_MATCH_MAX_RETRIES)
        {
          self->match_retries++;
          fp_dbg ("unusable press, asking again (retry %d of %d)",
                  self->match_retries, ELANPRESS_MATCH_MAX_RETRIES);
          elanpress_capture_touch (self, elanpress_match_touch_done);
          return;
        }

      elanpress_send_stop (dev);
      elanpress_match_complete (dev, NULL, FALSE,
                                fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL),
                                NULL);
      return;
    }

  features = elanpress_image_features (probe, self->frame_width,
                                       self->frame_height);

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *gallery = NULL;

      fpi_device_get_identify_data (dev, &gallery);
      for (guint i = 0; gallery && i < gallery->len; i++)
        {
          FpPrint *print = g_ptr_array_index (gallery, i);
          gint c = elanpress_match_print (self, features, print);

          /* the best scoring print wins, so that among several enrolled
           * fingers the one actually pressed is reported */
          if (c > best)
            {
              best = c;
              matched_print = print;
            }
        }
    }
  else
    {
      FpPrint *print = NULL;

      fpi_device_get_verify_data (dev, &print);
      best = elanpress_match_print (self, features, print);
      matched_print = print;

      /* the stored print itself is unusable; more presses cannot help */
      if (best < 0)
        {
          elanpress_send_stop (dev);
          elanpress_match_complete (dev, NULL, FALSE, NULL,
                                    fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }
    }

  fp_dbg ("%s: %d pairs (threshold %d) from %u keypoints, std-dev %.1f",
          best >= ELANPRESS_MATCH_MIN_PAIRS ? "MATCH" : "no match",
          best, ELANPRESS_MATCH_MIN_PAIRS, features->n, quality);

  elanpress_send_stop (dev);
  if (best >= ELANPRESS_MATCH_MIN_PAIRS)
    elanpress_match_complete (dev, matched_print, TRUE, NULL, NULL);
  else
    elanpress_match_complete (dev, NULL, FALSE, NULL, NULL);
}

static void
elanpress_identify_verify (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);

  self->match_retries = 0;
  self->recoveries = 0;
  self->finger_off_frames = 0;

  elanpress_capture_touch (self, elanpress_match_touch_done);
}

/* === open and close === */

enum open_states {
  OPEN_GET_DIM_SEND,
  OPEN_GET_DIM_READ,
  OPEN_NUM_STATES,
};

static void
elanpress_dim_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                  gpointer user_data, GError *error)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  unsigned short width, height;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* raw frames are column-major: byte 0 is the column height, byte 2 the
   * number of columns (see the elan driver) */
  height = transfer->buffer[0];
  width = transfer->buffer[2];

  /* work around sensors returning the sizes as zero-based index rather
   * than the number of pixels (same quirk as the elan driver) */
  if ((width % 2 == 1) && (height % 2 == 1))
    {
      width++;
      height++;
    }
  fp_dbg ("sensor dimensions, WxH: %dx%d", width, height);

  if (width < 50 || height < 20)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Unexpected sensor dimensions %dx%d",
                                                     width, height));
      return;
    }

  /* a background kept from an earlier open only fits the same geometry */
  if (width != self->frame_width || height != self->frame_height)
    g_clear_pointer (&self->background, g_free);
  self->frame_width = width;
  self->frame_height = height;

  fpi_ssm_next_state (transfer->ssm);
}

static void
open_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_GET_DIM_SEND:
      elanpress_send_cmd (ssm, dev, cmd_get_sensor_dim);
      break;

    case OPEN_GET_DIM_READ:
      elanpress_read (ssm, dev, ELANPRESS_EP_CMD_IN, 4,
                      ELANPRESS_CMD_TIMEOUT, elanpress_dim_cb);
      break;
    }
}

static void
open_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_device_open_complete (dev, error);
}

static void
elanpress_open (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  GError *error = NULL;

  /* a finger already on the sensor when the device is opened is the press
   * the user meant for it, so it is imaged rather than waited out, unless it
   * is the press the previous session just imaged */
  if (self->wait_for_finger_off && self->closed_at_us &&
      g_get_monotonic_time () - self->closed_at_us <
      ELANPRESS_REOPEN_HOLD_MS * 1000)
    fp_dbg ("reopened right after a touch, waiting for it to lift first");
  else
    self->wait_for_finger_off = FALSE;
  self->finger_off_frames = 0;
  self->hold_start_us = 0;

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  /* start from a known state: whatever a previous user of the sensor left
   * running is stopped, and any frame it left queued is read off */
  elanpress_send_stop (dev);
  elanpress_drain (dev);

  fpi_ssm_start (fpi_ssm_new (dev, open_run_state, OPEN_NUM_STATES),
                 open_complete);
}

static void
elanpress_close (FpDevice *dev)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (dev);
  GError *error = NULL;

  /* the background is kept on purpose, see the struct */
  elanpress_reset_capture (self);
  self->closed_at_us = g_get_monotonic_time ();
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);

  g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                  0, 0, &error);
  fpi_device_close_complete (dev, error);
}

static void
elanpress_cancel (FpDevice *dev)
{
  /* the capture state machine checks for cancellation between transfers;
   * nothing to abort here (see the USB helpers for why) */
}

static void
fpi_device_elanpress_finalize (GObject *object)
{
  FpiDeviceElanPress *self = FPI_DEVICE_ELANPRESS (object);

  elanpress_reset_capture (self);
  g_clear_pointer (&self->background, g_free);
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);

  G_OBJECT_CLASS (fpi_device_elanpress_parent_class)->finalize (object);
}

static void
fpi_device_elanpress_init (FpiDeviceElanPress *self)
{
}

static void
fpi_device_elanpress_class_init (FpiDeviceElanPressClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_elanpress_finalize;

  dev_class->id = "elanpress";
  dev_class->full_name = "ElanTech press-type fingerprint sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = elanpress_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->nr_enroll_stages = ELANPRESS_ENROLL_STAGES;
  /* the sensor spends lock-screen sessions waiting for a finger; it is a
   * tiny capacitive sensor designed to be always-on */
  dev_class->temp_hot_seconds = -1;

  dev_class->open = elanpress_open;
  dev_class->close = elanpress_close;
  dev_class->enroll = elanpress_enroll;
  dev_class->verify = elanpress_identify_verify;
  dev_class->identify = elanpress_identify_verify;
  dev_class->cancel = elanpress_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
}
