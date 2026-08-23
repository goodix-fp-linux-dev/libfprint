/*
 * Goodix 27c6:533c native driver for libfprint — Verify/identify flow
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
 * Verify/identify SSM shape and the queued-report pattern ported
 * near-verbatim from goodix53x5-auth.c (sibling driver, same SIGFM
 * approach). Two deliberate deviations from that reference, both because
 * this driver has no equivalent of goodix53x5's EC-power-controlled
 * "deactivate" primitive (a bounded sleep+EC-off cleanup that skips waiting
 * for lift-off on a successful match):
 *
 *  - GOODIX_VERIFY_FINISH there branches between waiting for finger-up and
 *    a cheap deactivate; here FINISH always waits for finger-up
 *    (goodix533c_start_finger_up_subsm()), matching what the single-capture
 *    test harness already does unconditionally on real hardware. Inventing
 *    a "skip cleanup on success" shortcut for a command sequence never
 *    exercised that way would be an unverified behavioral change to
 *    already-proven hardware interaction; a successful verify simply takes
 *    a little longer (until the user's own finger lift, which they were
 *    going to do anyway).
 *  - There is no REINIT/REINIT_DONE pair -- this driver has no suspend()/
 *    resume() story yet, so there is nothing to reinitialize before an
 *    action.
 */

#define FP_COMPONENT "goodix533c"

#include "drivers_api.h"
#include "goodix533c-private.h"
#include "goodix533c-match.h"
#include "goodix533c-auth.h"

#include <string.h>

static gboolean
goodix533c_match_scores_need_exhaustive_logging (void)
{
#if GLIB_VERSION_MAX_ALLOWED >= GLIB_VERSION_2_68
  return !g_log_writer_default_would_drop (G_LOG_LEVEL_DEBUG, G_LOG_DOMAIN);
#else
  /* g_log_writer_default_would_drop() is 2.68+; this project pins
   * GLIB_VERSION_MAX_ALLOWED to its declared floor of 2.56 (see
   * glib_min_version in meson.build), regardless of the glib actually
   * installed on the build machine -- so gate on that macro, not
   * GLIB_CHECK_VERSION (which reflects the build machine's headers and
   * would silently produce a binary that needs a newer runtime glib than
   * the project claims to support). Below 2.68 there is no cheap way to
   * ask in advance whether debug logging would be dropped, so just
   * always do the exhaustive per-candidate logging. */
  return TRUE;
#endif
}

static gboolean
goodix533c_gallery_has_single_username (GPtrArray *gallery)
{
  const gchar *username;

  if (gallery->len == 0)
    return FALSE;

  username = fp_print_get_username (g_ptr_array_index (gallery, 0));
  if (username == NULL || username[0] == '\0')
    return FALSE;

  for (guint i = 1; i < gallery->len; i++)
    {
      FpPrint *print = g_ptr_array_index (gallery, i);

      if (g_strcmp0 (username, fp_print_get_username (print)) != 0)
        return FALSE;
    }

  return TRUE;
}

typedef enum {
  GOODIX533C_VERIFY_CAPTURE_REF = 0,
  GOODIX533C_VERIFY_WAIT_FINGER,
  GOODIX533C_VERIFY_CAPTURE,
  GOODIX533C_VERIFY_MATCH,
  GOODIX533C_VERIFY_FINISH,
  GOODIX533C_VERIFY_NUM_STATES,
} Goodix533cVerifyState;

void
goodix533c_clear_pending_result_report (FpiDeviceGoodix533c *self)
{
  self->pending_result_report = FALSE;
  self->pending_result_action = 0;
  self->pending_verify_result = 0;
  g_clear_object (&self->pending_identify_match);
  g_clear_error (&self->pending_result_error);
  g_clear_error (&self->pending_action_error);
}

static void
goodix533c_queue_action_error (FpiDeviceGoodix533c *self,
                               GError              *error)
{
  goodix533c_clear_pending_result_report (self);

  self->pending_action_error = error;
}

static void
goodix533c_queue_verify_report (FpiDeviceGoodix533c *self,
                                FpiMatchResult       result,
                                GError              *error)
{
  goodix533c_clear_pending_result_report (self);

  self->pending_result_report = TRUE;
  self->pending_result_action = FPI_DEVICE_ACTION_VERIFY;
  self->pending_verify_result = result;
  self->pending_result_error = error;
}

static void
goodix533c_queue_identify_report (FpiDeviceGoodix533c *self,
                                  FpPrint             *match,
                                  GError              *error)
{
  goodix533c_clear_pending_result_report (self);

  self->pending_result_report = TRUE;
  self->pending_result_action = FPI_DEVICE_ACTION_IDENTIFY;
  if (match != NULL)
    self->pending_identify_match = g_object_ref (match);
  self->pending_result_error = error;
}

