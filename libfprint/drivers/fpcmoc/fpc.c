/*
 * Copyright (c) 2022 Fingerprint Cards AB <tech@fingerprints.com>
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

#include "drivers_api.h"
#include "fpc.h"

#define FP_COMPONENT "fpcmoc"
#define MAX_ENROLL_SAMPLES (25)
#define CTRL_TIMEOUT (1000)
#define DATA_TIMEOUT (5000)

/* Usb port setting */
#define EP_IN (1 | FPI_USB_ENDPOINT_IN)
#define EP_IN_MAX_BUF_SIZE (2048)

struct _FpiDeviceFpcMoc
{
  FpDevice      parent;
  FpiSsm       *task_ssm;
  FpiSsm       *cmd_ssm;
  gboolean      cmd_suspended;
  gint          enroll_stage;
  gint          immobile_stage;
  gint          max_enroll_stage;
  gint          max_immobile_stage;
  gint          max_stored_prints;
  guint         cmd_data_timeout;
  guint8       *dbid;
  gboolean      do_cleanup;
  GCancellable *interrupt_cancellable;
};

G_DEFINE_TYPE (FpiDeviceFpcMoc, fpi_device_fpcmoc, FP_TYPE_DEVICE);

typedef void (*SynCmdMsgCallback) (FpiDeviceFpcMoc *self,
                                   void            *resp,
                                   GError          *error);

typedef struct
{
  FpcCmdType        cmdtype;
  guint8            request;
  guint16           value;
  guint16           index;
  guint8           *data;
  gsize             data_len;
  SynCmdMsgCallback callback;
} CommandData;

static const FpIdEntry id_table[] = {
  { .vid = 0x10A5,  .pid = 0xFFE0,  },
  { .vid = 0x10A5,  .pid = 0xA305,  },
  { .vid = 0x10A5,  .pid = 0xDA04,  },
  { .vid = 0x10A5,  .pid = 0xD805,  },
  { .vid = 0x10A5,  .pid = 0xD205,  },
  /* terminating entry */
  { .vid = 0,  .pid = 0,  .driver_data = 0 },
};

static void
fpc_suspend_resume_cb (FpiUsbTransfer *transfer,
                       FpDevice       *device,
                       gpointer        user_data,
                       GError         *error)
{
  int ssm_state = fpi_ssm_get_cur_state (transfer->ssm);

  fp_dbg ("%s current ssm state: %d", G_STRFUNC, ssm_state);

  if (ssm_state == FP_CMD_SUSPENDED)
    {
      if (error)
        fpi_ssm_mark_failed (transfer->ssm, error);

      fpi_device_suspend_complete (device, error);
      /* The resume handler continues to the next state! */
    }
  else if (ssm_state == FP_CMD_RESUME)
    {
      if (error)
        fpi_ssm_mark_failed (transfer->ssm, error);
      else
        fpi_ssm_jump_to_state (transfer->ssm, FP_CMD_GET_DATA);

      fpi_device_resume_complete (device, error);
    }
}

static void
fpc_cmd_receive_cb (FpiUsbTransfer *transfer,
                    FpDevice       *device,
                    gpointer        user_data,
                    GError         *error)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  CommandData *data = user_data;
  int ssm_state = 0;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && (self->cmd_suspended))
    {
      g_error_free (error);
      fpi_ssm_jump_to_state (transfer->ssm, FP_CMD_SUSPENDED);
      return;
    }

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (data == NULL)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  ssm_state = fpi_ssm_get_cur_state (transfer->ssm);
  fp_dbg ("%s current ssm state: %d", G_STRFUNC, ssm_state);

  if (data->cmdtype == FPC_CMDTYPE_TO_DEVICE)
    {
      /* It should not receive any data. */
      if (data->callback)
        data->callback (self, NULL, NULL);

      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }
  else if (data->cmdtype == FPC_CMDTYPE_TO_DEVICE_EVTDATA)
    {
      if (ssm_state == FP_CMD_SEND)
        {
          fpi_ssm_next_state (transfer->ssm);
          return;
        }

      if (ssm_state == FP_CMD_GET_DATA)
        {
          fpc_cmd_response_t evt_data = {0};
          fp_dbg ("%s recv evt data length: %ld", G_STRFUNC, transfer->actual_length);
          if (transfer->actual_length == 0)
            {
              fp_err ("%s Expect data but actual_length = 0", G_STRFUNC);
              fpi_ssm_mark_failed (transfer->ssm,
                                   fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
              return;
            }

          memcpy (&evt_data, transfer->buffer, transfer->actual_length);

          if (data->callback)
            data->callback (self, (guint8 *) &evt_data, NULL);

          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }
    }
  else if (data->cmdtype == FPC_CMDTYPE_FROM_DEVICE)
    {
      if (transfer->actual_length == 0)
        {
          fp_err ("%s Expect data but actual_length = 0", G_STRFUNC);
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
          return;
        }

      if (data->callback)
        data->callback (self, transfer->buffer, NULL);

      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }
  else
    {
      fp_err ("%s incorrect cmdtype (%x) ", G_STRFUNC, data->cmdtype);
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  /* should not run here... */
  fpi_ssm_mark_failed (transfer->ssm,
                       fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
}

static void
fpc_send_ctrl_cmd (FpDevice *dev)
{
  FpiUsbTransfer *transfer = NULL;
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);
  CommandData *cmd_data = fpi_ssm_get_data (self->cmd_ssm);
  FpcCmdType cmdtype = FPC_CMDTYPE_UNKNOWN;

  if (!cmd_data)
    {
      fp_err ("%s No cmd_data is set ", G_STRFUNC);
      fpi_ssm_mark_failed (self->cmd_ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
    }

  cmdtype = cmd_data->cmdtype;

  if ((cmdtype != FPC_CMDTYPE_FROM_DEVICE) && cmd_data->data_len &&
      (cmd_data->data == NULL))
    {
      fp_err ("%s data buffer is null but len is not! ", G_STRFUNC);
      fpi_ssm_mark_failed (self->cmd_ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
    }

  if (cmdtype == FPC_CMDTYPE_UNKNOWN)
    {
      fp_err ("%s unknown cmd type ", G_STRFUNC);
      fpi_ssm_mark_failed (self->cmd_ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
    }

  fp_dbg ("%s CMD: 0x%x, value: 0x%x, index: %x type: %d", G_STRFUNC,
          cmd_data->request, cmd_data->value, cmd_data->index, cmdtype);

  transfer = fpi_usb_transfer_new (dev);
  fpi_usb_transfer_fill_control (transfer,
                                 ((cmdtype == FPC_CMDTYPE_FROM_DEVICE) ?
                                  (G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST) :
                                  (G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE)),
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 cmd_data->request,
                                 cmd_data->value,
                                 cmd_data->index,
                                 cmd_data->data_len);

  transfer->ssm = self->cmd_ssm;
  if (cmdtype != FPC_CMDTYPE_FROM_DEVICE &&
      cmd_data->data != NULL &&
      cmd_data->data_len != 0)
    memcpy (transfer->buffer, cmd_data->data, cmd_data->data_len);

  fpi_usb_transfer_submit (transfer, CTRL_TIMEOUT, NULL,
                           fpc_cmd_receive_cb,
                           fpi_ssm_get_data (transfer->ssm));
}

static void
fpc_cmd_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);
  CommandData *data = fpi_ssm_get_data (ssm);

  /* Notify about the SSM failure from here instead. */
  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      if (data->callback)
        data->callback (self, NULL, error);
    }

  self->cmd_ssm = NULL;
}

static void
fpc_cmd_run_state (FpiSsm   *ssm,
                   FpDevice *dev)
{
  FpiUsbTransfer *transfer = NULL;
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_CMD_SEND:
      fpc_send_ctrl_cmd (dev);
      break;

    case FP_CMD_GET_DATA:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_bulk (transfer, EP_IN, EP_IN_MAX_BUF_SIZE);
      fpi_usb_transfer_submit (transfer,
                               self->cmd_data_timeout,
                               self->interrupt_cancellable,
                               fpc_cmd_receive_cb,
                               fpi_ssm_get_data (ssm));
      break;

    case FP_CMD_SUSPENDED:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_control (transfer,
                                     G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     FPC_CMD_INDICATE_S_STATE,
                                     FPC_HOST_MS_SX,
                                     0,
                                     0);

      fpi_usb_transfer_submit (transfer, CTRL_TIMEOUT, NULL,
                               fpc_suspend_resume_cb, NULL);
      break;

    case FP_CMD_RESUME:
      transfer = fpi_usb_transfer_new (dev);
      transfer->ssm = ssm;
      fpi_usb_transfer_fill_control (transfer,
                                     G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                     G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                     G_USB_DEVICE_RECIPIENT_DEVICE,
                                     FPC_CMD_INDICATE_S_STATE,
                                     FPC_HOST_MS_S0,
                                     0,
                                     0);

      fpi_usb_transfer_submit (transfer, CTRL_TIMEOUT, NULL,
                               fpc_suspend_resume_cb, NULL);
      break;
    }

}

