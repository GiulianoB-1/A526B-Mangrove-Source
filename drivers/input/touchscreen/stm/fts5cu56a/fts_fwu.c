#include <linux/input/a52_touch_compat.h>
/* A52_PHASE446X_STM_FTS_READ_ONLY_V1 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>

#include "fts_ts.h"


#define FTS_DEFAULT_UMS_FW "/sdcard/Firmware/TSP/stm.fw"
#define FTS_DEFAULT_SPU_FW "/spu/TSP/ffu_tsp.fw"
#define FTSFILE_SIGNATURE 0xAA55AA55

enum {
	BUILT_IN = 0,
	UMS,
	NONE,
	SPU,
};

/**
 * @brief  Type definitions - header structure of firmware file (ftb)
 */

struct fts_header {
	u32	signature;
	u32	ftb_ver;
	u32	target;
	u32	fw_id;
	u32	fw_ver;
	u32	cfg_id;
	u32	cfg_ver;
	u8	fw_area_bs;	// bs : block size
	u8	panel_area_bs;
	u8	cx_area_bs;
	u8	cfg_area_bs;
	u32	reserved1;
	u32	ext_release_ver;
	u8	project_id;
	u8	ic_name;
	u8	module_ver;
	u8	reserved2;
	u32	sec0_size;
	u32	sec1_size;
	u32	sec2_size;
	u32	sec3_size;
	u32	hdr_crc;
} __packed;

#define FW_HEADER_SIZE  64

#define WRITE_CHUNK_SIZE 1024
/*
 * FTS9CU80F : 64K
 * FTS5CU56A : 32K
 */
#define DRAM_SIZE (32 * 1024) // 32kB

#define CODE_ADDR_START 0x00000000
#define CX_ADDR_START 0x00007000
#define CONFIG_ADDR_START 0x00007C00

#define	SIGNEDKEY_SIZE		(256)

int FTS_Check_DMA_StartAndDone(struct fts_ts_info *info)
{
	int timeout = 60;
	u8 regAdd[6] = { 0xFA, 0x20, 0x00, 0x00, 0x71, 0xC0 };
	u8 val[1];

	info->fts_write_reg(info, &regAdd[0], 6);
	fts_delay(10);

	do {
		info->fts_read_reg(info, &regAdd[0], 5, (u8 *)val, 1);

		if ((val[0] & 0x80) != 0x80)
			break;

		fts_delay(50);
		timeout--;
	} while (timeout != 0);

	if (timeout == 0) {
		input_err(true, &info->client->dev, "%s: Time Over\n", __func__);
		return -1;
	}

	return 0;
}

static __maybe_unused int FTS_Check_Erase_Done(struct fts_ts_info *info)
{
	int timeout = 60;  // 3 sec timeout
	u8 regAdd[5] = { 0xFA, 0x20, 0x00, 0x00, 0x6A };
	u8 val[1];

	do {
		info->fts_read_reg(info, &regAdd[0], 5, (u8 *)val, 1);

		if ((val[0] & 0x80) != 0x80)
			break;

		fts_delay(50);
		timeout--;
	} while (timeout != 0);

	if (timeout == 0) {
		input_err(true, &info->client->dev, "%s: Time Over\n", __func__);
		return -1;
	}

	return 0;
}

int fts_fw_fillFlash(struct fts_ts_info *info, u32 address, u8 *data, int size)
{
	pr_warn_ratelimited("P446X TOUCH FW_WRITE_BLOCKED function=%s (no IC flash writes on GKI)\n",__func__);
	return -EPERM;
}

static __maybe_unused int fts_fw_burn(struct fts_ts_info *info, u8 *fw_data)
{
	pr_warn_ratelimited("P446X TOUCH FW_WRITE_BLOCKED function=%s (no IC flash writes on GKI)\n",__func__);
	return -EPERM;
}

