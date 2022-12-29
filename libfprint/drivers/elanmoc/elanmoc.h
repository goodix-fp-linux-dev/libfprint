/*
 * Copyright (C) 2021 Elan Microelectronics
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
#include "fpi-ssm.h"
#include <libusb.h>

#include <stdio.h>
#include <stdlib.h>

G_DECLARE_FINAL_TYPE (FpiDeviceElanmoc, fpi_device_elanmoc, FPI, DEVICE_ELANMOC, FpDevice)

#define ELAN_MOC_DRIVER_FULLNAME "Elan MOC Sensors"
#define ELAN_M0C_CMD_LEN 0x3
#define ELAN_EP_CMD_OUT (0x1 | LIBUSB_ENDPOINT_OUT)
#define ELAN_EP_CMD_IN (0x3 | LIBUSB_ENDPOINT_IN)
#define ELAN_EP_MOC_CMD_IN (0x4 | LIBUSB_ENDPOINT_IN)
#define ELAN_EP_IMG_IN (0x2 | LIBUSB_ENDPOINT_IN)

#define ELAN_MOC_CMD_TIMEOUT 5000
#define ELAN_MOC_CAL_RETRY 500
#define ELAN_MOC_ENROLL_TIMES 9
#define ELAN_MAX_USER_ID_LEN 92
#define ELAN_MAX_ENROLL_NUM 9

#define ELAN_MSG_VERIFY_ERR 0xfd
#define ELAN_MSG_DIRTY 0xfb
#define ELAN_MSG_AREA_NOT_ENOUGH 0xfe
#define ELAN_MSG_TOO_HIGH 0x41
#define ELAN_MSG_TOO_LEFT 0x42
#define ELAN_MSG_TOO_LOW 0x43
#define ELAN_MSG_TOO_RIGHT 0x44
#define ELAN_MSG_OK 0x00

#define ELAN_MAX_HDR_LEN 3
#define ELAN_USERDATE_SIZE (ELAN_MAX_USER_ID_LEN + 3)

#define ELAN_MSG_DRIVER_VERSION "1004"

struct elanmoc_cmd
{
  unsigned char cmd_header[ELAN_MAX_HDR_LEN];
  int           cmd_len;
  int           resp_len;
};

static const struct elanmoc_cmd fw_ver_cmd = {
  .cmd_header = {0x40, 0x19},
  .cmd_len = 2,
  .resp_len = 2,
};

static const struct elanmoc_cmd sensor_dim_cmd = {
  .cmd_header = {0x00, 0x0c},
  .cmd_len = 2,
  .resp_len = 4,
};

static const struct elanmoc_cmd cal_status_cmd = {
  .cmd_header = {0x40, 0xff, 0x00},
  .cmd_len = 3,
  .resp_len = 2,
};

static const struct elanmoc_cmd enrolled_number_cmd = {
  .cmd_header = {0x40, 0xff, 0x04},
  .cmd_len = 3,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_verify_cmd = {
  .cmd_header = {0x40, 0xff, 0x73},
  .cmd_len = 5,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_above_cmd = {
  .cmd_header = {0x40, 0xff, 0x02},
  .cmd_len = 3,
  .resp_len = 0,
};

static const struct elanmoc_cmd elanmoc_enroll_cmd = {
  .cmd_header = {0x40, 0xff, 0x01},
  .cmd_len = 7,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_delete_cmd = {
  .cmd_header = {0x40, 0xff, 0x13},
  .cmd_len = 128,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_enroll_commit_cmd = {
  .cmd_header = {0x40, 0xff, 0x11},
  .cmd_len = 128,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_remove_all_cmd = {
  .cmd_header = {0x40, 0xff, 0x98},
  .cmd_len = 3,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_get_userid_cmd = {
  .cmd_header = {0x43, 0x21, 0x00},
  .cmd_len = 3,
  .resp_len = 97,
};

static const struct elanmoc_cmd elanmoc_set_mod_cmd = {
  .cmd_header = {0x40, 0xff, 0x14},
  .cmd_len = 4,
  .resp_len = 2,
};

static const struct elanmoc_cmd elanmoc_check_reenroll_cmd = {
  .cmd_header = {0x40, 0xff, 0x22},
  .cmd_len = 3 + ELAN_USERDATE_SIZE,
  .resp_len = 2,
};

typedef void (*ElanCmdMsgCallback) (FpiDeviceElanmoc *self,
                                    GError           *error);

enum moc_enroll_states {
  MOC_ENROLL_GET_ENROLLED_NUM,
  MOC_ENROLL_REENROLL_CHECK,
  MOC_ENROLL_WAIT_FINGER,
  MOC_ENROLL_COMMIT_RESULT,
  MOC_ENROLL_NUM_STATES,
};

enum moc_list_states {
  MOC_LIST_GET_ENROLLED,
  MOC_LIST_GET_FINGER,
  MOC_LIST_NUM_STATES,
};

enum delete_states {
  DELETE_SEND_CMD,
  DELETE_NUM_STATES,
};

enum dev_init_states {
  DEV_WAIT_READY,
  DEV_SET_MODE,
  DEV_GET_VER,
  DEV_GET_DIM,
  DEV_GET_ENROLLED,
  DEV_INIT_STATES,
};

enum dev_exit_states {
  DEV_EXIT_ABOVE,
  DEV_EXIT_STATES,
};

struct _FpiDeviceElanmoc
{
  FpDevice        parent;
  FpiSsm         *task_ssm;
  FpiSsm         *cmd_ssm;
  FpiUsbTransfer *cmd_transfer;
  gboolean        cmd_cancelable;
  gsize           cmd_len_in;
  unsigned short  fw_ver;
  unsigned char   x_trace;
  unsigned char   y_trace;
  int             num_frames;
  int             curr_enrolled;
  int             max_moc_enroll_time;
  int             cancel_result;
  int             cmd_retry_cnt;
  int             list_index;
  GPtrArray      *list_result;
};
