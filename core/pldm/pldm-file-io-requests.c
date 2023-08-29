// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
// Copyright 2022 IBM Corp.

#define pr_fmt(fmt) "PLDM: " fmt

#include <opal.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <timebase.h>
#include <libpldm/file_io.h>
#include "pldm.h"

/* list of lid files available */
static void *file_attr_table;
static size_t file_attr_length;

static bool file_io_ready;

static void file_io_init_complete(bool success)
{
	/* Read not successful, error out and free the buffer */
	if (!success) {
		file_io_ready = false;

		if (file_attr_table != NULL) {
			free(file_attr_table);
			file_attr_length = 0;
		}
		return;
	}

	/* Mark ready */
	file_io_ready = true;
}

/* maximum currently transfer size for PLDM */
#define KILOBYTE 1024ul
#define MAX_TRANSFER_SIZE_BYTES (127 * KILOBYTE)

/*
 * Send/receive a PLDM ReadFile request message.
 */
static int read_file_req(uint32_t file_handle, uint32_t file_length,
			 uint32_t pos, void *buf, uint64_t len)
{
	size_t data_size = PLDM_MSG_SIZE(struct pldm_read_file_req);
	size_t response_len, payload_len, file_data_offset;
	uint8_t completion_code;
	struct pldm_tx_data *tx;
	uint32_t resp_length;
	uint64_t total_read;
	void *response_msg;
	int num_transfers;
	void *curr_buf;
	int rc, i;

	struct pldm_read_file_req file_req = {
		.file_handle = file_handle,
		.offset = pos,
		.length = len
	};

	if (!file_length)
		return OPAL_PARAMETER;

	if ((!len) || ((len + pos) > file_length))
		return OPAL_PARAMETER;

	num_transfers = 1;
	curr_buf = buf;
	total_read = 0;

	if (file_req.length > MAX_TRANSFER_SIZE_BYTES) {
		num_transfers = (file_req.length + MAX_TRANSFER_SIZE_BYTES - 1) /
				MAX_TRANSFER_SIZE_BYTES;
		file_req.length = MAX_TRANSFER_SIZE_BYTES;
	}

	prlog(PR_TRACE, "%s - file_handle: %d, offset: 0x%x, len: 0x%llx num_transfers: %d\n",
			__func__, file_handle, file_req.offset,
			len, num_transfers);

	/* init request */
	tx = zalloc(sizeof(struct pldm_tx_data) + data_size);
	if (!tx)
		return OPAL_NO_MEM;
	tx->data_size = data_size;

	for (i = 0; i < num_transfers; i++) {
		file_req.offset = pos + (i * MAX_TRANSFER_SIZE_BYTES);

		/* Encode the file request */
		rc = encode_read_file_req(
				DEFAULT_INSTANCE_ID,
				file_req.file_handle,
				file_req.offset,
				file_req.length,
				(struct pldm_msg *)tx->data);
		if (rc != PLDM_SUCCESS) {
			prlog(PR_ERR, "Encode ReadFileReq Error, rc: %d\n",
				      rc);
			free(tx);
			return OPAL_PARAMETER;
		}

		/* Send and get the response message bytes */
		rc = pldm_requester_queue_and_wait(tx,
						   &response_msg, &response_len);
		if (rc) {
			prlog(PR_ERR, "Communication Error, req: ReadFileReq, rc: %d\n", rc);
			free(tx);
			return rc;
		}

		/* Decode the message */
		payload_len = response_len - sizeof(struct pldm_msg_hdr);
		rc = decode_read_file_resp(
				response_msg,
				payload_len,
				&completion_code,
				&resp_length,
				&file_data_offset);
		if (rc != PLDM_SUCCESS || completion_code != PLDM_SUCCESS) {
			prlog(PR_ERR, "Decode ReadFileResp Error, rc: %d, cc: %d\n",
				      rc, completion_code);
			free(tx);
			free(response_msg);
			return OPAL_PARAMETER;
		}

		if (resp_length == 0) {
			free(response_msg);
			break;
		}

		memcpy(curr_buf,
		       ((struct pldm_msg *)response_msg)->payload + file_data_offset,
		       resp_length);

		total_read += resp_length;
		curr_buf += resp_length;
		free(response_msg);

		prlog(PR_TRACE, "%s - file_handle: %d, resp_length: 0x%x, total_read: 0x%llx\n",
			__func__, file_handle, resp_length, total_read);

		if (total_read >= len)
			break;
		else if (resp_length != file_req.length) {
			/* end of file */
			break;
		} else if (MAX_TRANSFER_SIZE_BYTES > (len - total_read))
			file_req.length = len - total_read;
	}

	free(tx);
	return OPAL_SUCCESS;
}

