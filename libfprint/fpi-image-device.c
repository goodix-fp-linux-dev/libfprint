/*
 * FpImageDevice - An image based fingerprint reader device - Private APIs
 * Copyright (C) 2019 Benjamin Berg <bberg@redhat.com>
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

#define FP_COMPONENT "image_device"
#include "fpi-log.h"

#include "fp-image-device-private.h"
#include "fp-image-device.h"
#include "fpi-print.h"
#include "sigfm/sigfm.hpp"

#define SIGFM_ENROLL_DUPLICATE_THRESHOLD 70

/**
 * SECTION: fpi-image-device
 * @title: Internal FpImageDevice
 * @short_description: Internal image device functions
 *
 * Internal image device functions. See #FpImageDevice for public routines.
 */

/* Manually redefine what G_DEFINE_* macro does */
static inline gpointer
fp_image_device_get_instance_private (FpImageDevice *self)
{
  FpImageDeviceClass *img_class = g_type_class_peek_static (FP_TYPE_IMAGE_DEVICE);

  return G_STRUCT_MEMBER_P (self,
                            g_type_class_get_instance_private_offset (img_class));
}

static void fp_image_device_change_state (FpImageDevice      *self,
                                          FpiImageDeviceState state);

/* Private shared functions */

void
fpi_image_device_activate (FpImageDevice *self)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpImageDeviceClass *cls = FP_IMAGE_DEVICE_GET_CLASS (self);

  g_assert (!priv->active);

  fp_dbg ("Activating image device");
  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_ACTIVATING);
  cls->activate (self);
}

void
fpi_image_device_deactivate (FpImageDevice *self, gboolean cancelling)
{
  FpDevice *device = FP_DEVICE (self);
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpImageDeviceClass *cls = FP_IMAGE_DEVICE_GET_CLASS (device);

  if (!priv->active || priv->state == FPI_IMAGE_DEVICE_STATE_DEACTIVATING)
    {
      /* XXX: We currently deactivate both from minutiae scan result
       *      and finger off report. */
      fp_dbg ("Already deactivated, ignoring request.");
      return;
    }
  if (!cancelling && priv->state != FPI_IMAGE_DEVICE_STATE_IDLE)
    g_warning ("Deactivating image device while it is not idle, this should not happen.");

  if (priv->enroll_features)
    g_ptr_array_set_size (priv->enroll_features, 0);

  fp_dbg ("Deactivating image device");
  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_DEACTIVATING);
  cls->deactivate (self);
}

/* Static helper functions */

/* This should not be called directly to activate/deactivate the device! */
static void
fp_image_device_change_state (FpImageDevice *self, FpiImageDeviceState state)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  g_autofree char *prev_state_str = NULL;
  g_autofree char *state_str = NULL;
  gboolean transition_is_valid = FALSE;
  gint i;

  struct
  {
    FpiImageDeviceState from;
    FpiImageDeviceState to;
  } valid_transitions[] = {
    { FPI_IMAGE_DEVICE_STATE_INACTIVE, FPI_IMAGE_DEVICE_STATE_ACTIVATING },

    { FPI_IMAGE_DEVICE_STATE_ACTIVATING, FPI_IMAGE_DEVICE_STATE_IDLE },
    { FPI_IMAGE_DEVICE_STATE_ACTIVATING, FPI_IMAGE_DEVICE_STATE_INACTIVE },

    { FPI_IMAGE_DEVICE_STATE_IDLE, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON },
    { FPI_IMAGE_DEVICE_STATE_IDLE, FPI_IMAGE_DEVICE_STATE_CAPTURE }, /* raw mode -- currently not supported */
    { FPI_IMAGE_DEVICE_STATE_IDLE, FPI_IMAGE_DEVICE_STATE_DEACTIVATING },

    { FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON, FPI_IMAGE_DEVICE_STATE_CAPTURE },
    { FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON, FPI_IMAGE_DEVICE_STATE_DEACTIVATING }, /* cancellation */

    { FPI_IMAGE_DEVICE_STATE_CAPTURE, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF },
    { FPI_IMAGE_DEVICE_STATE_CAPTURE, FPI_IMAGE_DEVICE_STATE_IDLE }, /* raw mode -- currently not supported */
    { FPI_IMAGE_DEVICE_STATE_CAPTURE, FPI_IMAGE_DEVICE_STATE_DEACTIVATING }, /* cancellation */

    { FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF, FPI_IMAGE_DEVICE_STATE_IDLE },
    { FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF, FPI_IMAGE_DEVICE_STATE_DEACTIVATING }, /* cancellation */

    { FPI_IMAGE_DEVICE_STATE_DEACTIVATING, FPI_IMAGE_DEVICE_STATE_INACTIVE },
  };

  prev_state_str = g_enum_to_string (FPI_TYPE_IMAGE_DEVICE_STATE, priv->state);
  state_str = g_enum_to_string (FPI_TYPE_IMAGE_DEVICE_STATE, state);
  fp_dbg ("Image device internal state change from %s to %s",
          prev_state_str, state_str);

  for (i = 0; i < G_N_ELEMENTS (valid_transitions); i++)
    {
      if (valid_transitions[i].from == priv->state && valid_transitions[i].to == state)
        {
          transition_is_valid = TRUE;
          break;
        }
    }
  if (!transition_is_valid)
    g_warning ("Internal state machine issue: transition from %s to %s should not happen!",
               prev_state_str, state_str);

  priv->state = state;
  g_object_notify (G_OBJECT (self), "fpi-image-device-state");
  g_signal_emit_by_name (self, "fpi-image-device-state-changed", priv->state);

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);
    }
  else if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    {
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_NONE,
                                               FP_FINGER_STATUS_NEEDED);
    }
}