static void
fpc_sensor_cmd (FpiDeviceFpcMoc *self,
                gboolean         wait_data_delay,
                CommandData     *cmd_data)
{
  CommandData *data = NULL;

  g_return_if_fail (cmd_data);
  g_return_if_fail (cmd_data->cmdtype != FPC_CMDTYPE_UNKNOWN);

  data = g_memdup2 (cmd_data, sizeof (CommandData));

  if (wait_data_delay)
    {
      self->cmd_data_timeout = 0;
      g_set_object (&self->interrupt_cancellable, g_cancellable_new ());
    }
  else
    {
      self->cmd_data_timeout = DATA_TIMEOUT;
      g_clear_object (&self->interrupt_cancellable);
    }

  self->cmd_ssm = fpi_ssm_new (FP_DEVICE (self),
                               fpc_cmd_run_state,
                               FP_CMD_NUM_STATES);

  fpi_ssm_set_data (self->cmd_ssm, data, g_free);
  fpi_ssm_start (self->cmd_ssm, fpc_cmd_ssm_done);
}

static void
fpc_dev_release_interface (FpiDeviceFpcMoc *self,
                           GError          *error)
{
  g_autoptr(GError) release_error = NULL;

  /* Release usb interface */
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (self)),
                                  0, 0, &release_error);
  /* Retain passed error if set, otherwise propagate error from release. */
  if (error)
    {
      fpi_device_close_complete (FP_DEVICE (self), error);
      return;
    }

  /* Notify close complete */
  fpi_device_close_complete (FP_DEVICE (self), release_error);
}

static gboolean
check_data (void *data, GError **error)
{
  if (*error != NULL)
    return FALSE;

  if (data == NULL)
    {
      g_propagate_error (error,
                         fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return FALSE;
    }

  return TRUE;
}

static void
fpc_evt_cb (FpiDeviceFpcMoc *self,
            void            *data,
            GError          *error)
{
  pfpc_cmd_response_t presp = NULL;

  if (!check_data (data, &error))
    {
      fp_err ("%s error: %s", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  presp = (pfpc_cmd_response_t) data;

  switch (presp->evt_hdr.cmdid)
    {
    case FPC_EVT_FID_DATA:
      fp_dbg ("%s Enum Fids: status = %d, NumFids = %d", G_STRFUNC,
              presp->evt_enum_fids.status, presp->evt_enum_fids.num_ids);
      if (presp->evt_enum_fids.status || (presp->evt_enum_fids.num_ids > FPC_TEMPLATES_MAX))
        {
          fpi_ssm_mark_failed (self->task_ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                         "Get Fids failed"));
          return;
        }
      break;

    case FPC_EVT_INIT_RESULT:
      fp_dbg ("%s INIT: status=%d, Sensor = %d, HWID = 0x%04X, WxH = %d x %d", G_STRFUNC,
              presp->evt_inited.hdr.status, presp->evt_inited.sensor,
              presp->evt_inited.hw_id, presp->evt_inited.img_w, presp->evt_inited.img_h);

      fp_dbg ("%s INIT: FW version: %s", G_STRFUNC, (gchar *) presp->evt_inited.fw_version);
      break;

    case FPC_EVT_FINGER_DWN:
      fp_dbg ("%s Got finger down event", G_STRFUNC);
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_PRESENT,
                                               FP_FINGER_STATUS_NONE);
      break;

    case FPC_EVT_IMG:
      fp_dbg ("%s Got capture event", G_STRFUNC);
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_NONE,
                                               FP_FINGER_STATUS_PRESENT);
      break;

    default:
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "Unknown Evt (0x%x)!", presp->evt_hdr.cmdid));
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fpc_do_abort_cb (FpiDeviceFpcMoc *self,
                 void            *data,
                 GError          *error)
{
  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_dbg ("%s Do abort for reasons", G_STRFUNC);
  fpi_ssm_next_state (self->task_ssm);
}

