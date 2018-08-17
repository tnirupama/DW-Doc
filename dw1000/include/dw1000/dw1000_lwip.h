/*
 * Copyright 2018, Decawave Limited, All Rights Reserved
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * @file dw1000_lwip.h
 * @author paul kettle
 * @date 2018
 * 
 * @brief lwip service
 * @details This is the lwip base class which utilizes the functions to do the configurations related to lwip layer based on dependencies.
 *
 */

#ifndef _DW1000_LWIP_H_
#define _DW1000_LWIP_H_

#include <stdlib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <hal/hal_spi.h>
#include <dw1000/dw1000_regs.h>
#include <dw1000/dw1000_dev.h>
#include <dw1000/dw1000_ftypes.h>
#include <dw1000/dw1000_phy.h>
#include <lwip/pbuf.h>
#include <lwip/ip_addr.h>

//! Lwip config data
typedef struct _dw1000_lwip_config_t{
   uint16_t poll_resp_delay;    //!< Delay between frames, in UWB microseconds.
   uint16_t resp_timeout;       //!< Receive response timeout, in UWB microseconds.
   uint32_t uwbtime_to_systime; //!< UWB time to system time
}dw1000_lwip_config_t;

//! Lwip modes based on wait for transmit
typedef enum _dw1000_lwip_modes_t{
    LWIP_BLOCKING,              //!< lwip blocking mode
    LWIP_NONBLOCKING            //!< lwip non-blocking mode
}dw1000_lwip_modes_t;

//! Status of lwip instance
typedef struct _dw1000_lwip_status_t{
    uint32_t selfmalloc:1;             //!< Internal flag for memory garbage collection 
    uint32_t initialized:1;            //!< Instance allocated 
    uint32_t start_tx_error:1;         //!< Set for start transmit error 
    uint32_t start_rx_error:1;         //!< Set for start receive error 
    uint32_t tx_frame_error:1;         //!< Set transmit frame error
    uint32_t rx_error:1;               //!< Set for receive error
    uint32_t rx_timeout_error:1;       //!< Set for receive timeout error 
    uint32_t request_timeout:1;        //!< Set for request timeout
}dw1000_lwip_status_t;

//! Attributes of lwip instance
typedef struct _dw1000_lwip_instance_t{
    struct _dw1000_dev_instance_t * dev;   //!< Structure for DW1000 instance 
    struct os_sem sem;                     //!< Structure for OS semaphores
    struct os_sem data_sem;                //!< Structure for data of semaphores
    struct _ieee_std_frame_t * tx_frame;   //!< Structure of transmit frame
    struct _ieee_std_frame_t * rx_frame;   //!< Structure of receive frame
    dw1000_lwip_config_t * config;         //!< lwip config parameters 
    dw1000_lwip_status_t status;           //!< lwip status
    uint16_t nframes;                      //!< Number of buffers defined to store the lwip data  
    uint16_t buf_idx;                      //!< Indicates number of buffer instances for the chosen bsp 
    uint16_t buf_len;                      //!< Indicates buffer length 
    struct netif * netif;                  //!< Network interface 
    char * data_buf[];                     //!< Data buffers 
}dw1000_lwip_instance_t;

//! lwip callback 
typedef struct _dw1000_lwip_cb_t{
   void (*recv)(dw1000_dev_instance_t * inst, uint16_t timeout);  //!< Keep tracks of lwip tx/rx status
}dw1000_lwip_cb_t;

//! lwip context based on callback 
typedef struct _dw1000_lwip_context_t{
   dw1000_lwip_cb_t rx_cb;    //!< DW1000 lwip receive callback
}dw1000_lwip_context_t;

dw1000_lwip_config_t *
dw1000_config(dw1000_dev_instance_t * inst);

dw1000_lwip_instance_t *
dw1000_lwip_init(dw1000_dev_instance_t * inst, dw1000_lwip_config_t * config, uint16_t nframes, uint16_t buf_len);

void
dw1000_lwip_free(dw1000_lwip_instance_t * inst);

void
dw1000_lwip_set_callbacks(dw1000_dev_instance_t * inst, dw1000_dev_cb_t lwip_tx_complete_cb, dw1000_dev_cb_t lwip_rx_complete_cb,  dw1000_dev_cb_t lwip_timeout_cb,  dw1000_dev_cb_t lwip_error_cb);

dw1000_dev_status_t
dw1000_lwip_write(dw1000_dev_instance_t * inst, struct pbuf *p, dw1000_lwip_modes_t code);

void
dw1000_low_level_init( dw1000_dev_instance_t * inst, 
			dw1000_dev_txrf_config_t * txrf_config,
			dw1000_dev_config_t * mac_config);

void
dw1000_netif_config( dw1000_dev_instance_t * inst, struct netif *netif, ip_addr_t *my_ip_addr, bool rx_status);

err_t
dw1000_netif_init( struct netif * dw1000_netif);

err_t
dw1000_ll_output(struct netif * dw1000_netif, struct pbuf *p);


err_t
dw1000_ll_input(struct pbuf *p, struct netif *dw1000_netif);

void
dw1000_lwip_start_rx(dw1000_dev_instance_t * inst, uint16_t timeout);

void print_error(err_t error);

#ifdef __cplusplus
}
#endif
#endif /* _DW1000_LWIP_H_ */