static void
fp_image_device_enroll_maybe_await_finger_on (FpImageDevice *self)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);

  /* We wait for both the minutiae scan to complete and the finger to
   * be removed before we switch to AWAIT_FINGER_ON. */
  if (priv->minutiae_scan_active || priv->finger_present)
    return;

  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON);
}

static void
fp_image_device_maybe_complete_action (FpImageDevice *self, GError *error)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpDevice *device = FP_DEVICE (self);
  FpiDeviceAction action;

  if (error)
    {
      /* Keep the first error we encountered, but not if it is of type retry */
      if (priv->action_error && !(priv->action_error->domain == FP_DEVICE_RETRY))
        {
          g_warning ("Will complete with first error, new error was: %s", error->message);
          g_clear_error (&error);
        }
      else
        {
          g_clear_error (&priv->action_error);
          priv->action_error = error;
        }
    }

  /* Do not complete if the device is still active or a minutiae scan is pending. */
  if (priv->active || priv->minutiae_scan_active)
    return;

  if (!priv->action_error)
    g_cancellable_set_error_if_cancelled (fpi_device_get_cancellable (device), &priv->action_error);

  if (priv->action_error)
    {
      fpi_device_action_error (device, g_steal_pointer (&priv->action_error));
      g_clear_object (&priv->capture_image);
      return;
    }

  /* We are done, report the result. */
  action = fpi_device_get_current_action (FP_DEVICE (self));

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      FpPrint *enroll_print;
      fpi_device_get_enroll_data (device, &enroll_print);

      fpi_device_enroll_complete (device, g_object_ref (enroll_print), NULL);
    }
  else if (action == FPI_DEVICE_ACTION_VERIFY)
    {
      fpi_device_verify_complete (device, NULL);
    }
  else if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_identify_complete (device, NULL);
    }
  else if (action == FPI_DEVICE_ACTION_CAPTURE)
    {
      fpi_device_capture_complete (device, g_steal_pointer (&priv->capture_image), NULL);
    }
  else
    {
      g_assert_not_reached ();
    }
}