static void
fpc_do_cleanup_cb (FpiDeviceFpcMoc *self,
                   void            *data,
                   GError          *error)
{
  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fp_dbg ("%s Do cleanup for reasons", G_STRFUNC);
  self->do_cleanup = FALSE;
  fpi_ssm_next_state (self->task_ssm);
}

static void
fpc_template_delete_cb (FpiDeviceFpcMoc *self,
                        void            *resp,
                        GError          *error)
{
  FpDevice *device = FP_DEVICE (self);

  fpi_device_delete_complete (device, error);
}

static gboolean
parse_print_data (GVariant      *data,
                  guint8        *finger,
                  const guint8 **user_id,
                  gsize         *user_id_len)
{
  g_autoptr(GVariant) user_id_var = NULL;

  g_return_val_if_fail (data, FALSE);
  g_return_val_if_fail (finger, FALSE);
  g_return_val_if_fail (user_id, FALSE);
  g_return_val_if_fail (user_id_len, FALSE);

  *user_id = NULL;
  *user_id_len = 0;
  *finger = FPC_SUBTYPE_NOINFORMATION;

  if (!g_variant_check_format_string (data, "(y@ay)", FALSE))
    return FALSE;

  g_variant_get (data,
                 "(y@ay)",
                 finger,
                 &user_id_var);

  *user_id = g_variant_get_fixed_array (user_id_var, user_id_len, 1);

  if (*user_id_len == 0 || *user_id_len > SECURITY_MAX_SID_SIZE)
    return FALSE;

  if (*user_id_len <= 0 || *user_id[0] == '\0' || *user_id[0] == ' ')
    return FALSE;

  if (*finger != FPC_SUBTYPE_RESERVED)
    return FALSE;

  return TRUE;
}

/******************************************************************************
 *
 *  fpc_template_xxx Function
 *
 *****************************************************************************/
static FpPrint *
fpc_print_from_data (FpiDeviceFpcMoc *self, fpc_fid_data_t *fid_data)
{
  FpPrint *print = NULL;
  GVariant *data;
  GVariant *uid;
  g_autofree gchar *userid = NULL;

  userid = g_strndup ((gchar *) fid_data->identity, fid_data->identity_size);
  print = fp_print_new (FP_DEVICE (self));

  uid = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                   fid_data->identity,
                                   fid_data->identity_size,
                                   1);

  data = g_variant_new ("(y@ay)",
                        fid_data->subfactor,
                        uid);

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", data, NULL);
  g_object_set (print, "description", userid, NULL);
  fpi_print_fill_from_user_id (print, userid);

  return print;
}

static void
fpc_template_list_cb (FpiDeviceFpcMoc *self,
                      void            *data,
                      GError          *error)
{
  g_autoptr(GPtrArray) list_result = NULL;
  FpDevice *device = FP_DEVICE (self);
  pfpc_cmd_response_t presp = NULL;

  if (error)
    {
      fpi_device_list_complete (FP_DEVICE (self), NULL, error);
      return;
    }

  if (data == NULL)
    {
      fpi_device_list_complete (FP_DEVICE (self),
                                NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                          "Data is null"));
      return;
    }

  presp = (pfpc_cmd_response_t) data;
  if (presp->evt_hdr.cmdid != FPC_EVT_FID_DATA)
    {
      fpi_device_list_complete (FP_DEVICE (self),
                                NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                          "Recv evt is incorrect: 0x%x",
                                                          presp->evt_hdr.cmdid));
      return;
    }

  if (presp->evt_enum_fids.num_ids > FPC_TEMPLATES_MAX)
    {
      fpi_device_list_complete (FP_DEVICE (self),
                                NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                                          "Database is full"));
      return;
    }

  list_result = g_ptr_array_new_with_free_func (g_object_unref);

  if (presp->evt_enum_fids.num_ids == 0)
    {
      fp_info ("Database is empty");
      fpi_device_list_complete (device,
                                g_steal_pointer (&list_result),
                                NULL);
      return;
    }

  for (int n = 0; n < presp->evt_enum_fids.num_ids; n++)
    {
      FpPrint *print = NULL;
      fpc_fid_data_t *fid_data = &presp->evt_enum_fids.fid_data[n];

      if ((fid_data->subfactor != FPC_SUBTYPE_RESERVED) &&
          (fid_data->identity_type != FPC_IDENTITY_TYPE_RESERVED))
        {
          fp_info ("Incompatible template found (0x%x, 0x%x)",
                   fid_data->subfactor, fid_data->identity_type);
          continue;
        }

      print = fpc_print_from_data (self, fid_data);

      g_ptr_array_add (list_result, g_object_ref_sink (print));
    }

  fp_info ("Query templates complete!");
  fpi_device_list_complete (device,
                            g_steal_pointer (&list_result),
                            NULL);
}

/******************************************************************************
 *
 *  fpc_enroll_xxxx Function
 *
 *****************************************************************************/

static void
fpc_enroll_create_cb (FpiDeviceFpcMoc *self,
                      void            *data,
                      GError          *error)
{
  FPC_BEGIN_ENROL *presp = NULL;

  if (!check_data (data, &error))
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  presp = (FPC_BEGIN_ENROL *) data;
  if (presp->status != 0)
    {
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                        "End Enroll failed: %d",
                                        presp->status);
    }

  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  else
    {
      self->do_cleanup = TRUE;
      fpi_ssm_next_state (self->task_ssm);
    }
}