int fts_fw_wait_for_event(struct fts_ts_info *info, u8 *result, u8 result_cnt)
{
	int rc = 0;
	int i;
	bool matched = false;
	u8 regAdd;
	u8 data[FTS_EVENT_SIZE];
	int retry = 0;

	memset(data, 0x0, FTS_EVENT_SIZE);

	regAdd = FTS_READ_ONE_EVENT;
	rc = -1;
	while (info->fts_read_reg(info, &regAdd, 1, (u8 *)data, FTS_EVENT_SIZE)) {
		for (i = 0; i < result_cnt; i++) {
			if (data[i] != result[i]) {
				matched = false;
				break;
			}
			matched = true;
		}

		if (matched == true) {
			rc = 0;
			break;
		}

		if (data[0] == FTS_EVENT_ERROR_REPORT) {
			input_info(true, &info->client->dev, "%s: Error detected %02X,%02X,%02X,%02X,%02X,%02X\n",
					__func__, data[0], data[1], data[2], data[3], data[4], data[5]);
			break;
		}

		if (retry++ > FTS_RETRY_COUNT * 15) {
			rc = -1;
			input_err(true, &info->client->dev, "%s: Time Over (%02X,%02X,%02X,%02X,%02X,%02X)\n",
					__func__, data[0], data[1], data[2], data[3], data[4], data[5]);
			break;
		}
		fts_delay(20);
	}

	return rc;
}

int fts_fw_wait_for_echo_event(struct fts_ts_info *info, u8 *cmd, u8 cmd_cnt, int delay)
{
	int rc = 0;
	int i;
	bool matched = false;
	u8 regAdd;
	u8 data[FTS_EVENT_SIZE];
	int retry = 0;

	mutex_lock(&info->wait_for);

	rc = info->fts_write_reg(info, cmd, cmd_cnt);
	if (rc < 0) {
		input_err(true, &info->client->dev, "%s: failed to write command\n", __func__);
		mutex_unlock(&info->wait_for);
		return rc;
	}

	if (delay)
		fts_delay(delay);

	memset(data, 0x0, FTS_EVENT_SIZE);

	regAdd = FTS_READ_ONE_EVENT;
	rc = -1;
	while (info->fts_read_reg(info, &regAdd, 1, (u8 *)data, FTS_EVENT_SIZE)) {
		if (data[0] != 0x00)
			input_info(true, &info->client->dev,
					"%s: event %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X, %02X\n",
					__func__, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7],
					data[8], data[9], data[10], data[11], data[12], data[13], data[14], data[15]);

		if ((data[0] == FTS_EVENT_STATUS_REPORT) && (data[1] == 0x01)) {  // Check command ECHO
			int loop_cnt;

			if (cmd_cnt > 4)
				loop_cnt = 4;
			else
				loop_cnt = cmd_cnt;

			for (i = 0; i < loop_cnt; i++) {
				if (data[i + 2] != cmd[i]) {
					matched = false;
					break;
				}
				matched = true;
			}

			if (matched == true) {
				rc = 0;
				break;
			}
		} else if (data[0] == FTS_EVENT_ERROR_REPORT) {
			input_info(true, &info->client->dev, "%s: Error detected %02X,%02X,%02X,%02X,%02X,%02X\n",
				__func__, data[0], data[1], data[2], data[3], data[4], data[5]);

			if (retry >= FTS_RETRY_COUNT) 
				break;
		}

		if (retry++ > FTS_RETRY_COUNT * 50) {
			rc = -1;
			input_err(true, &info->client->dev, "%s: Time Over (%02X,%02X,%02X,%02X,%02X,%02X)\n",
				__func__, data[0], data[1], data[2], data[3], data[4], data[5]);
			break;
		}
		fts_delay(20);
	}

	mutex_unlock(&info->wait_for);

	return rc;
}