static void
fpi_image_device_minutiae_detected (GObject *source_object, GAsyncResult *res, gpointer user_data)
{
  g_autoptr(FpImage) image = FP_IMAGE (source_object);
  g_autoptr(FpPrint) print = NULL;
  GError *error = NULL;
  FpImageDevice *self = FP_IMAGE_DEVICE (user_data);
  FpDevice *device = FP_DEVICE (self);
  FpImageDevicePrivate *priv;
  FpiDeviceAction action;

  /* Note: We rely on the device to not disappear during an operation. */
  priv = fp_image_device_get_instance_private (FP_IMAGE_DEVICE (device));
  priv->minutiae_scan_active = FALSE;

  if (!fp_image_detect_minutiae_finish (image, res, &error))
    {
      /* Cancel operation . */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
          fpi_image_device_deactivate (self, TRUE);
          return;
        }

      /* Replace error with a retry condition. */
      g_warning ("Failed to detect minutiae: %s", error->message);
      g_clear_pointer (&error, g_error_free);

      error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL, "Minutiae detection failed, please retry");
    }

  action = fpi_device_get_current_action (device);

  if (action == FPI_DEVICE_ACTION_CAPTURE)
    {
      priv->capture_image = g_steal_pointer (&image);
      fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
      return;
    }

  if (!error)
    {
      print = fp_print_new (device);
      fpi_print_set_type (print, FPI_PRINT_NBIS);
      if (!fpi_print_add_from_image (print, image, &error))
        {
          g_clear_object (&print);

          if (error->domain != FP_DEVICE_RETRY)
            {
              fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
              /* We might not yet be deactivating, if we are enrolling. */
              fpi_image_device_deactivate (self, TRUE);
              return;
            }
        }
    }

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      FpPrint *enroll_print;
      fpi_device_get_enroll_data (device, &enroll_print);

      if (print)
        {
          fpi_print_add_print (enroll_print, print);
          priv->enroll_stage += 1;
          g_message ("Minutiae in captured frame: %d (enroll stage %d)",
                     fp_image_get_minutiae (image) ? fp_image_get_minutiae (image)->len : 0,
                     priv->enroll_stage);
        }

      fpi_device_enroll_progress (device, priv->enroll_stage,
                                  g_steal_pointer (&print), error);

      /* Start another scan or deactivate. */
      if (priv->enroll_stage == fp_device_get_nr_enroll_stages (device))
        {
          fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
          fpi_image_device_deactivate (self, FALSE);
        }
      else
        {
          fp_image_device_enroll_maybe_await_finger_on (FP_IMAGE_DEVICE (device));
        }
    }
  else if (action == FPI_DEVICE_ACTION_VERIFY)
    {
      FpPrint *template;
      FpiMatchResult result;

      fpi_device_get_verify_data (device, &template);
      g_message ("Verify probe: extracted %d minutiae",
                 fp_image_get_minutiae (image) ? fp_image_get_minutiae (image)->len : 0);
      if (print)
        result = fpi_print_bz3_match (template, print, priv->bz3_threshold, &error);
      else
        result = FPI_MATCH_ERROR;

      if (!error || error->domain == FP_DEVICE_RETRY)
        fpi_device_verify_report (device, result, g_steal_pointer (&print), g_steal_pointer (&error));

      fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
    }
  else if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      gint i;
      GPtrArray *templates;
      FpPrint *result = NULL;

      fpi_device_get_identify_data (device, &templates);
      for (i = 0; !error && i < templates->len; i++)
        {
          FpPrint *template = g_ptr_array_index (templates, i);

          if (fpi_print_bz3_match (template, print, priv->bz3_threshold, &error) == FPI_MATCH_SUCCESS)
            {
              result = template;
              break;
            }
        }

      if (!error || error->domain == FP_DEVICE_RETRY)
        fpi_device_identify_report (device, result, g_steal_pointer (&print), g_steal_pointer (&error));

      fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
    }
  else
    {
      /* XXX: This can be hit currently due to a race condition in the enroll code!
       *      In that case we scan a further image even though the minutiae for the previous
       *      one have not yet been detected.
       *      We need to keep track on the pending minutiae detection and the fact that
       *      it will finish eventually (or we may need to retry on error and activate the
       *      device again). */
      g_assert_not_reached ();
    }
}

/*********************************************************/
/* Private API */

/**
 * fpi_image_device_set_bz3_threshold:
 * @self: a #FpImageDevice imaging fingerprint device
 * @bz3_threshold: BZ3 threshold to use
 *
 * Dynamically adjust the bz3 threshold. This is only needed for drivers
 * that support devices with different properties. It should generally be
 * called from the probe callback, but is acceptable to call from the open
 * callback.
 */