static void
fpc_enroll_update_cb (FpiDeviceFpcMoc *self,
                      void            *data,
                      GError          *error)
{
  FPC_ENROL *presp = NULL;

  if (!check_data (data, &error))
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  presp = (FPC_ENROL *) data;
  fp_dbg ("Enrol Update status: %d, remaining: %d", presp->status, presp->remaining);
  switch (presp->status)
    {
    case FPC_ENROL_STATUS_FAILED_COULD_NOT_COMPLETE:
      error = fpi_device_error_new (FP_DEVICE_ERROR_GENERAL);
      break;

    case FPC_ENROL_STATUS_FAILED_ALREADY_ENROLED:
      error = fpi_device_error_new (FP_DEVICE_ERROR_DATA_DUPLICATE);
      break;

    case FPC_ENROL_STATUS_COMPLETED:
      self->enroll_stage++;
      fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, NULL);
      fpi_ssm_jump_to_state (self->task_ssm, FP_ENROLL_COMPLETE);
      return;

    case FPC_ENROL_STATUS_IMAGE_TOO_SIMILAR:
      fp_dbg ("Sample overlapping ratio is too High");
      /* here should tips remove finger and try again */
      if (self->max_immobile_stage)
        {
          if (self->immobile_stage >= self->max_immobile_stage)
            {
              fp_dbg ("Skip similar handle due to customer enrollment %d(%d)",
                      self->immobile_stage, self->max_immobile_stage);
              /* Skip too similar handle, treat as normal enroll progress. */
              fpi_ssm_jump_to_state (self->task_ssm, FPC_ENROL_STATUS_PROGRESS);
              break;
            }
          self->immobile_stage++;
        }
      fpi_device_enroll_progress (FP_DEVICE (self),
                                  self->enroll_stage,
                                  NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_REMOVE_FINGER));
      break;

    case FPC_ENROL_STATUS_PROGRESS:
      self->enroll_stage++;
      fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_stage, NULL, NULL);
      /* Used for customer enrollment scheme */
      if (self->enroll_stage >= (self->max_enroll_stage - self->max_immobile_stage))
        fpi_ssm_jump_to_state (self->task_ssm, FP_ENROLL_COMPLETE);
      break;

    case FPC_ENROL_STATUS_IMAGE_LOW_COVERAGE:
      fpi_device_enroll_progress (FP_DEVICE (self),
                                  self->enroll_stage,
                                  NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
      break;

    case FPC_ENROL_STATUS_IMAGE_LOW_QUALITY:
      fpi_device_enroll_progress (FP_DEVICE (self),
                                  self->enroll_stage,
                                  NULL,
                                  fpi_device_retry_new (FP_DEVICE_RETRY_TOO_SHORT));
      break;

    default:
      fp_err ("%s Unknown result code: %d ", G_STRFUNC, presp->status);
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                        "Enroll failed: %d",
                                        presp->status);
      break;
    }

  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  else
    {
      fpi_ssm_jump_to_state (self->task_ssm, FP_ENROLL_CAPTURE);
    }
}

static void
fpc_enroll_complete_cb (FpiDeviceFpcMoc *self,
                        void            *data,
                        GError          *error)
{
  FPC_END_ENROL *presp = NULL;

  self->do_cleanup = FALSE;

  if (check_data (data, &error))
    {
      presp = (FPC_END_ENROL *) data;
      if (presp->status != 0)
        {
          error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                            "End Enroll failed: %d",
                                            presp->status);
        }
      else
        {
          fp_dbg ("Enrol End status: %d, fid: 0x%x",
                  presp->status, presp->fid);
        }
    }

  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  else
    {
      fpi_ssm_next_state (self->task_ssm);
    }
}

static void
fpc_enroll_check_duplicate_cb (FpiDeviceFpcMoc *self,
                               void            *data,
                               GError          *error)
{
  FPC_IDENTIFY *presp = NULL;

  if (check_data (data, &error))
    {
      presp = (FPC_IDENTIFY *) data;
      if ((presp->status == 0) && (presp->subfactor == FPC_SUBTYPE_RESERVED) &&
          (presp->identity_type == FPC_IDENTITY_TYPE_RESERVED) &&
          (presp->identity_size <= SECURITY_MAX_SID_SIZE))
        {
          fp_info ("%s Got a duplicated template", G_STRFUNC);
          error = fpi_device_error_new (FP_DEVICE_ERROR_DATA_DUPLICATE);
        }
    }

  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  else
    {
      fpi_ssm_next_state (self->task_ssm);
    }

}

static void
fpc_enroll_bindid_cb (FpiDeviceFpcMoc *self,
                      void            *data,
                      GError          *error)
{
  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);
}

static void
fpc_enroll_commit_cb (FpiDeviceFpcMoc *self,
                      void            *data,
                      GError          *error)
{
  gint32 *result = NULL;

  if (check_data (data, &error))
    {
      result = (gint32 *) data;
      if (*result != 0)
        {
          error = fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_FULL,
                                            "Save DB failed: %d",
                                            *result);
        }
    }

  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }
  else
    {
      fpi_ssm_mark_completed (self->task_ssm);
    }
}