int pldm_file_io_read_file(uint32_t file_handle, uint32_t file_length,
			   uint32_t pos, void *buf, uint64_t len)
{
	if (!file_io_ready)
		return OPAL_HARDWARE;

	return read_file_req(file_handle, file_length, pos, buf, len);
}

/*
 * Send/receive a PLDM GetFileTable request message.
 * The file table contains the list of files available and
 * their attributes.
 *
 * Ex:
 * {
 *   "FileHandle": "11",
 *   "FileNameLength": 12,
 *   "FileName": "81e0066b.lid",
 *   "FileSize": 589824,
 *   "FileTraits": 6
 * },
 */
static int get_file_table_req(void)
{
	size_t data_size = PLDM_MSG_SIZE(struct pldm_get_file_table_req);
	size_t response_len, payload_len;
	uint8_t file_table_data_start_offset;
	uint8_t transfer_flag, completion_code;
	struct pldm_tx_data *tx = NULL;
	uint32_t next_transfer_handle;
	void *response_msg;
	int rc = OPAL_SUCCESS;

	struct pldm_get_file_table_req file_table_req = {
		.transfer_handle = 0, /* (0 if operation op is FIRSTPART) */
		.operation_flag = PLDM_GET_FIRSTPART,
		.table_type = PLDM_FILE_ATTRIBUTE_TABLE,
	};

	prlog(PR_DEBUG, "%s - GetFileReq\n", __func__);

	/* Encode the file table request */
	tx = zalloc(sizeof(struct pldm_tx_data) + data_size);
	if (!tx)
		return OPAL_NO_MEM;
	tx->data_size = data_size;

	rc = encode_get_file_table_req(
			DEFAULT_INSTANCE_ID,
			file_table_req.transfer_handle,
			file_table_req.operation_flag,
			file_table_req.table_type,
			(struct pldm_msg *)tx->data);
	if (rc != PLDM_SUCCESS) {
		prlog(PR_ERR, "Encode GetFileReq Error, rc: %d\n", rc);
		free(tx);
		return OPAL_PARAMETER;
	}

	/* Send and get the response message bytes */
	rc = pldm_requester_queue_and_wait(tx,
					   &response_msg, &response_len);
	if (rc) {
		prlog(PR_ERR, "Communication Error, req: GetFileReq, rc: %d\n", rc);
		free(tx);
		return rc;
	}

	/* Decode the message */
	payload_len = response_len - sizeof(struct pldm_msg_hdr);
	rc = decode_get_file_table_resp(
				response_msg,
				payload_len,
				&completion_code,
				&next_transfer_handle,
				&transfer_flag,
				&file_table_data_start_offset,
				&file_attr_length);
	if (rc != PLDM_SUCCESS || completion_code != PLDM_SUCCESS) {
		prlog(PR_ERR, "Decode GetFileResp Error, rc: %d, cc: %d\n",
			      rc, completion_code);
		rc = OPAL_PARAMETER;
		goto out;
	}

	/* we do not support multipart transfer */
	if ((next_transfer_handle != PLDM_GET_NEXTPART) ||
	    (transfer_flag != PLDM_START_AND_END)) {
		prlog(PR_ERR, "Transfert GetFileResp not complete, "
			      "transfer_hndl: %d, transfer_flag: %d\n",
			      next_transfer_handle,
			      transfer_flag);
	}

	file_attr_table = zalloc(file_attr_length);
	if (!file_attr_table) {
		rc = OPAL_NO_MEM;
		goto out;
	}

	memcpy(file_attr_table,
	       ((struct pldm_msg *)response_msg)->payload +
	       file_table_data_start_offset,
	       file_attr_length);

out:
	free(tx);
	free(response_msg);
	return rc;
}

int pldm_file_io_init(void)
{
	int rc;

	/* PLDM GetFileTable request */
	rc = get_file_table_req();
	if (rc)
		goto err;

	file_io_init_complete(true);
	prlog(PR_DEBUG, "%s - done\n", __func__);

	return OPAL_SUCCESS;

err:
	file_io_init_complete(false);
	return rc;
}
