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

#pragma once

#include "fpi-device.h"
#include "fpi-ssm.h"

#include <stdio.h>
#include <stdlib.h>

#define TEMPLATE_ID_SIZE (32)
#define MAX_FW_VERSION_STR_LEN (16)
#define FPC_CMD_INIT (0x01)
#define FPC_CMD_ARM (0x02)
#define FPC_CMD_ABORT (0x03)
#define FPC_CMD_INDICATE_S_STATE (0x08)
#define FPC_CMD_GET_IMG (0x09)
#define FPC_CMD_GET_KPI (0x0C)
#define FPC_CMD_LOAD_DB (0x60)
#define FPC_CMD_STORE_DB (0x61)
#define FPC_CMD_DELETE_DB (0x62)
#define FPC_CMD_DELETE_TEMPLATE (0x63)
#define FPC_CMD_BEGIN_ENROL (0x67)
#define FPC_CMD_ENROL (0x68)
#define FPC_CMD_END_ENROL (0x69)
#define FPC_CMD_BIND_IDENTITY (0x6A)
#define FPC_CMD_IDENTIFY (0x6B)
#define FPC_CMD_ENUM (0x70)
#define FPC_EVT_INIT_RESULT (0x02)
#define FPC_EVT_FINGER_DWN (0x06)
#define FPC_EVT_IMG (0x08)
#define FPC_EVT_FID_DATA (0x31)
#define FPC_DB_ID_LEN (16)
#define FPC_IDENTITY_TYPE_WILDCARD (0x1)
#define FPC_IDENTITY_TYPE_RESERVED (0x3)
#define FPC_IDENTITY_WILDCARD (0x25066282)
#define FPC_SUBTYPE_ANY (0xFF)
#define FPC_SUBTYPE_RESERVED (0xF5)
#define FPC_SUBTYPE_NOINFORMATION (0x00)
#define FPC_CAPTUREID_RESERVED (0x701100F)
#define FPC_SESSIONID_RESERVED (0x0077FF12)
#define FPC_TEMPLATES_MAX (10)
#define SECURITY_MAX_SID_SIZE (68)
#define FPC_HOST_MS_S0 (0x10)
#define FPC_HOST_MS_SX (0x11)

G_DECLARE_FINAL_TYPE (FpiDeviceFpcMoc, fpi_device_fpcmoc, FPI,
                      DEVICE_FPCMOC, FpDevice);

typedef struct _FPC_FID_DATA
{
  guint32 identity_type;
  guint32 reserved;
  guint32 identity_size;
  guint32 subfactor;
  guint8  data[SECURITY_MAX_SID_SIZE];
} FPC_FID_DATA, *PFPC_FID_DATA;

typedef struct _FPC_LOAD_DB
{
  gint32  status;
  guint32 reserved;
  guint32 database_id_size;
  guint8  data[FPC_DB_ID_LEN];
} FPC_LOAD_DB, *PFPC_LOAD_DB;

typedef union _FPC_DELETE_DB
{
  guint32 reserved;
  guint32 database_id_size;
  guint8  data[FPC_DB_ID_LEN];
} FPC_DB_OP, *PFPC_DB_OP;

typedef struct _FPC_BEGIN_ENROL
{
  gint32  status;
  guint32 reserved1;
  guint32 reserved2;
} FPC_BEGIN_ENROL, *PFPC_BEGIN_ENROL;

typedef struct _FPC_ENROL
{
  gint32  status;
  guint32 remaining;
} FPC_ENROL, *PFPC_ENROL;

typedef struct _FPC_END_ENROL
{
  gint32  status;
  guint32 fid;
} FPC_END_ENROL, *PFPC_END_ENROL;

typedef struct _FPC_IDENTIFY
{
  gint32  status;
  guint32 identity_type;
  guint32 identity_offset;
  guint32 identity_size;
  guint32 subfactor;
  guint8  data[SECURITY_MAX_SID_SIZE];
} FPC_IDENTIFY, *PFPC_IDENTIFY;

typedef struct
{
  guint32 cmdid;
  guint32 length;
  guint32 status;
} evt_hdr_t;

typedef struct
{
  evt_hdr_t hdr;
  guint16   sensor;
  guint16   hw_id;
  guint16   img_w;
  guint16   img_h;
  guint8    fw_version[MAX_FW_VERSION_STR_LEN];
  guint16   fw_capabilities;
} evt_initiated_t;

typedef struct
{
  guint8  subfactor;
  guint32 identity_type;
  guint32 identity_size;
  guint8  identity[SECURITY_MAX_SID_SIZE];
} __attribute__((packed)) fpc_fid_data_t;

typedef struct
{
  evt_hdr_t      hdr;
  gint           status;
  guint32        num_ids;
  fpc_fid_data_t fid_data[FPC_TEMPLATES_MAX];
} __attribute__((packed)) evt_enum_fids_t;

typedef struct _fp_cmd_response
{
  union
  {
    evt_hdr_t       evt_hdr;
    evt_initiated_t evt_inited;
    evt_enum_fids_t evt_enum_fids;
  };
} fpc_cmd_response_t, *pfpc_cmd_response_t;

enum {
  FPC_ENROL_STATUS_COMPLETED = 0,
  FPC_ENROL_STATUS_PROGRESS = 1,
  FPC_ENROL_STATUS_FAILED_COULD_NOT_COMPLETE = 2,
  FPC_ENROL_STATUS_FAILED_ALREADY_ENROLED = 3,
  FPC_ENROL_STATUS_IMAGE_LOW_COVERAGE = 4,
  FPC_ENROL_STATUS_IMAGE_TOO_SIMILAR = 5,
  FPC_ENROL_STATUS_IMAGE_LOW_QUALITY = 6,
};

typedef enum {
  FPC_CMDTYPE_UNKNOWN = 0,
  FPC_CMDTYPE_TO_DEVICE,
  FPC_CMDTYPE_TO_DEVICE_EVTDATA,
  FPC_CMDTYPE_FROM_DEVICE,
} FpcCmdType;

typedef enum {
  FP_CMD_SEND = 0,
  FP_CMD_GET_DATA,
  FP_CMD_SUSPENDED,
  FP_CMD_RESUME,
  FP_CMD_NUM_STATES,
} FpCmdState;

typedef enum {
  FP_INIT = 0,
  FP_LOAD_DB,
  FP_INIT_NUM_STATES,
} FpInitState;

typedef enum {
  FP_ENROLL_ENUM = 0,
  FP_ENROLL_CREATE,
  FP_ENROLL_CAPTURE,
  FP_ENROLL_GET_IMG,
  FP_ENROLL_UPDATE,
  FP_ENROLL_COMPLETE,
  FP_ENROLL_CHECK_DUPLICATE,
  FP_ENROLL_BINDID,
  FP_ENROLL_COMMIT,
  FP_ENROLL_DICARD,
  FP_ENROLL_CLEANUP,
  FP_ENROLL_NUM_STATES,
} FpEnrollState;

typedef enum {
  FP_VERIFY_CAPTURE = 0,
  FP_VERIFY_GET_IMG,
  FP_VERIFY_IDENTIFY,
  FP_VERIFY_CANCEL,
  FP_VERIFY_NUM_STATES,
} FpVerifyState;

typedef enum {
  FP_CLEAR_DELETE_DB = 0,
  FP_CLEAR_CREATE_DB,
  FP_CLEAR_NUM_STATES,
} FpClearState;