static void
fpc_enroll_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  CommandData cmd_data = {0};
  gsize recv_data_len = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_ENROLL_ENUM:
      {
        FPC_FID_DATA pquery_data = {0};
        gsize query_data_len = 0;
        guint32 wildcard_value = FPC_IDENTITY_WILDCARD;
        query_data_len = sizeof (FPC_FID_DATA);
        pquery_data.identity_type = FPC_IDENTITY_TYPE_WILDCARD;
        pquery_data.reserved = 16;
        pquery_data.identity_size = sizeof (wildcard_value);
        pquery_data.subfactor = (guint32) FPC_SUBTYPE_ANY;
        memcpy (&pquery_data.data[0],
                &wildcard_value, pquery_data.identity_size);

        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
        cmd_data.request = FPC_CMD_ENUM;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = (guint8 *) &pquery_data;
        cmd_data.data_len = query_data_len;
        cmd_data.callback = fpc_evt_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_CREATE:
      {
        recv_data_len = sizeof (FPC_BEGIN_ENROL);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_BEGIN_ENROL;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_enroll_create_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_CAPTURE:
      {
        guint32 capture_id = FPC_CAPTUREID_RESERVED;
        fpi_device_report_finger_status_changes (device,
                                                 FP_FINGER_STATUS_NEEDED,
                                                 FP_FINGER_STATUS_NONE);
        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
        cmd_data.request = FPC_CMD_ARM;
        cmd_data.value = 0x1;
        cmd_data.index = 0x0;
        cmd_data.data = (guint8 *) &capture_id;
        cmd_data.data_len = sizeof (guint32);
        cmd_data.callback = fpc_evt_cb;

        fpc_sensor_cmd (self, TRUE, &cmd_data);
      }
      break;

    case FP_ENROLL_GET_IMG:
      {
        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
        cmd_data.request = FPC_CMD_GET_IMG;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = 0;
        cmd_data.callback = fpc_evt_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_UPDATE:
      {
        recv_data_len = sizeof (FPC_ENROL);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_ENROL;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_enroll_update_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_COMPLETE:
      {
        recv_data_len = sizeof (FPC_END_ENROL);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_END_ENROL;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_enroll_complete_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_CHECK_DUPLICATE:
      {
        recv_data_len = sizeof (FPC_IDENTIFY);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_IDENTIFY;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_enroll_check_duplicate_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_BINDID:
      {
        FPC_FID_DATA data = {0};
        gsize data_len = 0;
        FpPrint *print = NULL;
        GVariant *fpi_data = NULL;
        GVariant *uid = NULL;
        guint finger = FPC_SUBTYPE_RESERVED;
        g_autofree gchar *user_id = NULL;
        gssize user_id_len;
        g_autofree guint8 *payload = NULL;

        fpi_device_get_enroll_data (device, &print);

        user_id = fpi_print_generate_user_id (print);

        user_id_len = strlen (user_id);
        user_id_len = MIN (SECURITY_MAX_SID_SIZE, user_id_len);

        uid = g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                         user_id,
                                         user_id_len,
                                         1);
        fpi_data = g_variant_new ("(y@ay)",
                                  finger,
                                  uid);

        fpi_print_set_type (print, FPI_PRINT_RAW);
        fpi_print_set_device_stored (print, TRUE);
        g_object_set (print, "fpi-data", fpi_data, NULL);
        g_object_set (print, "description", user_id, NULL);

        fp_dbg ("user_id: %s, finger: 0x%x", user_id, finger);

        data_len = sizeof (FPC_FID_DATA);
        data.identity_type = FPC_IDENTITY_TYPE_RESERVED;
        data.reserved = 16;
        data.identity_size = user_id_len;
        data.subfactor = (guint32) finger;
        memcpy (&data.data[0],
                user_id, user_id_len);

        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE;
        cmd_data.request = FPC_CMD_BIND_IDENTITY;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = (guint8 *) &data;
        cmd_data.data_len = data_len;
        cmd_data.callback = fpc_enroll_bindid_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_COMMIT:
      {
        recv_data_len = sizeof (gint32);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_STORE_DB;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_enroll_commit_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_DICARD:
      {
        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE;
        cmd_data.request = FPC_CMD_ABORT;
        cmd_data.value = 0x1;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = 0;
        cmd_data.callback = fpc_do_abort_cb;
        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_ENROLL_CLEANUP:
      {
        if (self->do_cleanup == TRUE)
          {
            recv_data_len = sizeof (FPC_END_ENROL);
            cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
            cmd_data.request = FPC_CMD_END_ENROL;
            cmd_data.value = 0x0;
            cmd_data.index = 0x0;
            cmd_data.data = NULL;
            cmd_data.data_len = recv_data_len;
            cmd_data.callback = fpc_do_cleanup_cb;
            fpc_sensor_cmd (self, FALSE, &cmd_data);
          }
        else
          {
            fpi_ssm_next_state (self->task_ssm);
          }
      }
      break;
    }
}

static void
fpc_enroll_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);
  FpPrint *print = NULL;

  fp_info ("Enrollment complete!");

  if (fpi_ssm_get_error (ssm))
    error = fpi_ssm_get_error (ssm);

  if (error)
    {
      fpi_device_enroll_complete (dev, NULL, error);
      self->task_ssm = NULL;
      return;
    }

  fpi_device_get_enroll_data (FP_DEVICE (self), &print);
  fpi_device_enroll_complete (FP_DEVICE (self), g_object_ref (print), NULL);
  self->task_ssm = NULL;
}

/******************************************************************************
 *
 *  fpc_verify_xxx function
 *
 *****************************************************************************/

static void
fpc_verify_cb (FpiDeviceFpcMoc *self,
               void            *data,
               GError          *error)
{
  g_autoptr(GPtrArray) templates = NULL;
  FpDevice *device = FP_DEVICE (self);
  gboolean found = FALSE;
  FpiDeviceAction current_action;
  FPC_IDENTIFY *presp = NULL;

  if (!check_data (data, &error))
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  presp = (FPC_IDENTIFY *) data;
  current_action = fpi_device_get_current_action (device);

  g_assert (current_action == FPI_DEVICE_ACTION_VERIFY ||
            current_action == FPI_DEVICE_ACTION_IDENTIFY);

  if ((presp->status == 0) && (presp->subfactor == FPC_SUBTYPE_RESERVED) &&
      (presp->identity_type == FPC_IDENTITY_TYPE_RESERVED) &&
      (presp->identity_size <= SECURITY_MAX_SID_SIZE))
    {
      FpPrint *match = NULL;
      FpPrint *print = NULL;
      gint cnt = 0;
      fpc_fid_data_t fid_data = {0};

      fid_data.subfactor = presp->subfactor;
      fid_data.identity_type = presp->identity_type;
      fid_data.identity_size = presp->identity_size;
      memcpy (fid_data.identity,  &presp->data[0],
              fid_data.identity_size);

      match = fpc_print_from_data (self, &fid_data);

      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        {
          templates = g_ptr_array_sized_new (1);
          fpi_device_get_verify_data (device, &print);
          g_ptr_array_add (templates, print);
        }
      else
        {
          fpi_device_get_identify_data (device, &templates);
          g_ptr_array_ref (templates);
        }

      for (cnt = 0; cnt < templates->len; cnt++)
        {
          print = g_ptr_array_index (templates, cnt);

          if (fp_print_equal (print, match))
            {
              found = TRUE;
              break;
            }
        }

      if (found)
        {
          if (current_action == FPI_DEVICE_ACTION_VERIFY)
            fpi_device_verify_report (device, FPI_MATCH_SUCCESS, match, error);
          else
            fpi_device_identify_report (device, print, match, error);

          fpi_ssm_mark_completed (self->task_ssm);
          return;
        }
    }

  if (!found)
    {
      if (current_action == FPI_DEVICE_ACTION_VERIFY)
        fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, error);
      else
        fpi_device_identify_report (device, NULL, NULL, error);
    }

  /* This is the last state for verify/identify */
  fpi_ssm_mark_completed (self->task_ssm);
}

static void
fpc_verify_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  CommandData cmd_data = {0};

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_VERIFY_CAPTURE:
      {
        guint32 capture_id = FPC_CAPTUREID_RESERVED;
        fpi_device_report_finger_status_changes (device,
                                                 FP_FINGER_STATUS_NEEDED,
                                                 FP_FINGER_STATUS_NONE);
        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
        cmd_data.request = FPC_CMD_ARM;
        cmd_data.value = 0x1;
        cmd_data.index = 0x0;
        cmd_data.data = (guint8 *) &capture_id;
        cmd_data.data_len = sizeof (guint32);
        cmd_data.callback = fpc_evt_cb;

        fpc_sensor_cmd (self, TRUE, &cmd_data);
      }
      break;

    case FP_VERIFY_GET_IMG:
      {
        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
        cmd_data.request = FPC_CMD_GET_IMG;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = 0;
        cmd_data.callback = fpc_evt_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_VERIFY_IDENTIFY:
      {
        gsize recv_data_len = sizeof (FPC_IDENTIFY);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_IDENTIFY;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_verify_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;

    case FP_VERIFY_CANCEL:
      {
        cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE;
        cmd_data.request = FPC_CMD_ABORT;
        cmd_data.value = 0x1;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = 0;
        cmd_data.callback = fpc_do_abort_cb;
        fpc_sensor_cmd (self, FALSE, &cmd_data);

      }
      break;
    }
}

static void
fpc_verify_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);

  fp_info ("Verify_identify complete!");

  if (fpi_ssm_get_error (ssm))
    error = fpi_ssm_get_error (ssm);

  if (error && error->domain == FP_DEVICE_RETRY)
    {
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
        fpi_device_verify_report (dev, FPI_MATCH_ERROR, NULL, g_steal_pointer (&error));
      else
        fpi_device_identify_report (dev, NULL, NULL, g_steal_pointer (&error));
    }

  if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_VERIFY)
    fpi_device_verify_complete (dev, error);
  else
    fpi_device_identify_complete (dev, error);

  self->task_ssm = NULL;
}