/* block on winner model / use other tablet models */
static void fts_set_factory_history_data(struct fts_ts_info *info, u8 level)
{
	int ret;
	u8 regaddr[3] = { 0 };
	u8 wlevel;

	switch (level) {
	case OFFSET_FAC_NOSAVE:
		input_info(true, &info->client->dev, "%s: not save to flash area\n", __func__);
		return;
	case OFFSET_FAC_SUB:
		wlevel = OFFSET_FW_SUB;
		break;
	case OFFSET_FAC_MAIN:
		wlevel = OFFSET_FW_MAIN;
		break;
	default:
		input_info(true, &info->client->dev, "%s: wrong level %d\n", __func__, level);
		return;
	}

	regaddr[0] = 0xC7;
	regaddr[1] = 0x04;
	regaddr[2] = wlevel;

	ret = info->fts_write_reg(info, regaddr, 3);
	if (ret < 0) {
		input_err(true, &info->client->dev,
				"%s: failed to write factory level %d\n", __func__, wlevel);
		return;
	}

	regaddr[0] = 0xA4;
	regaddr[1] = 0x05;
	regaddr[2] = 0x04; /* panel configuration area */
	ret = fts_fw_wait_for_echo_event(info, regaddr, 3, 200);
	if (ret < 0)
		return;

	input_info(true, &info->client->dev, "%s: save to flash area, level=%d\n", __func__, wlevel);
	return;
}

#ifdef TCLM_CONCEPT
int fts_tclm_execute_force_calibration(struct i2c_client *client, int cal_mode)
{
	struct fts_ts_info *info = (struct fts_ts_info *)i2c_get_clientdata(client);

	return fts_execute_autotune(info, true);
}
#endif

int fts_execute_autotune(struct fts_ts_info *info, bool IsSaving)
{
	u8 regAdd[FTS_EVENT_SIZE] = {0,};
	u8 DataType = 0x00;
	int rc;

	input_info(true, &info->client->dev, "%s: start\n", __func__);

	fts_set_scanmode(info, FTS_SCAN_MODE_SCAN_OFF);

	fts_delay(10);

	info->fts_command(info, FTS_CMD_CLEAR_ALL_EVENT, true);

	fts_interrupt_set(info, INT_DISABLE);

	// w A4 00 03
	if (IsSaving == true) {
		// full panel init
		regAdd[0] = 0xA4; regAdd[1] = 0x00; regAdd[2] = 0x03;

		rc = fts_fw_wait_for_echo_event(info, &regAdd[0], 3, 500);
#ifdef TCLM_CONCEPT
		if (info->tdata->nvdata.cal_fail_cnt == 0xFF)
			info->tdata->nvdata.cal_fail_cnt = 0;
		if (rc < 0) {
			info->tdata->nvdata.cal_fail_cnt++;
			info->tdata->nvdata.cal_fail_falg = 0;
		} else {
			info->tdata->nvdata.cal_fail_falg = SEC_CAL_PASS;
			info->is_cal_done = true;
		}
		info->tdata->tclm_write(info->tdata->client, SEC_TCLM_NVM_ALL_DATA);
		/* interrupt is enabled by tclm write */
		fts_interrupt_set(info, INT_DISABLE);
#endif
		if (rc < 0) {
			input_info(true, &info->client->dev, "%s: timeout\n", __func__);
			goto ERROR;
		}
	} else {
		// SS ATune
		//DataType = 0x0C;
		DataType = 0x3F;

		regAdd[0] = 0xA4; regAdd[1] = 0x03; regAdd[2] = (u8)DataType; regAdd[3] = 0x00;

		rc = fts_fw_wait_for_echo_event(info, &regAdd[0], 4, 500);
		if (rc < 0) {
			input_info(true, &info->client->dev, "%s: timeout\n", __func__);
			goto ERROR;
		}
	}

	/* block on winner model / use other tablet models */
	fts_set_factory_history_data(info, info->factory_position);
	if (IsSaving == true)
		fts_panel_ito_test(info, SAVE_MISCAL_REF_RAW);

ERROR:

	info->factory_position = OFFSET_FAC_NOSAVE;
	fts_interrupt_set(info, INT_ENABLE);

	fts_set_scanmode(info, info->scan_mode);

	fts_checking_miscal(info);	//temp

	return rc;
}

