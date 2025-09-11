/**
  ******************************************************************************
  * @file   main.c
  * @author Sifli software development team
  ******************************************************************************
*/
/**
 * @attention
 * Copyright (c) 2025- 2025,  Sifli Technology
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form, except as embedded into a Sifli integrated circuit
 *    in a product or a software update for such product, must reproduce the above
 *    copyright notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of Sifli nor the names of its contributors may be used to endorse
 *    or promote products derived from this software without specific prior written permission.
 *
 * 4. This software, with or without modification, must only be used with a
 *    Sifli integrated circuit.
 *
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 *
 * THIS SOFTWARE IS PROVIDED BY SIFLI TECHNOLOGY "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SIFLI TECHNOLOGY OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <string.h>
#include <stdlib.h>

#include "bf0_ble_gap.h"
#include "bf0_sibles.h"
#include "bf0_sibles_internal.h"
#include "bf0_sibles_advertising.h"
#include "ble_connection_manager.h"


#define LOG_TAG "ble_app"
#include "log.h"

#ifdef BT_FINSH
    #include "bts2_msg.h"
    #include "gap_api.h"
    #include "sc_api.h"
    #include "bts2_app_interface.h"
#endif

#ifdef BSP_BLE_BASC
    #include "bf0_ble_basc.h"
#endif

enum ble_app_att_list
{
    BLE_APP_SVC,
    BLE_APP_CHAR,
    BLE_APP_CHAR_VALUE,
    BLE_APP_CLIENT_CHAR_CONFIG_DESCRIPTOR,
    BLE_APP_ATT_NB
};


#define app_svc_uuid { \
    0x73, 0x69, 0x66, 0x6c, \
    0x69, 0x5f, 0x61, 0x70, \
    0x70, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00 \
};

#define app_chara_uuid { \
    0x73, 0x69, 0x66, 0x6c, \
    0x69, 0x5f, 0x61, 0x70, \
    0x70, 0x01, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00 \
}

#define SERIAL_UUID_16(x) {((uint8_t)(x&0xff)),((uint8_t)(x>>8))}
/* 24 * 1.25 = 30ms */
#define BLE_APP_HIGH_PERFORMANCE_INTERVAL (24)
#define BLE_APP_TIMEOUT_INTERVAL (5000)

typedef struct
{
    uint8_t is_power_on;
    uint8_t conn_idx;
    uint8_t is_bg_adv_on;
    struct
    {
        bd_addr_t peer_addr;
        uint16_t mtu;
        uint16_t conn_interval;
        uint8_t peer_addr_type;
    } conn_para;
    struct
    {
        sibles_hdl srv_handle;
        uint32_t data;
        uint8_t is_config_on;
    } data;
    rt_timer_t time_handle;
    rt_timer_t write_time_handle;
    rt_mailbox_t mb_handle;
} app_env_t;

static app_env_t g_app_env;
static rt_mailbox_t g_app_mb;

static uint8_t g_app_svc[ATT_UUID_128_LEN] = app_svc_uuid;



struct attm_desc_128 app_att_db[] =
{
    // Service declaration
    [BLE_APP_SVC] = {SERIAL_UUID_16(ATT_DECL_PRIMARY_SERVICE), PERM(RD, ENABLE), 0, 0},
    // Characteristic  declaration
    [BLE_APP_CHAR] = {SERIAL_UUID_16(ATT_DECL_CHARACTERISTIC), PERM(RD, ENABLE), 0, 0},
    // Characteristic value config
    [BLE_APP_CHAR_VALUE] = {
        /* The permissions are for: 1.Allowed read, write req, write command and notification.
                                    2.Write requires Unauthenticated link
           The ext_perm are for: 1. Support 128bit UUID. 2. Read will involve callback. */
        app_chara_uuid, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE) | PERM(NTF, ENABLE), PERM(UUID_LEN, UUID_128) | PERM(RI, ENABLE), 1024
    },
    // Descriptor config
    [BLE_APP_CLIENT_CHAR_CONFIG_DESCRIPTOR] = {
        SERIAL_UUID_16(ATT_DESC_CLIENT_CHAR_CFG), PERM(RD, ENABLE) | PERM(WRITE_REQ,
                ENABLE), PERM(RI, ENABLE), 2
    },

};


