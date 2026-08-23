/*
 * Goodix 27c6:533c native driver for libfprint — Enrollment flow
 *
 * Copyright (C) 2026 libfprint contributors
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

/*
 * Enroll SSM shape ported near-verbatim from goodix53x5-enroll.c (sibling
 * driver, same SIGFM approach). goodix53x5 has REINIT/REINIT_DONE states to
 * recover from a suspend-induced stale USB claim; this driver has no
 * suspend/resume story yet (open()'s TLS/config/FDT-baseline bring-up is
 * assumed valid for the whole session), so those states are dropped here.
 */

#define FP_COMPONENT "goodix533c"

#include "drivers_api.h"
#include "goodix533c-private.h"
#include "goodix533c-match.h"
#include "goodix533c-enroll.h"

#include <string.h>

/* Settle time between an enrollment stage's finger-up wait and the next
 * stage's fresh reference capture, so sensor state from the just-released
 * touch doesn't bleed into the next capture. Same value goodix53x5 uses. */
#define GOODIX533C_ENROLL_RELEASE_SETTLE_MS 350

typedef enum {
  GOODIX533C_ENROLL_CAPTURE_REF = 0,
  GOODIX533C_ENROLL_WAIT_FINGER,
  GOODIX533C_ENROLL_CAPTURE,
  GOODIX533C_ENROLL_PROCESS,
  GOODIX533C_ENROLL_WAIT_FINGER_UP,
  GOODIX533C_ENROLL_NEXT,
  GOODIX533C_ENROLL_NUM_STATES,
} Goodix533cEnrollState;

static void
goodix533c_enroll_ssm_handler (FpiSsm   *ssm,
                               FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case GOODIX533C_ENROLL_CAPTURE_REF:
      if (fpi_device_action_is_cancelled (dev))
        {
          fpi_ssm_mark_failed (ssm,
                               g_error_new_literal (G_IO_ERROR,
                                                    G_IO_ERROR_CANCELLED,
                                                    "Enrollment cancelled"));
          return;
        }

      goodix533c_start_ref_capture_subsm (ssm, dev);
      break;

    case GOODIX533C_ENROLL_WAIT_FINGER:
      goodix533c_start_finger_wait_subsm (ssm, dev, NULL, NULL);
      break;

    case GOODIX533C_ENROLL_CAPTURE:
      goodix533c_start_live_capture_subsm (ssm, dev);
      break;

    case GOODIX533C_ENROLL_PROCESS:
      {
        GoodixMatchInfo *info;
        GBytes *feature;
        int keypoints;

        /* Partial-contact captures make weak templates -- ask the user to
         * re-place the finger instead of storing such a stage. This gate is
         * cheap and correct, though likely inert at this driver's current
         * headroom-safe gain -- see GOODIX533C_RAW12_CLIP's doc comment. */
        if (self->captured_clipped_fraction > GOODIX533C_ENROLL_MAX_CLIPPED_FRACTION)
          {
            fp_dbg ("Enrollment stage rejected: %.1f%% of frame has no "
                    "finger contact (limit %.1f%%)",
                    self->captured_clipped_fraction * 100.0,
                    GOODIX533C_ENROLL_MAX_CLIPPED_FRACTION * 100.0);
            g_clear_pointer (&self->captured_image, g_free);
            fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
            fpi_ssm_next_state (ssm);
            return;
          }

        info = goodix533c_match_extract (self->captured_image);
        keypoints = goodix533c_match_keypoints_count (info);

        if (keypoints < GOODIX533C_MIN_CAPTURE_KEYPOINTS)
          {
            goodix533c_match_free_info (info);
            g_clear_pointer (&self->captured_image, g_free);
            fpi_device_enroll_progress (dev, self->enroll_stage, NULL,
                                        fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
            fpi_ssm_next_state (ssm);
            return;
          }

        feature = goodix533c_match_serialize_template (info);
        goodix533c_match_free_info (info);
        if (feature == NULL)
          {
            g_clear_pointer (&self->captured_image, g_free);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                           "Failed to serialize SIGFM features"));
            return;
          }

        g_ptr_array_add (self->enroll_features, feature);
        g_clear_pointer (&self->captured_image, g_free);
        self->enroll_stage++;

        fp_dbg ("Enrollment stage %d/%d complete",
                self->enroll_stage, GOODIX533C_ENROLL_SAMPLES);

        fpi_device_enroll_progress (dev, self->enroll_stage, NULL, NULL);
        fpi_ssm_next_state (ssm);
      }
      break;

    case GOODIX533C_ENROLL_WAIT_FINGER_UP:
      goodix533c_start_finger_up_subsm (ssm, dev);
      break;

    case GOODIX533C_ENROLL_NEXT:
      if (self->enroll_stage < GOODIX533C_ENROLL_SAMPLES)
        {
          if (fpi_device_action_is_cancelled (dev))
            {
              fpi_ssm_mark_failed (ssm,
                                   g_error_new_literal (G_IO_ERROR,
                                                        G_IO_ERROR_CANCELLED,
                                                        "Enrollment cancelled"));
              return;
            }

          fp_dbg ("Waiting %dms for enrollment release to settle",
                  GOODIX533C_ENROLL_RELEASE_SETTLE_MS);
          fpi_ssm_jump_to_state_delayed (ssm, GOODIX533C_ENROLL_CAPTURE_REF,
                                         GOODIX533C_ENROLL_RELEASE_SETTLE_MS);
        }
      else
        fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
goodix533c_enroll_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  FpPrint *print = NULL;
  GVariantBuilder builder;
  GVariant *data;

  self->task_ssm = NULL;

  if (error)
    {
      g_clear_pointer (&self->enroll_features, g_ptr_array_unref);
      g_clear_pointer (&self->reference_pixels, g_free);
      g_clear_pointer (&self->captured_image, g_free);
      fpi_device_enroll_complete (dev, NULL, error);
      return;
    }

  /* Build print from serialized enrollment features. */
  fpi_device_get_enroll_data (dev, &print);
  fpi_print_set_type (print, FPI_PRINT_RAW);

  /* GVariant "aay" -- array of byte arrays, one per enrollment sample. */
  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));

  for (guint i = 0; i < self->enroll_features->len; i++)
    {
      GBytes *feature = g_ptr_array_index (self->enroll_features, i);
      gsize feature_len;
      const guint8 *feature_data = g_bytes_get_data (feature, &feature_len);

      g_variant_builder_add (&builder, "@ay",
                             g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                        feature_data,
                                                        feature_len,
                                                        1));
    }

  data = g_variant_builder_end (&builder);
  g_object_set (G_OBJECT (print), "fpi-data", data, NULL);

  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);

  fp_info ("Enrollment complete with %d samples", GOODIX533C_ENROLL_SAMPLES);

  fpi_device_enroll_complete (dev, g_object_ref (print), NULL);
}

void
goodix533c_enroll_start (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  FpiSsm *ssm;

  self->enroll_stage = 0;
  g_clear_pointer (&self->reference_pixels, g_free);
  self->have_reference = FALSE;
  g_clear_pointer (&self->captured_image, g_free);
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);
  self->enroll_features = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

  ssm = fpi_ssm_new (dev, goodix533c_enroll_ssm_handler,
                     GOODIX533C_ENROLL_NUM_STATES);
  self->task_ssm = ssm;
  fpi_ssm_start (ssm, goodix533c_enroll_ssm_done);
}