/******************************************************************************
 *
 *  fpc_init_xxx function
 *
 *****************************************************************************/
static void
fpc_clear_storage_cb (FpiDeviceFpcMoc *self,
                      void            *resp,
                      GError          *error)
{
  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  fpi_ssm_next_state (self->task_ssm);

}

static void
fpc_clear_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  CommandData cmd_data = {0};
  FPC_DB_OP data = {0};
  gsize data_len = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_CLEAR_DELETE_DB:
      {
        if (self->dbid)
          {
            data_len = sizeof (FPC_DB_OP);
            data.database_id_size = FPC_DB_ID_LEN;
            data.reserved = 8;
            memcpy (&data.data[0], self->dbid, FPC_DB_ID_LEN);
            cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE;
            cmd_data.request = FPC_CMD_DELETE_DB;
            cmd_data.value = 0x0;
            cmd_data.index = 0x0;
            cmd_data.data = (guint8 *) &data;
            cmd_data.data_len = data_len;
            cmd_data.callback = fpc_clear_storage_cb;
            fpc_sensor_cmd (self, FALSE, &cmd_data);
          }
        else
          {
            fpi_ssm_mark_failed (self->task_ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                           "No DBID found"));
          }

      }
      break;

    case FP_CLEAR_CREATE_DB:
      {
        if (self->dbid)
          {
            data_len = sizeof (FPC_DB_OP);
            data.database_id_size = FPC_DB_ID_LEN;
            data.reserved = 8;
            memcpy (&data.data[0], self->dbid, FPC_DB_ID_LEN);
            cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE;
            cmd_data.request = FPC_CMD_LOAD_DB;
            cmd_data.value = 0x1;
            cmd_data.index = 0x0;
            cmd_data.data = (guint8 *) &data;
            cmd_data.data_len = data_len;
            cmd_data.callback = fpc_clear_storage_cb;
            fpc_sensor_cmd (self, FALSE, &cmd_data);
          }
        else
          {
            fpi_ssm_mark_failed (self->task_ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED,
                                                           "No DBID found"));
          }
      }
      break;
    }
}

static void
fpc_clear_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);

  fp_info ("Clear Storage complete!");

  if (fpi_ssm_get_error (ssm))
    error = fpi_ssm_get_error (ssm);

  fpi_device_clear_storage_complete (dev, error);
  self->task_ssm = NULL;
}

/******************************************************************************
 *
 *  fpc_init_xxx function
 *
 *****************************************************************************/