static app_env_t *ble_app_get_env(void)
{
    return &g_app_env;
}

SIBLES_ADVERTISING_CONTEXT_DECLAR(g_app_advertising_context);

static uint8_t ble_app_advertising_event(uint8_t event, void *context, void *data)
{
    app_env_t *env = ble_app_get_env();

    switch (event)
    {
    case SIBLES_ADV_EVT_ADV_STARTED:
    {
        sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data;
        LOG_I("ADV start resutl %d, mode %d\r\n", evt->status, evt->adv_mode);
        break;
    }
    case SIBLES_ADV_EVT_ADV_STOPPED:
    {
        sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
        LOG_I("ADV stopped reason %d, mode %d\r\n", evt->reason, evt->adv_mode);
        break;
    }
    default:
        break;
    }
    return 0;
}


#define DEFAULT_LOCAL_NAME "SIFLI_APP"
#define EXAMPLE_LOCAL_NAME "SIFLI_EXAMPLE"
/* Enable advertise via advertising service. */
static void ble_app_advertising_start(void)
{
    sibles_advertising_para_t para = {0};
    uint8_t ret;

    char local_name[31] = {0};
    uint8_t manu_additnal_data[] = {0x20, 0xC4, 0x00, 0x91};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    bd_addr_t addr;
    ret = ble_get_public_address(&addr);
    if (ret == HL_ERR_NO_ERROR)
        rt_snprintf(local_name, 31, "SIFLI_APP-%x-%x-%x-%x-%x-%x", addr.addr[0], addr.addr[1], addr.addr[2], addr.addr[3], addr.addr[4], addr.addr[5]);
    else
        memcpy(local_name, DEFAULT_LOCAL_NAME, sizeof(DEFAULT_LOCAL_NAME));

    ble_gap_dev_name_t *dev_name = malloc(sizeof(ble_gap_dev_name_t) + strlen(local_name));
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    free(dev_name);

    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    /* Keep advertising till disable it or connected. */
    para.config.mode_config.conn_config.duration = 0x0;
    para.config.mode_config.conn_config.interval = 0x30;
    para.config.max_tx_pwr = 0x7F;
    /* Advertising will re-start after disconnected. */
    para.config.is_auto_restart = 1;
    /* Scan rsp data is same as advertising data. */
    //para.config.is_rsp_data_duplicate = 1;

    /* Prepare name filed. Due to name is too long to put adv data, put it to rsp data.*/
    para.rsp_data.completed_name = rt_malloc(rt_strlen(local_name) + sizeof(sibles_adv_type_name_t));
    para.rsp_data.completed_name->name_len = rt_strlen(local_name);
    rt_memcpy(para.rsp_data.completed_name->name, local_name, para.rsp_data.completed_name->name_len);

    /* Prepare manufacturer filed .*/
    para.adv_data.manufacturer_data = rt_malloc(sizeof(sibles_adv_type_manufacturer_data_t) + sizeof(manu_additnal_data));
    para.adv_data.manufacturer_data->company_id = manu_company_id;
    para.adv_data.manufacturer_data->data_len = sizeof(manu_additnal_data);
    rt_memcpy(para.adv_data.manufacturer_data->additional_data, manu_additnal_data, sizeof(manu_additnal_data));

    para.evt_handler = ble_app_advertising_event;

    ret = sibles_advertising_init(g_app_advertising_context, &para);
    if (ret == SIBLES_ADV_NO_ERR)
    {
        sibles_advertising_start(g_app_advertising_context);
    }

    rt_free(para.rsp_data.completed_name);
    rt_free(para.adv_data.manufacturer_data);
}

