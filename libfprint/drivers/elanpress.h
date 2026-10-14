/*
 * Driver for ELAN press-type (touch) fingerprint sensors
 * Copyright (C) 2026 Filip Spanne
 *
 * These sensors stream raw frames like the swipe sensors handled by the
 * elan driver, but the finger rests on the sensor instead of being swiped
 * across it. The imaged area is far too small for reliable minutiae
 * matching, so enrollment stores the images themselves and matching pairs
 * up local keypoint features between them, like the Windows driver does.
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

#pragma once

#include "fpi-device.h"
#include "fpi-usb-transfer.h"

#define ELANPRESS_VEND_ID 0x04f3

#define ELANPRESS_EP_CMD_OUT (0x1 | FPI_USB_ENDPOINT_OUT)
#define ELANPRESS_EP_CMD_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define ELANPRESS_EP_IMG_IN (0x2 | FPI_USB_ENDPOINT_IN)

#define ELANPRESS_CMD_LEN 2
#define ELANPRESS_CMD_TIMEOUT 5000
#define ELANPRESS_FRAME_TIMEOUT 2000

/* the stop command is sent synchronously on the main loop when an action
 * ends; a wedged sensor must not stall fprintd for the full command timeout */
#define ELANPRESS_STOP_TIMEOUT 500

/* stale image data left on the image endpoint (from an action that ended
 * mid-frame in an older build, or a crashed fprintd) is read off at open with
 * this timeout per attempt, so the first poll does not receive half a frame */
#define ELANPRESS_DRAIN_TIMEOUT 25
#define ELANPRESS_DRAIN_MAX_READS 4

/* Waiting for a finger: each poll pulls a whole frame, so the transfer
 * already paces the loop. Right after the action starts, or after a touch,
 * polls follow each other almost back to back, since that is when a finger
 * is most likely to land. Once the sensor has read clear for
 * ELANPRESS_IDLE_FAST_POLLS polls (a few seconds), the loop backs off to
 * ELANPRESS_IDLE_SLOW_POLL_MS between polls, which roughly halves the USB and
 * CPU load of a lock screen sitting idle for hours, for at most that many
 * milliseconds of extra latency on the next touch. */
#define ELANPRESS_IDLE_POLL_INTERVAL_MS 5
#define ELANPRESS_IDLE_SLOW_POLL_MS 20
#define ELANPRESS_IDLE_FAST_POLLS 150

/* consecutive untouched frames that end a touch. Presence is inferred from
 * the image itself (see ELANPRESS_TOUCH_MIN_COVERAGE) rather than asked of
 * cmd_pre_scan, whose status byte answers once per power-up and then wedges
 * at "finger present" for good, stranding capture in a wait-for-lift loop
 * that only a full power cycle could clear. Requiring a run of clear frames
 * keeps sensor noise from ending a touch early, and keeps a finger left
 * resting on the sensor from being counted as the next press. */
#define ELANPRESS_FINGER_OFF_FRAMES 10

/* reopening the device within this long of closing it keeps waiting for the
 * last imaged finger to lift (see FpiDeviceElanPress.closed_at_us) */
#define ELANPRESS_REOPEN_HOLD_MS 5000

/* consecutive clear polls after which the background frame is retaken while
 * waiting for a press, to follow the sensor's drift. A background that has a
 * finger in it is caught much sooner than this: the first frame after the
 * finger lifts reads darker than it (see ElanpressTouchStats.neg_coverage). */
#define ELANPRESS_BG_REFRESH_FRAMES 50

/* a "touch" that has not lifted after this long, while waiting for the
 * sensor to read clear, is taken to be a background gone stale (drift after
 * a long idle or a resume) rather than a finger, and becomes the new
 * background. A finger really resting that long is absorbed into the
 * background, and recovered from as soon as it lifts. */
#define ELANPRESS_STUCK_TOUCH_MS 10000

/* When the finger is imaged.
 *
 * The first frames of a touch catch the finger still landing, with only part
 * of it in contact; from about 100 ms on the image is as good as any later
 * one. Rather than wait a fixed time, every frame of the touch is looked at
 * as it arrives, and the touch is imaged as soon as its contact area stops
 * growing: once ELANPRESS_SETTLE_MIN_MS have passed and the covered share of
 * the sensor changed by at most ELANPRESS_SETTLE_COVERAGE_DELTA since the
 * previous frame. A firm press settles in two or three frames. A touch still
 * changing at ELANPRESS_SETTLE_MAX_MS is imaged then regardless.
 *
 * A tap that lifts before settling is not thrown away: the frame with the
 * largest contact area seen during it is used. Before, the finger had to stay
 * down for the whole fixed settle delay or the touch was silently dropped,
 * which is exactly what a light tap does. */
#define ELANPRESS_SETTLE_MIN_MS 40
#define ELANPRESS_SETTLE_MAX_MS 150
#define ELANPRESS_SETTLE_COVERAGE_DELTA 0.03

/* number of touches stored during enrollment. The sensor sees a different
 * part of the finger on each press, and a press only matches an enrolled one
 * it overlaps, so more touches cover more of the finger: measured on
 * recorded presses, 1 genuine press in 40 failed to match against 8 stored
 * touches, 1 in 80 against 12 and 1 in 170 against 16. */
#define ELANPRESS_ENROLL_STAGES 12

/* extra presses allowed when one yields no usable image at all: too little
 * contrast to be worth scoring. A press that produced nothing is not evidence
 * either way, whereas one that produced a poor score is. */
#define ELANPRESS_MATCH_MAX_RETRIES 3

/* a USB error during capture (a stalled endpoint, a timeout after resume) is
 * recovered from by clearing the endpoint halts and starting the capture over,
 * this many times per action, before the error is reported */
#define ELANPRESS_MAX_RECOVERIES 2

/* version tags for the serialized print data. Version 1 stored the enrolled
 * images themselves and is still read, so existing enrollments keep working,
 * but a print is only ever written as version 2: the SIFT keypoints of each
 * enrolled image, no image. Re-enrolling replaces a version 1 print. */
#define ELANPRESS_PRINT_VERSION_IMAGES 1
#define ELANPRESS_PRINT_VERSION 2

/* when set in fprintd's environment, every processed touch is written there
 * as a PGM image, for measuring score distributions with
 * tools/elanpress-eval. These are pictures of fingerprints: only ever set it
 * for a measurement session, and delete the directory afterwards. */
#define ELANPRESS_DUMP_DIR_ENV "ELANPRESS_DUMP_DIR"

G_DECLARE_FINAL_TYPE (FpiDeviceElanPress, fpi_device_elanpress, FPI,
                      DEVICE_ELANPRESS, FpDevice);

/* 0c4f and 0c6e, listed here originally, are also claimed by the elan swipe
 * driver; two drivers claiming one ID with equal score leaves the choice to
 * registration order. Only the sensor this driver is tested with is claimed. */
static const FpIdEntry elanpress_id_table[] = {
  {.vid = ELANPRESS_VEND_ID, .pid = 0x0903, }, /* 04f3:0903 ASUS VivoBook 16X */
  {.vid = 0, .pid = 0, },
};