void
fpi_image_device_set_bz3_threshold (FpImageDevice *self,
                                    gint           bz3_threshold)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);

  g_return_if_fail (FP_IS_IMAGE_DEVICE (self));
  g_return_if_fail (bz3_threshold > 0);

  priv->bz3_threshold = bz3_threshold;
}

/**
 * fpi_image_device_report_finger_status:
 * @self: a #FpImageDevice imaging fingerprint device
 * @present: whether the finger is present on the sensor
 *
 * Reports from the driver whether the user's finger is on
 * the sensor.
 */
void
fpi_image_device_report_finger_status (FpImageDevice *self,
                                       gboolean       present)
{
  FpDevice *device = FP_DEVICE (self);
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpiDeviceAction action;

  if (present)
    {
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);
    }
  else
    {
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NONE,
                                               FP_FINGER_STATUS_PRESENT);
    }

  if (priv->state == FPI_IMAGE_DEVICE_STATE_INACTIVE)
    {
      /* Do we really want to always ignore such reports? We could
       * also track the state in case the user had the finger on
       * the device at initialisation time and the driver reports
       * this early.
       */
      g_debug ("Ignoring finger presence report as the device is not active!");
      return;
    }

  action = fpi_device_get_current_action (device);

  g_assert (action != FPI_DEVICE_ACTION_OPEN);
  g_assert (action != FPI_DEVICE_ACTION_CLOSE);

  g_debug ("Image device reported finger status: %s", present ? "on" : "off");

  priv->finger_present = present;

  if (present && priv->state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_CAPTURE);
    }
  else if (!present && priv->state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
    {
      /* If we are in the non-enroll case, we always deactivate.
       *
       * In the enroll case, the decision can only be made after minutiae
       * detection has finished.
       */
      fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_IDLE);

      if (action != FPI_DEVICE_ACTION_ENROLL || priv->enroll_stage == fp_device_get_nr_enroll_stages (device))
        fpi_image_device_deactivate (self, FALSE);
      else
        fp_image_device_enroll_maybe_await_finger_on (self);
    }
}

/* Reject only a strong SIGFM match against an accepted enrollment stage. The
 * threshold matches the single-stage score sufficient for high-confidence
 * verification, so ordinary partial overlap remains acceptable.
 */
static gboolean
sigfm_enroll_stage_is_duplicate (SigfmImgInfo *info,
                                 GPtrArray *enroll_features,
                                 guint *matched_stage,
                                 gint *matched_score)
{
  for (guint i = 0; i < enroll_features->len; i++)
    {
      GBytes *stage = g_ptr_array_index (enroll_features, i);
      gsize stage_len = 0;
      const guint8 *stage_data = g_bytes_get_data (stage, &stage_len);
      SigfmImgInfo *previous = sigfm_deserialize_binary (
        stage_data, stage_len <= G_MAXINT ? (gint) stage_len : 0);

      if (!previous)
        {
          g_warning ("SIGFM enroll: could not decode stored stage %u for duplicate check",
                     i + 1);
          continue;
        }

      gint score = sigfm_match_score (info, previous);
      sigfm_free_info (previous);

      if (score < 0)
        {
          g_warning ("SIGFM enroll: comparison with stage %u failed",
                     i + 1);
          continue;
        }

      g_message ("SIGFM enroll: candidate similarity to stage %u: score=%d",
                 i + 1, score);
      if (score >= SIGFM_ENROLL_DUPLICATE_THRESHOLD)
        {
          *matched_stage = i + 1;
          *matched_score = score;
          return TRUE;
        }
    }

  return FALSE;
}