static void update_adv_content()
{
    sibles_advertising_para_t para = {0};

    char temp_name[31] = {0};
    memcpy(temp_name, EXAMPLE_LOCAL_NAME, sizeof(EXAMPLE_LOCAL_NAME));

    para.rsp_data.completed_name = rt_malloc(rt_strlen(temp_name) + sizeof(sibles_adv_type_name_t));
    para.rsp_data.completed_name->name_len = rt_strlen(temp_name);
    rt_memcpy(para.rsp_data.completed_name->name, temp_name, para.rsp_data.completed_name->name_len);

    uint8_t manu_additnal_data[] = {0x23, 0x33, 0x33, 0x33};
    uint16_t manu_company_id = SIG_SIFLI_COMPANY_ID;
    para.adv_data.manufacturer_data = rt_malloc(sizeof(sibles_adv_type_manufacturer_data_t) + sizeof(manu_additnal_data));
    para.adv_data.manufacturer_data->company_id = manu_company_id;
    para.adv_data.manufacturer_data->data_len = sizeof(manu_additnal_data);
    rt_memcpy(para.adv_data.manufacturer_data->additional_data, manu_additnal_data, sizeof(manu_additnal_data));

    uint8_t ret = sibles_advertising_update_adv_and_scan_rsp_data(g_app_advertising_context, &para.adv_data, &para.rsp_data);
    LOG_I("update adv %d", ret);

    rt_free(para.rsp_data.completed_name);
    rt_free(para.adv_data.manufacturer_data);
}


// Hanlde read operation
uint8_t *ble_app_gatts_get_cbk(uint8_t conn_idx, uint8_t idx, uint16_t *len)
{
    uint8_t *ret_val = NULL;
    app_env_t *env = ble_app_get_env();
    *len = 0;
    switch (idx)
    {
    case BLE_APP_CHAR_VALUE:
    {
        // Prepare data to remote device
        ret_val = (uint8_t *)&env->data.data;
        *len = 4;
        break;
    }
    default:
        break;
    }
    return ret_val;
}

// Hanlde read operation
uint8_t ble_app_gatts_set_cbk(uint8_t conn_idx, sibles_set_cbk_t *para)
{
    app_env_t *env = ble_app_get_env();
    switch (para->idx)
    {
    case BLE_APP_CHAR_VALUE:
    {
        // Store value that remote device writes
        uint32_t old_val = env->data.data;
        LOG_HEX("BLE RX", 16, para->value, para->len);
        if (para->len <= 4)
            memcpy(&env->data.data, para->value, para->len);
        LOG_I("Updated app value from:%d to:%d", old_val, env->data.data);

        break;
    }
    case BLE_APP_CLIENT_CHAR_CONFIG_DESCRIPTOR:
    {
        env->data.is_config_on = *(para->value);
        LOG_I("CCCD %d", env->data.is_config_on);
        break;
    }
    default:
        break;
    }
    return 0;
}

static void ble_app_service_init(void)
{
    app_env_t *env = ble_app_get_env();
    sibles_register_svc_128_t svc;

    svc.att_db = (struct attm_desc_128 *)&app_att_db;
    svc.num_entry = BLE_APP_ATT_NB;
    /* Service security level to control all characteristic. */
    svc.sec_lvl = PERM(SVC_AUTH, NO_AUTH) | PERM(SVC_UUID_LEN, UUID_128);
    svc.uuid = g_app_svc;
    /* Reigster GATT service and related callback for next response. */
    env->data.srv_handle = sibles_register_svc_128(&svc);
    if (env->data.srv_handle)
        sibles_register_cbk(env->data.srv_handle, ble_app_gatts_get_cbk, ble_app_gatts_set_cbk);
}


#ifndef NVDS_AUTO_UPDATE_MAC_ADDRESS_ENABLE
ble_common_update_type_t ble_request_public_address(bd_addr_t *addr)
{
    int ret = bt_mac_addr_generate_via_uid_v2(addr);

    if (ret != 0)
    {
        LOG_W("generate mac addres failed %d", ret);
        return BLE_UPDATE_NO_UPDATE;
    }

    return BLE_UPDATE_ONCE;
}
#endif // NVDS_AUTO_UPDATE_MAC_ADDRESS_ENABLE

#ifdef BSP_BLE_TIMES