static __maybe_unused const int fts_fw_updater(struct fts_ts_info *info, u8 *fw_data)
{
	pr_warn_ratelimited("P446X TOUCH FW_WRITE_BLOCKED function=%s (no IC flash writes on GKI)\n",__func__);
	return -EPERM;
}

int fts_fw_update_on_probe(struct fts_ts_info *info)
{
	int retval = 0;
	const struct firmware *fw_entry = NULL;
	u8 *fw_data = NULL;
	char fw_path[FTS_MAX_FW_PATH];
	const struct fts_header *header;
#ifdef TCLM_CONCEPT
	bool restore_cal = false;
	int ret = 0;
#endif

#ifdef TCLM_CONCEPT
	if (info->tdata->support_tclm_test) {
		ret = sec_tclm_test_on_probe(info->tdata);
		if (ret < 0)
			input_info(true, &info->client->dev, "%s: SEC_TCLM_NVM_ALL_DATA i2c read fail", __func__);
	}
#endif

	if (info->board->bringup == 1)
		return 0;

	if (info->board->firmware_name) {
		info->firmware_name = info->board->firmware_name;
	} else {
		input_err(true, &info->client->dev, "%s: firmware name does not declair in dts\n", __func__);
		goto exit_fwload;
	}

	snprintf(fw_path, FTS_MAX_FW_PATH, "%s", info->firmware_name);
	input_info(true, &info->client->dev, "%s: Load firmware : %s, TSP_ID : %d\n", __func__, fw_path, info->board->tsp_id);

	retval = request_firmware(&fw_entry, fw_path, &info->client->dev);
	if (retval) {
		input_err(true, &info->client->dev,
				"%s: Firmware image %s not available\n", __func__,
				fw_path);
		pr_warn("P446X TOUCH BIN_MISSING name=%s ic_version=%04x: read-only bringup continues\n",
			fw_path,info->fw_version_of_ic);
		retval=FTS_NOT_ERROR;
		goto done;
	}

	fw_data = (u8 *)fw_entry->data;
	header = (struct fts_header *)fw_data;

	info->fw_version_of_bin = (u16)header->fw_ver;
	info->fw_main_version_of_bin = (u16)header->ext_release_ver;
	info->config_version_of_bin = (u16)header->cfg_ver;
	info->project_id_of_bin = header->project_id;
	info->ic_name_of_bin = header->ic_name;
	info->module_version_of_bin = header->module_ver;

	input_info(true, &info->client->dev,
			"%s: [BIN] Firmware Ver: 0x%04X, Config Ver: 0x%04X, Main Ver: 0x%04X\n", __func__,
			info->fw_version_of_bin,
			info->config_version_of_bin,
			info->fw_main_version_of_bin);
	input_info(true, &info->client->dev,
			"%s: [BIN] Project ID: 0x%02X, IC Name: 0x%02X, Module Ver: 0x%02X\n",
			__func__, info->project_id_of_bin,
			info->ic_name_of_bin, info->module_version_of_bin);

	if (info->board->bringup == 2) {
		input_err(true, &info->client->dev, "%s: skip fw_update for bringup\n", __func__);
		retval = FTS_NOT_ERROR;
		goto done;
	}

	if (info->checksum_result)
		retval = FTS_NEED_FW_UPDATE;
	else if (info->ic_name_of_ic == 0xFF && info->project_id_of_ic == 0xFF && info->module_version_of_ic == 0xFF)
		retval = FTS_NEED_FW_UPDATE;
	else if (info->ic_name_of_ic != info->ic_name_of_bin)
		retval = FTS_NOT_UPDATE;
	else if (info->project_id_of_ic != info->project_id_of_bin)
		retval = FTS_NEED_FW_UPDATE;
	else if (info->module_version_of_ic != info->module_version_of_bin)
		retval = FTS_NOT_UPDATE;
	else if ((info->fw_main_version_of_ic < info->fw_main_version_of_bin)
			|| ((info->config_version_of_ic < info->config_version_of_bin))
			|| ((info->fw_version_of_ic < info->fw_version_of_bin)))
		retval = FTS_NEED_FW_UPDATE;
	else
		retval = FTS_NOT_ERROR;

	/* ic fw ver > bin fw ver && force is false */
	if (retval != FTS_NEED_FW_UPDATE) {
		input_err(true, &info->client->dev, "%s: skip fw update\n", __func__);

		goto done;
	}

	pr_info("P446X TOUCH FW_VERSIONS ic=%04x bin=%04x ic_main=%04x bin_main=%04x fw_write=BLOCKED\n",
		info->fw_version_of_ic,info->fw_version_of_bin,
		info->fw_main_version_of_ic,info->fw_main_version_of_bin);
	retval=FTS_NOT_ERROR;
	goto done; /* Firmware always read-only on GKI. */
	if (retval < 0)
		goto done;

#ifdef TCLM_CONCEPT
	ret = info->tdata->tclm_read(info->tdata->client, SEC_TCLM_NVM_ALL_DATA);
	if (ret < 0) {
		input_info(true, &info->client->dev, "%s: SEC_TCLM_NVM_ALL_DATA i2c read fail", __func__);
		goto done;
	}

	input_info(true, &info->client->dev, "%s: tune_fix_ver [%04X] afe_base [%04X]\n",
		__func__, info->tdata->nvdata.tune_fix_ver, info->tdata->afe_base);

	if ((info->tdata->tclm_level > TCLM_LEVEL_CLEAR_NV) &&
		((info->tdata->nvdata.tune_fix_ver == 0xffff)
		|| (info->tdata->afe_base > info->tdata->nvdata.tune_fix_ver))) {
		/* tune version up case */
		sec_tclm_root_of_cal(info->tdata, CALPOSITION_TUNEUP);
		restore_cal = true;
	} else if (info->tdata->tclm_level == TCLM_LEVEL_CLEAR_NV) {
		/* firmup case */
		sec_tclm_root_of_cal(info->tdata, CALPOSITION_FIRMUP);
		restore_cal = true;
	}

	if (restore_cal) {
		input_info(true, &info->client->dev, "%s: RUN OFFSET CALIBRATION\n", __func__);
		if (sec_execute_tclm_package(info->tdata, 0) < 0)
			input_err(true, &info->client->dev, "%s: sec_execute_tclm_package fail\n", __func__);
	}
#endif

done:
#ifdef TCLM_CONCEPT
	sec_tclm_root_of_cal(info->tdata, CALPOSITION_NONE);
#endif
	if (fw_entry)
		release_firmware(fw_entry);
exit_fwload:
	return retval;
}
EXPORT_SYMBOL(fts_fw_update_on_probe);

static __maybe_unused int fts_load_fw_from_kernel(struct fts_ts_info *info,
		const char *fw_path)
{
	pr_warn_ratelimited("P446X TOUCH FW_WRITE_BLOCKED function=%s (no IC flash writes on GKI)\n",__func__);
	return -EPERM;
}

static __maybe_unused int fts_load_fw_from_ums(struct fts_ts_info *info, int type)
{
	pr_warn_ratelimited("P446X TOUCH FW_WRITE_BLOCKED function=%s (no IC flash writes on GKI)\n",__func__);
	return -EPERM;
}

int fts_fw_update_on_hidden_menu(struct fts_ts_info *info, int update_type)
{
	pr_warn_ratelimited("P446X TOUCH FW_WRITE_BLOCKED function=%s (no IC flash writes on GKI)\n",__func__);
	return -EPERM;
}
EXPORT_SYMBOL(fts_fw_update_on_hidden_menu);