static void
fpc_init_load_db_cb (FpiDeviceFpcMoc *self,
                     void            *data,
                     GError          *error)
{
  FPC_LOAD_DB *presp = NULL;

  if (error)
    {
      fp_err ("%s error: %s ", G_STRFUNC, error->message);
      fpi_ssm_mark_failed (self->task_ssm, error);
      return;
    }

  if (data == NULL)
    {
      fpi_ssm_mark_failed (self->task_ssm,
                           fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }
  presp = (FPC_LOAD_DB *) data;
  if (presp->status)
    {
      fp_err ("%s Load DB failed: %d - Expect to create a new one", G_STRFUNC, presp->status);
      fpi_ssm_next_state (self->task_ssm);
      return;
    }

  g_clear_pointer (&self->dbid, g_free);
  self->dbid = g_memdup2 (presp->data, FPC_DB_ID_LEN);
  if (self->dbid == NULL)
    {
      fpi_ssm_mark_failed (self->task_ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  fp_dbg ("%s got dbid size: %d", G_STRFUNC, presp->database_id_size);
  fp_dbg ("%s dbid: 0x%02x%02x%02x%02x-%02x%02x-%02x%02x-" \
          "%02x%02x-%02x%02x%02x%02x%02x%02x",
          G_STRFUNC,
          presp->data[0], presp->data[1],
          presp->data[2], presp->data[3],
          presp->data[4], presp->data[5],
          presp->data[6], presp->data[7],
          presp->data[8], presp->data[9],
          presp->data[10], presp->data[11],
          presp->data[12], presp->data[13],
          presp->data[14], presp->data[15]);
  fpi_ssm_mark_completed (self->task_ssm);
}

static void
fpc_init_sm_run_state (FpiSsm *ssm, FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  guint32 session_id = FPC_SESSIONID_RESERVED;
  CommandData cmd_data = {0};

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FP_INIT:
      cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
      cmd_data.request = FPC_CMD_INIT;
      cmd_data.value = 0x1;
      cmd_data.index = 0x0;
      cmd_data.data = (guint8 *) &session_id;
      cmd_data.data_len = sizeof (session_id);
      cmd_data.callback = fpc_evt_cb;

      fpc_sensor_cmd (self, FALSE, &cmd_data);
      break;

    case FP_LOAD_DB:
      {
        gsize recv_data_len = sizeof (FPC_LOAD_DB);
        cmd_data.cmdtype = FPC_CMDTYPE_FROM_DEVICE;
        cmd_data.request = FPC_CMD_LOAD_DB;
        cmd_data.value = 0x0;
        cmd_data.index = 0x0;
        cmd_data.data = NULL;
        cmd_data.data_len = recv_data_len;
        cmd_data.callback = fpc_init_load_db_cb;

        fpc_sensor_cmd (self, FALSE, &cmd_data);
      }
      break;
    }
}

static void
fpc_init_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (dev);

  if (fpi_ssm_get_error (ssm))
    error = fpi_ssm_get_error (ssm);

  fpi_device_open_complete (dev, error);
  self->task_ssm = NULL;
}

/******************************************************************************
 *
 *  Interface Function
 *
 *****************************************************************************/

static void
fpc_dev_probe (FpDevice *device)
{
  GUsbDevice *usb_dev;
  GError *error = NULL;
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  g_autofree gchar *product = NULL;
  gint product_id = 0;

  fp_dbg ("%s enter --> ", G_STRFUNC);

  /* Claim usb interface */
  usb_dev = fpi_device_get_usb_device (device);
  if (!g_usb_device_open (usb_dev, &error))
    {
      fp_dbg ("%s g_usb_device_open failed %s", G_STRFUNC, error->message);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_reset (usb_dev, &error))
    {
      fp_dbg ("%s g_usb_device_reset failed %s", G_STRFUNC, error->message);
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      fp_dbg ("%s g_usb_device_claim_interface failed %s", G_STRFUNC, error->message);
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  product = g_usb_device_get_string_descriptor (usb_dev,
                                                g_usb_device_get_product_index (usb_dev),
                                                &error);
  if (product)
    fp_dbg ("Device name: %s", product);

  if (error)
    {
      fp_dbg ("%s g_usb_device_get_string_descriptor failed %s", G_STRFUNC, error->message);
      g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (device)),
                                      0, 0, NULL);
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, error);
      return;
    }

  product_id = g_usb_device_get_pid (usb_dev);
  /* Reserved for customer enroll scheme */
  self->max_immobile_stage = 0;   /* By default is not customer enrollment */
  switch (product_id)
    {
    case 0xFFE0:
    case 0xA305:
    case 0xD805:
    case 0xDA04:
    case 0xD205:
      self->max_enroll_stage = MAX_ENROLL_SAMPLES;
      break;

    default:
      fp_warn ("Device %x is not supported", product_id);
      self->max_enroll_stage = MAX_ENROLL_SAMPLES;
      break;
    }
  fpi_device_set_nr_enroll_stages (device, self->max_enroll_stage);
  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (device)),
                                  0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);
  fpi_device_probe_complete (device, NULL, product, error);
}

static void
fpc_dev_open (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  GError *error = NULL;

  fp_dbg ("%s enter -->", G_STRFUNC);
  if (!g_usb_device_reset (fpi_device_get_usb_device (device), &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  /* Claim usb interface */
  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (device), 0, 0, &error))
    {
      fpi_device_open_complete (FP_DEVICE (self), error);
      return;
    }

  self->task_ssm = fpi_ssm_new (device, fpc_init_sm_run_state,
                                FP_INIT_NUM_STATES);

  fpi_ssm_start (self->task_ssm, fpc_init_ssm_done);
}

static void
fpc_dev_close (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);

  fp_dbg ("%s enter -->", G_STRFUNC);
  g_clear_pointer (&self->dbid, g_free);
  g_clear_object (&self->interrupt_cancellable);
  fpc_dev_release_interface (self, NULL);
}

static void
fpc_dev_verify_identify (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);

  fp_dbg ("%s enter -->", G_STRFUNC);
  self->task_ssm = fpi_ssm_new_full (device, fpc_verify_sm_run_state,
                                     FP_VERIFY_NUM_STATES,
                                     FP_VERIFY_CANCEL,
                                     "verify_identify");

  fpi_ssm_start (self->task_ssm, fpc_verify_ssm_done);
}

static void
fpc_dev_enroll (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);

  fp_dbg ("%s enter -->", G_STRFUNC);

  self->enroll_stage = 0;
  self->immobile_stage = 0;
  self->task_ssm = fpi_ssm_new_full (device, fpc_enroll_sm_run_state,
                                     FP_ENROLL_NUM_STATES,
                                     FP_ENROLL_DICARD,
                                     "enroll");

  fpi_ssm_start (self->task_ssm, fpc_enroll_ssm_done);
}