ble_tips_time_env_t app_init_time_env =
{
    //current time
    .cur_time.date_time = {2024, 10, 14, 10, 10, 9}, //2024/10/14  10:10:9
    .cur_time.day_of_week = 1,//Monday
    .cur_time.fraction_256 = 2,
    .cur_time.adjust_reason = 3,
    //local time info
    .local_time_inf.time_zone = 32,//beijing time zone
    .local_time_inf.dst_offset = 0,//standard time
    //reference time info
    .ref_time_inf.time_source = 4,//Manual
    .ref_time_inf.time_accuracy = 0,
    .ref_time_inf.days_update = 1,
    .ref_time_inf.hours_update = 2,
};

uint8_t *tips_app_callback(uint8_t conn_idx, uint8_t event, uint8_t *value)
{
    uint8_t *ret = NULL;
    switch (event)
    {
    case BLE_TIPS_GET_CURRENT_TIME:
    {
        time_t now;
        now = time(RT_NULL);
        LOG_I("%s\n", ctime(&now));
        struct tm *time_local = localtime(&now);
        LOG_I("TIME YEAR %d MON %d DAY %d HOUR %d MIN %d SEC %d WDAY %d", time_local->tm_year + 1900, time_local->tm_mon + 1, time_local->tm_mday,
              time_local->tm_hour, time_local->tm_min, time_local->tm_sec, time_local->tm_wday);

        app_init_time_env.cur_time.date_time.year = time_local->tm_year + 1900;
        app_init_time_env.cur_time.date_time.month = time_local->tm_mon + 1;
        app_init_time_env.cur_time.date_time.day = time_local->tm_mday;
        app_init_time_env.cur_time.date_time.hour = time_local->tm_hour;
        app_init_time_env.cur_time.date_time.min = time_local->tm_min;
        app_init_time_env.cur_time.date_time.sec = time_local->tm_sec;
        app_init_time_env.cur_time.day_of_week = time_local->tm_wday;

        ret = (uint8_t *)&app_init_time_env.cur_time;
        break;
    }
    case BLE_TIPS_GET_LOCAL_TIME_INFO:
    {
        ret = (uint8_t *)&app_init_time_env.local_time_inf;
        break;
    }
    case BLE_TIPS_GET_REF_TIME_INFO:
    {
        ret = (uint8_t *)&app_init_time_env.ref_time_inf;
        break;
    }
    case BLE_TIPS_SET_CURRENT_TIME:
    {
        ble_tips_cur_time_t *cur_time = (ble_tips_cur_time_t *)value;

        // update rtc
#ifdef RT_USING_RTC
        set_date((uint32_t)cur_time->date_time.year, (uint32_t)cur_time->date_time.month, (uint32_t)cur_time->date_time.day);
        set_time((uint32_t)cur_time->date_time.hour, (uint32_t)cur_time->date_time.min, (uint32_t)cur_time->date_time.sec);
#endif
        app_init_time_env.cur_time = *cur_time;
        break;
    }
    case BLE_TIPS_SET_LOCAL_TIME_INFO:
    {
        ble_tips_local_time_info_t *local_time_info = (ble_tips_local_time_info_t *)value;
        app_init_time_env.local_time_inf = *local_time_info;
        break;
    }
    default:
        break;
    }
    LOG_I("tips callback type %d, ret %d\r\n", event, ret);
    return ret;
}

#endif

#ifdef BSP_BLE_DISS
uint8_t *dis_app_callback(uint8_t conn_idx, uint8_t event, uint16_t *len)
{
    uint8_t *data = NULL;
    *len = 0;
    switch (event)
    {
    case BLE_DIS_GET_MANU_NAME:
    {
        // Set information
        char *ch = "Sifli-Tech";
        *len = 10;
        data = (uint8_t *)ch;
        break;
    }
    case BLE_DIS_GET_MODEL_NB:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_SYS_ID:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_PNP_ID:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_SERI_NB:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_HW_REV:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_FW_REV:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_SW_REV:
    {
        // Set information
        break;
    }
    case BLE_DIS_GET_IEEE_DATA:
    {
        // Set information
        break;
    }
    default:
        break;
    }
    return data;
}
#endif