static void
fpi_image_device_sigfm_process (FpImageDevice *self, FpImage *image)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpDevice *device = FP_DEVICE (self);
  FpiDeviceAction action = fpi_device_get_current_action (device);
  GError *error = NULL;
  gsize img_len = 0;
  const guchar *img_data = fp_image_get_data (image, &img_len);
  gint width = fp_image_get_width (image);
  gint height = fp_image_get_height (image);

  priv->minutiae_scan_active = FALSE;

  if (action == FPI_DEVICE_ACTION_CAPTURE)
    {
      priv->capture_image = g_object_ref (image);
      fp_image_device_maybe_complete_action (self, NULL);
      return;
    }

  SigfmImgInfo *info = sigfm_extract (img_data, width, height);
  int kpts = sigfm_keypoints_count (info);
  g_message ("SIGFM: extracted %d keypoints from %dx%d image", kpts, width, height);

  if (kpts < 6)
    {
      g_warning ("SIGFM: too few keypoints (%d < 6), requesting retry", kpts);
      if (info)
        sigfm_free_info (info);
      info = NULL;
      error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_CENTER_FINGER, "Please center finger on sensor");
    }

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      FpPrint *enroll_print = NULL;
      fpi_device_get_enroll_data (device, &enroll_print);

      if (!priv->enroll_features)
        priv->enroll_features = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

      if (!error && info && priv->enroll_features->len > 0)
        {
          guint matched_stage = 0;
          gint matched_score = 0;

          if (sigfm_enroll_stage_is_duplicate (info, priv->enroll_features,
                                               &matched_stage, &matched_score))
            {
              g_warning ("SIGFM enroll: rejecting near-duplicate press (stage %u, score=%d); requesting a new placement",
                         matched_stage, matched_score);
              error = fpi_device_retry_new_msg (
                FP_DEVICE_RETRY_GENERAL,
                "This press is too similar to an earlier one. Move your finger to cover a different area.");
            }
        }

      if (!error && info)
        {
          int out_len = 0;
          unsigned char *sdata = sigfm_serialize_binary (info, &out_len);
          if (sdata && out_len > 0)
            {
              g_ptr_array_add (priv->enroll_features, g_bytes_new_take (sdata, (gsize) out_len));
              priv->enroll_stage += 1;
              g_message ("SIGFM enroll: completed stage %d of %d (kpts=%d, bytes=%d)",
                         priv->enroll_stage, fp_device_get_nr_enroll_stages (device), kpts, out_len);
            }
          else
            {
              error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL, "Failed to serialize features");
            }
        }

      if (info)
        sigfm_free_info (info);

      g_autoptr(FpPrint) stage_print = fp_print_new (device);
      fpi_print_set_type (stage_print, FPI_PRINT_RAW);

      fpi_device_enroll_progress (device, priv->enroll_stage,
                                  error ? NULL : g_steal_pointer (&stage_print),
                                  error);

      if (priv->enroll_stage == fp_device_get_nr_enroll_stages (device))
        {
          GVariantBuilder builder;
          g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));
          for (guint i = 0; i < priv->enroll_features->len; i++)
            {
              GBytes *b = g_ptr_array_index (priv->enroll_features, i);
              g_variant_builder_add_value (&builder,
                                           g_variant_new_from_bytes (G_VARIANT_TYPE ("ay"), b, TRUE));
            }
          GVariant *var = g_variant_builder_end (&builder);
          fpi_print_set_raw_data (enroll_print, var);
          fpi_print_set_type (enroll_print, FPI_PRINT_RAW);
          g_ptr_array_set_size (priv->enroll_features, 0);

          g_message ("SIGFM enroll: completed successfully with %u stages stored",
                     fp_device_get_nr_enroll_stages (device));
          fp_image_device_maybe_complete_action (self, NULL);
          if (!priv->finger_present)
            fpi_image_device_deactivate (self, FALSE);
        }
      else
        {
          fp_image_device_enroll_maybe_await_finger_on (self);
        }
    }
  else if (action == FPI_DEVICE_ACTION_VERIFY)
    {
      FpPrint *template = NULL;
      FpiMatchResult result = FPI_MATCH_FAIL;
      fpi_device_get_verify_data (device, &template);

      if (!error && info)
        {
          GVariant *template_var = fpi_print_get_raw_data (template);
          if (!template_var)
            {
              g_warning ("SIGFM verify: template has no raw data!");
              result = FPI_MATCH_ERROR;
            }
          else
            {
              int max_score = 0;
              int matched_stages = 0;
              GVariantIter iter;
              GVariant *stage_bytes;

              g_variant_iter_init (&iter, template_var);
              while ((stage_bytes = g_variant_iter_next_value (&iter)) != NULL)
                {
                  gsize sz;
                  const uint8_t *b = (const uint8_t *) g_variant_get_fixed_array (stage_bytes, &sz, 1);
                  if (b && sz > 0)
                    {
                      SigfmImgInfo *gallery_info = sigfm_deserialize_binary (b, (int) sz);
                      if (gallery_info)
                        {
                          int score = sigfm_match_score (info, gallery_info);
                          if (score > max_score)
                            max_score = score;
                          if (score >= 40)
                            matched_stages++;
                          sigfm_free_info (gallery_info);
                        }
                    }
                  g_variant_unref (stage_bytes);
                }

              if (max_score >= 70 || (max_score >= 45 && matched_stages >= 2))
                result = FPI_MATCH_SUCCESS;
              else
                result = FPI_MATCH_FAIL;

              g_message ("SIGFM verify: max_score = %d, matched_stages = %d -> %s",
                         max_score, matched_stages, result == FPI_MATCH_SUCCESS ? "MATCH" : "NO_MATCH");
            }
        }
      else
        {
          result = FPI_MATCH_ERROR;
        }

      if (info)
        sigfm_free_info (info);

      g_autoptr(FpPrint) probe_print = fp_print_new (device);
      fpi_print_set_type (probe_print, FPI_PRINT_RAW);

      if (!error || error->domain == FP_DEVICE_RETRY)
        fpi_device_verify_report (device, result, g_steal_pointer (&probe_print), g_steal_pointer (&error));

      fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
      if (error)
        fpi_image_device_deactivate (self, TRUE);
    }
  else if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      GPtrArray *templates = NULL;
      FpPrint *matched_template = NULL;
      fpi_device_get_identify_data (device, &templates);

      if (!error && info && templates)
        {
          for (guint t = 0; t < templates->len; t++)
            {
              FpPrint *tmpl = g_ptr_array_index (templates, t);
              GVariant *template_var = fpi_print_get_raw_data (tmpl);
              if (!template_var)
                continue;

              int max_score = 0;
              int matched_stages = 0;
              GVariantIter iter;
              GVariant *stage_bytes;

              g_variant_iter_init (&iter, template_var);
              while ((stage_bytes = g_variant_iter_next_value (&iter)) != NULL)
                {
                  gsize sz;
                  const uint8_t *b = (const uint8_t *) g_variant_get_fixed_array (stage_bytes, &sz, 1);
                  if (b && sz > 0)
                    {
                      SigfmImgInfo *gallery_info = sigfm_deserialize_binary (b, (int) sz);
                      if (gallery_info)
                        {
                          int score = sigfm_match_score (info, gallery_info);
                          if (score > max_score)
                            max_score = score;
                          if (score >= 40)
                            matched_stages++;
                          sigfm_free_info (gallery_info);
                        }
                    }
                  g_variant_unref (stage_bytes);
                }

              if (max_score >= 70 || (max_score >= 45 && matched_stages >= 2))
                {
                  matched_template = tmpl;
                  break;
                }
            }
        }

      if (info)
        sigfm_free_info (info);

      g_autoptr(FpPrint) probe_print = fp_print_new (device);
      fpi_print_set_type (probe_print, FPI_PRINT_RAW);

      if (!error || error->domain == FP_DEVICE_RETRY)
        fpi_device_identify_report (device, matched_template, g_steal_pointer (&probe_print), g_steal_pointer (&error));

      fp_image_device_maybe_complete_action (self, g_steal_pointer (&error));
      if (error)
        fpi_image_device_deactivate (self, TRUE);
    }
}