static void
fpc_dev_template_list (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  CommandData cmd_data = {0};
  FPC_FID_DATA pquery_data = {0};
  gsize query_data_len = 0;
  guint32 wildcard_value = FPC_IDENTITY_WILDCARD;

  fp_dbg ("%s enter -->", G_STRFUNC);

  query_data_len = sizeof (FPC_FID_DATA);
  pquery_data.identity_type = FPC_IDENTITY_TYPE_WILDCARD;
  pquery_data.reserved = 16;
  pquery_data.identity_size = sizeof (wildcard_value);
  pquery_data.subfactor = (guint32) FPC_SUBTYPE_ANY;
  memcpy (&pquery_data.data[0],
          &wildcard_value, pquery_data.identity_size);

  cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE_EVTDATA;
  cmd_data.request = FPC_CMD_ENUM;
  cmd_data.value = 0x0;
  cmd_data.index = 0x0;
  cmd_data.data = (guint8 *) &pquery_data;
  cmd_data.data_len = query_data_len;
  cmd_data.callback = fpc_template_list_cb;

  fpc_sensor_cmd (self, FALSE, &cmd_data);
}

static void
fpc_dev_suspend (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  FpiDeviceAction action = fpi_device_get_current_action (device);

  fp_dbg ("%s enter -->", G_STRFUNC);

  if (action != FPI_DEVICE_ACTION_VERIFY && action != FPI_DEVICE_ACTION_IDENTIFY)
    {
      fpi_device_suspend_complete (device, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  g_assert (self->cmd_ssm);
  g_assert (fpi_ssm_get_cur_state (self->cmd_ssm) == FP_CMD_GET_DATA);
  self->cmd_suspended = TRUE;
  g_cancellable_cancel (self->interrupt_cancellable);
}

static void
fpc_dev_resume (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  FpiDeviceAction action = fpi_device_get_current_action (device);

  fp_dbg ("%s enter -->", G_STRFUNC);

  if (action != FPI_DEVICE_ACTION_VERIFY && action != FPI_DEVICE_ACTION_IDENTIFY)
    {
      g_assert_not_reached ();
      fpi_device_resume_complete (device, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
      return;
    }

  g_assert (self->cmd_ssm);
  g_assert (self->cmd_suspended);
  g_assert (fpi_ssm_get_cur_state (self->cmd_ssm) == FP_CMD_SUSPENDED);
  self->cmd_suspended = FALSE;
  g_set_object (&self->interrupt_cancellable, g_cancellable_new ());
  fpi_ssm_jump_to_state (self->cmd_ssm, FP_CMD_RESUME);
}

static void
fpc_dev_cancel (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);

  fp_dbg ("%s enter -->", G_STRFUNC);
  g_cancellable_cancel (self->interrupt_cancellable);
}

static void
fpc_dev_template_delete (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);
  CommandData cmd_data = {0};
  FpPrint *print = NULL;

  g_autoptr(GVariant) fpi_data = NULL;
  FPC_FID_DATA data = {0};
  gsize data_len = 0;
  guint8 finger = FPC_SUBTYPE_NOINFORMATION;
  const guint8 *user_id;
  gsize user_id_len = 0;

  fp_dbg ("%s enter -->", G_STRFUNC);

  fpi_device_get_delete_data (device, &print);

  g_object_get (print, "fpi-data", &fpi_data, NULL);

  if (!parse_print_data (fpi_data, &finger, &user_id, &user_id_len))
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  data_len = sizeof (FPC_FID_DATA);
  data.identity_type = FPC_IDENTITY_TYPE_RESERVED;
  data.reserved = 16;
  data.identity_size = user_id_len;
  data.subfactor = (guint32) finger;
  memcpy (&data.data[0], user_id, user_id_len);
  cmd_data.cmdtype = FPC_CMDTYPE_TO_DEVICE;
  cmd_data.request = FPC_CMD_DELETE_TEMPLATE;
  cmd_data.value = 0x0;
  cmd_data.index = 0x0;
  cmd_data.data = (guint8 *) &data;
  cmd_data.data_len = data_len;
  cmd_data.callback = fpc_template_delete_cb;

  fpc_sensor_cmd (self, FALSE, &cmd_data);
  fp_dbg ("%s exit <--", G_STRFUNC);
}

static void
fpc_dev_clear_storage (FpDevice *device)
{
  FpiDeviceFpcMoc *self = FPI_DEVICE_FPCMOC (device);

  fp_dbg ("%s enter -->", G_STRFUNC);
  self->task_ssm = fpi_ssm_new_full (device, fpc_clear_sm_run_state,
                                     FP_CLEAR_NUM_STATES,
                                     FP_CLEAR_NUM_STATES,
                                     "Clear storage");

  fpi_ssm_start (self->task_ssm, fpc_clear_ssm_done);
}

static void
fpi_device_fpcmoc_init (FpiDeviceFpcMoc *self)
{
  fp_dbg ("%s enter -->", G_STRFUNC);
  G_DEBUG_HERE ();
}

static void
fpi_device_fpcmoc_class_init (FpiDeviceFpcMocClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id =               FP_COMPONENT;
  dev_class->full_name =        "FPC MOC Fingerprint Sensor";
  dev_class->type =             FP_DEVICE_TYPE_USB;
  dev_class->scan_type =        FP_SCAN_TYPE_PRESS;
  dev_class->id_table =         id_table;
  dev_class->nr_enroll_stages = MAX_ENROLL_SAMPLES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open   =           fpc_dev_open;
  dev_class->close  =           fpc_dev_close;
  dev_class->probe  =           fpc_dev_probe;
  dev_class->enroll =           fpc_dev_enroll;
  dev_class->delete =           fpc_dev_template_delete;
  dev_class->list   =           fpc_dev_template_list;
  dev_class->verify   =         fpc_dev_verify_identify;
  dev_class->identify =         fpc_dev_verify_identify;
  dev_class->suspend =          fpc_dev_suspend;
  dev_class->resume =           fpc_dev_resume;
  dev_class->clear_storage =    fpc_dev_clear_storage;
  dev_class->cancel =           fpc_dev_cancel;

  fpi_device_class_auto_initialize_features (dev_class);
  dev_class->features |= FP_DEVICE_FEATURE_DUPLICATES_CHECK;
}