int main(void)
{
    int count = 0;
    app_env_t *env = ble_app_get_env();
    env->mb_handle = rt_mb_create("app", 8, RT_IPC_FLAG_FIFO);
    sifli_ble_enable();
#if defined(BT_FINSH) && defined(SF32LB52X_58)
    bt_interface_acl_accept_role_set(0);
    bt_interface_set_linkpolicy(1, 1);
#endif

#ifdef BSP_BLE_BASC
    ble_basc_init(true);
#endif
    while (1)
    {
        uint32_t value;
        int ret;
        rt_mb_recv(env->mb_handle, (rt_uint32_t *)&value, RT_WAITING_FOREVER);
        if (value == BLE_POWER_ON_IND)
        {
            env->is_power_on = 1;
            env->conn_para.mtu = 23; /* Default value. */
            ble_app_service_init();
#ifdef BSP_BLE_TIMES
            ble_tips_init(tips_app_callback, app_init_time_env);
#endif

#ifdef SF32LB55X
#ifdef BSP_BLE_DISS
            sibles_ble_diss_init(dis_app_callback);
#endif
#endif
            /* First enable connectable adv then enable non-connectable. */
            ble_app_advertising_start();
            LOG_I("receive BLE power on!\r\n");
        }
    }
    return RT_EOK;
}


static void ble_app_update_conn_param(uint8_t conn_idx, uint16_t inv_max, uint16_t inv_min, uint16_t timeout)
{
    ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(conn_idx, inv_min, inv_max, 0, timeout));
}

int ble_app_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    app_env_t *env = ble_app_get_env();
    switch (event_id)
    {
    case BLE_POWER_ON_IND:
    {
        /* Handle in own thread to avoid conflict */
        if (env->mb_handle)
            rt_mb_send(env->mb_handle, BLE_POWER_ON_IND);
        break;
    }
    case BLE_GAP_CONNECTED_IND:
    {
        ble_gap_connect_ind_t *ind = (ble_gap_connect_ind_t *)data;
        env->conn_idx = ind->conn_idx;
        env->conn_para.conn_interval = ind->con_interval;
        env->conn_para.peer_addr_type = ind->peer_addr_type;
        env->conn_para.peer_addr = ind->peer_addr;
        if (ind->role == 0)
            LOG_E("Peripheral should be slave!!!");

        LOG_I("Peer device(%x-%x-%x-%x-%x-%x) connected", env->conn_para.peer_addr.addr[5],
              env->conn_para.peer_addr.addr[4],
              env->conn_para.peer_addr.addr[3],
              env->conn_para.peer_addr.addr[2],
              env->conn_para.peer_addr.addr[1],
              env->conn_para.peer_addr.addr[0]);
        break;
    }
    case BLE_GAP_UPDATE_CONN_PARAM_IND:
    {
        ble_gap_update_conn_param_ind_t *ind = (ble_gap_update_conn_param_ind_t *)data;
        env->conn_para.conn_interval = ind->con_interval;
        LOG_I("Updated connection interval :%d", ind->con_interval);
        break;
    }
    case SIBLES_MTU_EXCHANGE_IND:
    {
        /* Negotiated MTU. */
        sibles_mtu_exchange_ind_t *ind = (sibles_mtu_exchange_ind_t *)data;
        env->conn_para.mtu = ind->mtu;
        LOG_I("Exchanged MTU size: %d", ind->mtu);
        break;
    }
    case BLE_GAP_DISCONNECTED_IND:
    {
        ble_gap_disconnected_ind_t *ind = (ble_gap_disconnected_ind_t *)data;
        LOG_I("BLE_GAP_DISCONNECTED_IND(%d)", ind->reason);
        break;
    }
    case SIBLES_WRITE_VALUE_RSP:
    {
        sibles_write_value_rsp_t *rsp = (sibles_write_value_rsp_t *)data;
        LOG_I("SIBLES_WRITE_VALUE_RSP %d", rsp->result);
        break;
    }
    case SIBLES_DIS_SET_VAL_RSP:
    {
        sibles_set_dis_rsp_t *rsp = (sibles_set_dis_rsp_t *)data;
        LOG_I("SIBLES_DIS_SET_VAL_RSP %d ,%d", rsp->value, rsp->status);
        break;
    }
    default:
        break;
    }
    return 0;
}
BLE_EVENT_REGISTER(ble_app_event_handler, NULL);