/**
 * fpi_image_device_image_captured:
 * @self: a #FpImageDevice imaging fingerprint device
 * @image: whether the finger is present on the sensor
 *
 * Reports an image capture. Only use this function if the image was
 * captured successfully. If there was an issue where the user should
 * retry, use fpi_image_device_retry_scan() to report the retry condition.
 *
 * In the event of a fatal error for the operation use
 * fpi_image_device_session_error(). This will abort the entire operation
 * including e.g. an enroll operation which captures multiple images during
 * one session.
 */
void
fpi_image_device_image_captured (FpImageDevice *self, FpImage *image)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpiDeviceAction action;

  action = fpi_device_get_current_action (FP_DEVICE (self));

  g_return_if_fail (image != NULL);
  g_return_if_fail (priv->state == FPI_IMAGE_DEVICE_STATE_CAPTURE);
  g_return_if_fail (action == FPI_DEVICE_ACTION_ENROLL ||
                    action == FPI_DEVICE_ACTION_VERIFY ||
                    action == FPI_DEVICE_ACTION_IDENTIFY ||
                    action == FPI_DEVICE_ACTION_CAPTURE);

  g_debug ("Image device captured an image");

  priv->minutiae_scan_active = TRUE;

  if (FP_IMAGE_DEVICE_GET_CLASS (self)->use_sigfm)
    {
      fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF);
      fpi_image_device_sigfm_process (self, image);
      return;
    }

  /* XXX: We also detect minutiae in capture mode, we solely do this
   *      to normalize the image which will happen as a by-product. */
  fp_image_detect_minutiae (image,
                            fpi_device_get_cancellable (FP_DEVICE (self)),
                            fpi_image_device_minutiae_detected,
                            self);

  /* XXX: This is wrong if we add support for raw capture mode. */
  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF);
}