static void
goodix533c_flush_pending_result_report (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  if (!self->pending_result_report)
    return;

  if (self->pending_result_action == FPI_DEVICE_ACTION_IDENTIFY)
    {
      g_autoptr(FpPrint) match = g_steal_pointer (&self->pending_identify_match);

      fpi_device_identify_report (dev, match, NULL,
                                  g_steal_pointer (&self->pending_result_error));
    }
  else
    {
      fpi_device_verify_report (dev, self->pending_verify_result, NULL,
                                g_steal_pointer (&self->pending_result_error));
    }

  self->pending_result_report = FALSE;
  self->pending_result_action = 0;
  self->pending_verify_result = 0;
}

static void
goodix533c_verify_ssm_handler (FpiSsm   *ssm,
                               FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case GOODIX533C_VERIFY_CAPTURE_REF:
      goodix533c_start_ref_capture_subsm (ssm, dev);
      break;

    case GOODIX533C_VERIFY_WAIT_FINGER:
      goodix533c_start_finger_wait_subsm (ssm, dev, NULL, NULL);
      break;

    case GOODIX533C_VERIFY_CAPTURE:
      goodix533c_start_live_capture_subsm (ssm, dev);
      break;

    case GOODIX533C_VERIFY_MATCH:
      {
        FpiDeviceAction action = fpi_device_get_current_action (dev);
        GoodixMatchInfo *probe_info;
        int keypoints;

        /* Extract SIFT features once for both identify and verify paths. */
        probe_info = goodix533c_match_extract (self->captured_image);
        keypoints = goodix533c_match_keypoints_count (probe_info);

        if (keypoints < GOODIX533C_MIN_CAPTURE_KEYPOINTS)
          {
            if (action == FPI_DEVICE_ACTION_IDENTIFY)
              {
                goodix533c_queue_identify_report (self, NULL,
                                                  fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
              }
            else
              {
                goodix533c_queue_verify_report (self, FPI_MATCH_ERROR,
                                                fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
              }

            self->verify_wait_finger_up = TRUE;
            goodix533c_match_free_info (probe_info);
            g_clear_pointer (&self->captured_image, g_free);
            fpi_ssm_next_state (ssm);
            return;
          }

        if (action == FPI_DEVICE_ACTION_IDENTIFY)
          {
            /* Identify: match against gallery of enrolled prints. */
            GPtrArray *gallery = NULL;
            FpPrint *match = NULL;
            int best_score = 0;
            int best_match_score = 0;
            int valid_templates = 0;
            gboolean saw_unusable_template = FALSE;
            gboolean stop_after_match;

            fpi_device_get_identify_data (dev, &gallery);
            stop_after_match =
              !goodix533c_match_scores_need_exhaustive_logging () &&
              goodix533c_gallery_has_single_username (gallery);

            for (guint i = 0; i < gallery->len; i++)
              {
                FpPrint *tmpl = g_ptr_array_index (gallery, i);
                GVariant *tmpl_data = NULL;
                GVariantIter iter;
                GVariant *child;
                int sample_idx = 0;
                int tmpl_best_score = 0;

                g_object_get (G_OBJECT (tmpl), "fpi-data", &tmpl_data, NULL);
                if (tmpl_data == NULL)
                  continue;

                g_variant_iter_init (&iter, tmpl_data);
                while ((child = g_variant_iter_next_value (&iter)))
                  {
                    gsize len;
                    const guint8 *feature;

                    feature = g_variant_get_fixed_array (child, &len, 1);
                    if (len > 0)
                      {
                        int score;
                        Goodix533cSigfmTemplateStatus template_status;

                        template_status = goodix533c_match_serialized_feature (probe_info,
                                                                               feature,
                                                                               len,
                                                                               &score);
                        if (template_status != GOODIX533C_SIGFM_TEMPLATE_OK)
                          {
                            saw_unusable_template = TRUE;

                            fp_dbg ("identify: gallery[%u] sample %d invalid SIGFM template",
                                    i, sample_idx);
                            sample_idx++;
                            g_variant_unref (child);
                            continue;
                          }

                        valid_templates++;
                        fp_dbg ("identify: gallery[%u] sample %d sigfm_score %d",
                                i, sample_idx, score);

                        if (score > tmpl_best_score)
                          tmpl_best_score = score;

                        sample_idx++;

                        if (stop_after_match &&
                            tmpl_best_score >= GOODIX533C_SIGFM_BEST_MIN)
                          {
                            g_variant_unref (child);
                            break;
                          }
                      }
                    g_variant_unref (child);
                  }
                g_variant_unref (tmpl_data);

                if (tmpl_best_score > best_score)
                  best_score = tmpl_best_score;

                if (tmpl_best_score >= GOODIX533C_SIGFM_BEST_MIN &&
                    tmpl_best_score > best_match_score)
                  {
                    best_match_score = tmpl_best_score;
                    match = tmpl;
                  }

                if (stop_after_match && match != NULL)
                  break;
              }

            fp_dbg ("Identify best SIGFM score: %d (best_min: %d)",
                    best_score, GOODIX533C_SIGFM_BEST_MIN);

            if (valid_templates == 0 && saw_unusable_template)
              {
                goodix533c_queue_action_error (self,
                                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
                self->verify_wait_finger_up = FALSE;
              }
            else if (match != NULL)
              {
                goodix533c_queue_identify_report (self, match, NULL);
                self->verify_wait_finger_up = FALSE;
              }
            else
              {
                goodix533c_queue_identify_report (self, NULL, NULL);
                self->verify_wait_finger_up = TRUE;
              }
          }
        else
          {
            /* Verify: match against single enrolled print. */
            FpPrint *print = NULL;
            GVariant *data = NULL;
            int best_score = 0;
            int sample_idx = 0;
            int valid_templates = 0;
            gboolean saw_unusable_template = FALSE;
            gboolean score_all_templates =
              goodix533c_match_scores_need_exhaustive_logging ();

            fpi_device_get_verify_data (dev, &print);
            g_object_get (G_OBJECT (print), "fpi-data", &data, NULL);

            if (data != NULL)
              {
                GVariantIter iter;
                GVariant *child;

                g_variant_iter_init (&iter, data);
                while ((child = g_variant_iter_next_value (&iter)))
                  {
                    gsize len;
                    const guint8 *feature;

                    feature = g_variant_get_fixed_array (child, &len, 1);
                    if (len > 0)
                      {
                        int score;
                        Goodix533cSigfmTemplateStatus template_status;

                        template_status = goodix533c_match_serialized_feature (probe_info,
                                                                               feature,
                                                                               len,
                                                                               &score);
                        if (template_status != GOODIX533C_SIGFM_TEMPLATE_OK)
                          {
                            saw_unusable_template = TRUE;

                            fp_dbg ("verify: sample %d invalid SIGFM template",
                                    sample_idx);
                            sample_idx++;
                            g_variant_unref (child);
                            continue;
                          }

                        valid_templates++;
                        fp_dbg ("verify: sample %d sigfm_score %d",
                                sample_idx, score);

                        if (score > best_score)
                          best_score = score;

                        sample_idx++;

                        if (!score_all_templates &&
                            best_score >= GOODIX533C_SIGFM_BEST_MIN)
                          {
                            g_variant_unref (child);
                            break;
                          }
                      }
                    g_variant_unref (child);
                  }
                g_variant_unref (data);
              }

            fp_dbg ("Verify best SIGFM score: %d (best_min: %d)",
                    best_score, GOODIX533C_SIGFM_BEST_MIN);

            if (valid_templates == 0 && saw_unusable_template)
              {
                goodix533c_queue_action_error (self,
                                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
                self->verify_wait_finger_up = FALSE;
              }
            else if (best_score >= GOODIX533C_SIGFM_BEST_MIN)
              {
                goodix533c_queue_verify_report (self, FPI_MATCH_SUCCESS, NULL);
                self->verify_wait_finger_up = FALSE;
              }
            else
              {
                goodix533c_queue_verify_report (self, FPI_MATCH_FAIL, NULL);
                self->verify_wait_finger_up = TRUE;
              }
          }

        goodix533c_match_free_info (probe_info);
        g_clear_pointer (&self->captured_image, g_free);

        if (self->verify_wait_finger_up)
          goodix533c_flush_pending_result_report (dev);

        fpi_ssm_next_state (ssm);
      }
      break;

    case GOODIX533C_VERIFY_FINISH:
      /* Always wait for lift-off here -- see the file comment for why this
       * driver has no cheap "deactivate without waiting" alternative for
       * the success path. */
      goodix533c_start_finger_up_subsm (ssm, dev);
      break;
    }
}

static void
goodix533c_verify_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);

  self->task_ssm = NULL;
  g_clear_pointer (&self->reference_pixels, g_free);
  self->have_reference = FALSE;
  g_clear_pointer (&self->captured_image, g_free);

  if (error)
    {
      /* If cleanup fails after matching, the auth result still matters more
       * than post-result hardware cleanup. */
      gint failed_state = fpi_ssm_get_cur_state (ssm);

      if (failed_state >= GOODIX533C_VERIFY_FINISH &&
          !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fp_warn ("Post-match cleanup error (non-fatal): %s",
                   error->message);
          g_clear_error (&error);
        }
    }

  if (error == NULL)
    {
      if (self->pending_action_error != NULL)
        error = g_steal_pointer (&self->pending_action_error);
      else
        goodix533c_flush_pending_result_report (dev);
    }
  else
    goodix533c_clear_pending_result_report (self);

  self->verify_wait_finger_up = FALSE;

  if (action == FPI_DEVICE_ACTION_IDENTIFY)
    fpi_device_identify_complete (dev, error);
  else
    fpi_device_verify_complete (dev, error);
}

void
goodix533c_auth_start (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  FpiSsm *ssm;

  goodix533c_clear_pending_result_report (self);
  self->verify_wait_finger_up = FALSE;
  g_clear_pointer (&self->reference_pixels, g_free);
  self->have_reference = FALSE;
  g_clear_pointer (&self->captured_image, g_free);

  ssm = fpi_ssm_new (dev, goodix533c_verify_ssm_handler,
                     GOODIX533C_VERIFY_NUM_STATES);
  self->task_ssm = ssm;
  fpi_ssm_start (ssm, goodix533c_verify_ssm_done);
}