uint8_t ble_app_dis_enable()
{
    return 1;
}

int cmd_diss(int argc, char *argv[])
{
    app_env_t *env = ble_app_get_env();

    if (argc > 1)
    {
        if (strcmp(argv[1], "trc") == 0)
        {
            if (strcmp(argv[2], "mode") == 0)
            {
                uint8_t mode = atoi(argv[3]);
                uint32_t mask = atoi(argv[4]);
                sibles_set_trc_cfg(mode, mask);
            }
        }
        else if (0 == strcmp(argv[1], "conn"))
        {
            uint32_t interval;
            app_env_t *env = ble_app_get_env();

            interval = atoi(argv[2]);
            // value = argv * 1.25
            interval = interval * 100 / 125;
            ble_app_update_conn_param(env->conn_idx, interval, interval, 500);
        }
        else if (strcmp(argv[1], "adv_start") == 0)
        {
            sibles_advertising_start(g_app_advertising_context);
        }
        else if (strcmp(argv[1], "adv_stop") == 0)
        {
            sibles_advertising_stop(g_app_advertising_context);
        }
        else if (strcmp(argv[1], "adv_update") == 0)
        {
            // in this example, directly update main adv content will get memory capacity exceeded
            // run "cmd_diss bg_adv_del" delete background adv before update main adv.
            update_adv_content();
        }
        else if (strcmp(argv[1], "gen_addr") == 0)
        {
            bd_addr_t addr;
            int ret;
            ret = bt_mac_addr_generate_via_uid_v2(&addr);
            LOG_D("ret %d", ret);
        }
        else if (strcmp(argv[1], "manu") == 0)
        {
            LOG_I("set manu");
            uint8_t set_len = 5;
            char *manu_name = "sifli";
            uint8_t *manu_data = malloc(set_len);
            sibles_set_dis_t dis_param;
            memcpy(manu_data, manu_name, set_len);
            dis_param.value = DIS_MANUFACTURER_NAME_CHAR;
            dis_param.len = set_len;
            dis_param.data = manu_data;

            sibles_set_dev_info(&dis_param);
            free(manu_data);
        }
    }

    return 0;
}

#ifdef RT_USING_FINSH
    MSH_CMD_EXPORT(cmd_diss, My device information service.);
#endif

#ifdef SF32LB52X_58
uint16_t g_em_offset[HAL_LCPU_CONFIG_EM_BUF_MAX_NUM] =
{
    0x178, 0x178, 0x740, 0x7A0, 0x810, 0x880, 0xA00, 0xBB0, 0xD48,
    0x133C, 0x13A4, 0x19BC, 0x21BC, 0x21BC, 0x21BC, 0x21BC, 0x21BC, 0x21BC,
    0x21BC, 0x21BC, 0x263C, 0x265C, 0x2734, 0x2784, 0x28D4, 0x28E8, 0x28FC,
    0x29EC, 0x29FC, 0x2BBC, 0x2BD8, 0x3BE8, 0x5804, 0x5804, 0x5804
};

void lcpu_rom_config(void)
{
    hal_lcpu_bluetooth_em_config_t em_offset;
    memcpy((void *)em_offset.em_buf, (void *)g_em_offset, HAL_LCPU_CONFIG_EM_BUF_MAX_NUM * 2);
    em_offset.is_valid = 1;
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_EM_BUF, &em_offset, sizeof(hal_lcpu_bluetooth_em_config_t));

    hal_lcpu_bluetooth_act_configt_t act_cfg;
    act_cfg.ble_max_act = 6;
    act_cfg.ble_max_iso = 0;
    act_cfg.ble_max_ral = 3;
    act_cfg.bt_max_acl = 7;
    act_cfg.bt_max_sco = 0;
    act_cfg.bit_valid = CO_BIT(0) | CO_BIT(1) | CO_BIT(2) | CO_BIT(3) | CO_BIT(4);
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_ACT_CFG, &act_cfg, sizeof(hal_lcpu_bluetooth_act_configt_t));
}
#endif


/************************ (C) COPYRIGHT Sifli Technology *******END OF FILE****/