/**
 * fpi_image_device_retry_scan:
 * @self: a #FpImageDevice imaging fingerprint device
 * @retry: The #FpDeviceRetry error code to report
 *
 * Reports a scan failure to the user. This may or may not abort the
 * current session. It is the equivalent of fpi_image_device_image_captured()
 * in the case of a retryable error condition (e.g. short swipe).
 */
void
fpi_image_device_retry_scan (FpImageDevice *self, FpDeviceRetry retry)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpiDeviceAction action;
  GError *error;

  action = fpi_device_get_current_action (FP_DEVICE (self));

  /* We might be waiting for a finger at this point, so just accept
   * all but INACTIVE */
  g_return_if_fail (priv->state != FPI_IMAGE_DEVICE_STATE_INACTIVE);
  g_return_if_fail (action == FPI_DEVICE_ACTION_ENROLL ||
                    action == FPI_DEVICE_ACTION_VERIFY ||
                    action == FPI_DEVICE_ACTION_IDENTIFY ||
                    action == FPI_DEVICE_ACTION_CAPTURE);

  error = fpi_device_retry_new (retry);

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      g_debug ("Reporting retry during enroll");
      fpi_device_enroll_progress (FP_DEVICE (self), priv->enroll_stage, NULL, error);

      fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF);
    }
  else if (action == FPI_DEVICE_ACTION_VERIFY)
    {
      fpi_device_verify_report (FP_DEVICE (self), FPI_MATCH_ERROR, NULL, error);
      fp_image_device_maybe_complete_action (self, NULL);
      fpi_image_device_deactivate (self, TRUE);

    }
  else if (action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_identify_report (FP_DEVICE (self), NULL, NULL, error);
      fp_image_device_maybe_complete_action (self, NULL);
      fpi_image_device_deactivate (self, TRUE);
    }
  else
    {
      /* The capture case where there is no early reporting. */
      g_debug ("Abort current operation due to retry (no early-reporting)");
      fp_image_device_maybe_complete_action (self, error);
      fpi_image_device_deactivate (self, TRUE);
    }
}

/**
 * fpi_image_device_session_error:
 * @self: a #FpImageDevice imaging fingerprint device
 * @error: The #GError to report
 *
 * Report an error while interacting with the device. This effectively
 * aborts the current ongoing action. Note that doing so will result in
 * the deactivation handler to be called and this function must not be
 * used to report an error during deactivation.
 */
void
fpi_image_device_session_error (FpImageDevice *self, GError *error)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);

  g_return_if_fail (self);

  if (!error)
    {
      g_warning ("Driver did not provide an error, generating a generic one");
      error = g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_GENERAL, "Driver reported session error without an error");
    }

  if (!priv->active)
    {
      FpiDeviceAction action = fpi_device_get_current_action (FP_DEVICE (self));
      g_warning ("Driver reported session error, but device is inactive.");

      if (action != FPI_DEVICE_ACTION_NONE)
        {
          g_warning ("Translating to activation failure!");
          fpi_image_device_activate_complete (self, error);
          return;
        }
    }
  else if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
           fpi_device_action_is_cancelled (FP_DEVICE (self)))
    {
      /* Ignore cancellation errors here, as we will explicitly deactivate
       * anyway (or, may already have done so at this point).
       */
      g_debug ("Driver reported a cancellation error, this is expected but not required. Ignoring.");
      g_clear_error (&error);
      return;
    }
  else if (priv->state == FPI_IMAGE_DEVICE_STATE_INACTIVE)
    {
      g_warning ("Driver reported session error while deactivating already, ignoring. This indicates a driver bug.");
      g_clear_error (&error);
      return;
    }

  if (error->domain == FP_DEVICE_RETRY)
    g_warning ("Driver should report retries using fpi_image_device_retry_scan!");

  fp_image_device_maybe_complete_action (self, error);
  fpi_image_device_deactivate (self, TRUE);
}

/**
 * fpi_image_device_activate_complete:
 * @self: a #FpImageDevice imaging fingerprint device
 * @error: A #GError or %NULL on success
 *
 * Reports completion of device activation.
 */
void
fpi_image_device_activate_complete (FpImageDevice *self, GError *error)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpiDeviceAction action;

  action = fpi_device_get_current_action (FP_DEVICE (self));

  g_return_if_fail (priv->active == FALSE);
  g_return_if_fail (priv->state == FPI_IMAGE_DEVICE_STATE_ACTIVATING);
  g_return_if_fail (action == FPI_DEVICE_ACTION_ENROLL ||
                    action == FPI_DEVICE_ACTION_VERIFY ||
                    action == FPI_DEVICE_ACTION_IDENTIFY ||
                    action == FPI_DEVICE_ACTION_CAPTURE);

  if (error)
    {
      g_debug ("Image device activation failed");
      fpi_device_action_error (FP_DEVICE (self), error);
      return;
    }

  g_debug ("Image device activation completed");

  priv->active = TRUE;

  /* We always want to capture at this point, move to AWAIT_FINGER
   * state. */
  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_IDLE);
  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON);
}

/**
 * fpi_image_device_deactivate_complete:
 * @self: a #FpImageDevice imaging fingerprint device
 * @error: A #GError or %NULL on success
 *
 * Reports completion of device deactivation.
 */
void
fpi_image_device_deactivate_complete (FpImageDevice *self, GError *error)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);

  g_return_if_fail (priv->active == TRUE);
  g_return_if_fail (priv->state == FPI_IMAGE_DEVICE_STATE_DEACTIVATING);

  g_debug ("Image device deactivation completed");

  priv->active = FALSE;

  /* Assume finger was removed. */
  priv->finger_present = FALSE;

  fp_image_device_change_state (self, FPI_IMAGE_DEVICE_STATE_INACTIVE);

  fp_image_device_maybe_complete_action (self, error);
}

/**
 * fpi_image_device_open_complete:
 * @self: a #FpImageDevice imaging fingerprint device
 * @error: A #GError or %NULL on success
 *
 * Reports completion of open operation.
 */
void
fpi_image_device_open_complete (FpImageDevice *self, GError *error)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpiDeviceAction action;

  action = fpi_device_get_current_action (FP_DEVICE (self));

  g_return_if_fail (priv->active == FALSE);
  g_return_if_fail (action == FPI_DEVICE_ACTION_OPEN);

  g_debug ("Image device open completed");

  priv->state = FPI_IMAGE_DEVICE_STATE_INACTIVE;
  g_object_notify (G_OBJECT (self), "fpi-image-device-state");

  fpi_device_report_finger_status (FP_DEVICE (self), FP_FINGER_STATUS_NONE);

  fpi_device_open_complete (FP_DEVICE (self), error);
}

/**
 * fpi_image_device_close_complete:
 * @self: a #FpImageDevice imaging fingerprint device
 * @error: A #GError or %NULL on success
 *
 * Reports completion of close operation.
 */
void
fpi_image_device_close_complete (FpImageDevice *self, GError *error)
{
  FpImageDevicePrivate *priv = fp_image_device_get_instance_private (self);
  FpiDeviceAction action;

  action = fpi_device_get_current_action (FP_DEVICE (self));

  g_debug ("Image device close completed");

  g_return_if_fail (priv->active == FALSE);
  g_return_if_fail (action == FPI_DEVICE_ACTION_CLOSE);

  priv->state = FPI_IMAGE_DEVICE_STATE_INACTIVE;
  g_object_notify (G_OBJECT (self), "fpi-image-device-state");

  fpi_device_close_complete (FP_DEVICE (self), error);
}
