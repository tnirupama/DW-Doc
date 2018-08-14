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
 * @file dw1000_rng.c
 * @athor paul kettle
 * @date 2018
 * @brief Range 
 *
 * @details This is the rng base class which utilises the functions to enable/disable the configurations related to rng
 *
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <os/os.h>
#include <hal/hal_spi.h>
#include <hal/hal_gpio.h>
#include "bsp/bsp.h"

#include <dw1000/dw1000_regs.h>
#include <dw1000/dw1000_dev.h>
#include <dw1000/dw1000_hal.h>
#include <dw1000/dw1000_mac.h>
#include <dw1000/dw1000_phy.h>
#include <dw1000/dw1000_ftypes.h>
#include <dw1000/dw1000_rng.h>
#if MYNEWT_VAL(DW1000_PROVISION)
#include <dw1000/dw1000_provision.h>
#endif
#if MYNEWT_VAL(DW1000_RANGE)
#include <dw1000/dw1000_range.h>
#endif

#include <dsp/polyval.h>


/*
% From APS011 Table 2 
rls = [-61,-63,-65,-67,-69,-71,-73,-75,-77,-79,-81,-83,-85,-87,-89,-91,-93];
bias = [-11,-10.4,-10.0,-9.3,-8.2,-6.9,-5.1,-2.7,0,2.1,3.5,4.2,4.9,6.2,7.1,7.6,8.1]./100;
p=polyfit(rls,bias,3) 
mat2c(p,'rng_bias_poly_PRF64')
bias = [-19.8,-18.7,-17.9,-16.3,-14.3,-12.7,-10.9,-8.4,-5.9,-3.1,0,3.6,6.5,8.4,9.7,10.6,11.0]./100;
p=polyfit(rls,bias,3) 
mat2c(p,'rng_bias_poly_PRF16')
*/
static float rng_bias_poly_PRF64[] ={
     	1.404476e-03, 3.208478e-01, 2.349322e+01, 5.470342e+02, 
     	};
static float rng_bias_poly_PRF16[] ={
     	1.754924e-05, 4.106182e-03, 3.061584e-01, 7.189425e+00, 
     	};

static void rng_tx_complete_cb(dw1000_dev_instance_t * inst);
static void rng_rx_complete_cb(dw1000_dev_instance_t * inst);
static void rng_rx_timeout_cb(dw1000_dev_instance_t * inst);
static void rng_rx_error_cb(dw1000_dev_instance_t * inst);
static void rng_tx_final_cb(dw1000_dev_instance_t * inst);

/**
 * This call initializes the ranging by setting all the required configurations and callbacks
 *
 * @param inst      pointer to dw1000_dev_instance_t 
 * @param config    pointer to the structure dw1000_rng_config_t to configure the delay parameters
 * @param nframes   number of buffers defined to store the ranging data 
 * @return dw1000_rng_instance_t
 */
dw1000_rng_instance_t * 
dw1000_rng_init(dw1000_dev_instance_t * inst, dw1000_rng_config_t * config, uint16_t nframes){

    assert(inst);
    if (inst->rng == NULL ) {
        inst->rng = (dw1000_rng_instance_t *) malloc(sizeof(dw1000_rng_instance_t) + nframes * sizeof(twr_frame_t *)); // struct + flexible array member
        assert(inst->rng);
        memset(inst->rng, 0, sizeof(dw1000_rng_instance_t));
        inst->rng->status.selfmalloc = 1;
        inst->rng->nframes = nframes;
    }

    os_error_t err = os_sem_init(&inst->rng->sem, 0x1); 
    assert(err == OS_OK);

    if (config != NULL ){
        inst->rng->config = config;
        dw1000_rng_config(inst, config);
    }

    dw1000_rng_set_callbacks(inst, rng_tx_complete_cb, rng_rx_complete_cb, rng_rx_timeout_cb, rng_rx_error_cb);
    dw1000_rng_set_tx_final_cb(inst, rng_tx_final_cb);
    dw1000_rng_set_complete_cb(inst, 0);

    inst->rng->control = (dw1000_rng_control_t){
        .delay_start_enabled = 0,
    };
    inst->rng->idx = 0xFFFF;
    inst->rng->status.initialized = 1;
    return inst->rng;
}


/**
 * This API free the range instance
 *
 * @param inst  pointer to dw1000_rng_instance_t 
 * @return void 
 */
void 
dw1000_rng_free(dw1000_rng_instance_t * inst){
   
    assert(inst);  
    if (inst->status.selfmalloc)
        free(inst);
    else
        inst->status.initialized = 0;
}

/**
 * This API sets the ranging callbacks
 *
 * @param inst                  pointer to dw1000_dev_instance_t 
 * @param rng_tx_complete_cb    pointer to the TX confirmation event callback function
 * @param rx_complete_cb        pointer to the RX good frame event callback function
 * @param rx_timeout_cb         pointer to the RX timeout events callback function
 * @param rx_error_cb           pointer to the RX error events callback function 
 * @return void
 */
void 
dw1000_rng_set_callbacks(dw1000_dev_instance_t * inst,  dw1000_dev_cb_t rng_tx_complete_cb, dw1000_dev_cb_t rng_rx_complete_cb,  dw1000_dev_cb_t rng_rx_timeout_cb,  dw1000_dev_cb_t rng_rx_error_cb){
    inst->rng_tx_complete_cb = rng_tx_complete_cb;
    inst->rng_rx_complete_cb = rng_rx_complete_cb;
    inst->rng_rx_timeout_cb = rng_rx_timeout_cb;
    inst->rng_rx_error_cb = rng_rx_error_cb;
}

/**
 * This API sets the pointer to the twr buffers
 *
 * @param inst      pointer to dw1000_dev_instance_t
 * @param twr[]     pointer to twr buffers
 * @param nframes   number of buffers defined to store the ranging data
 * @return void
 */
inline void 
dw1000_rng_set_frames(dw1000_dev_instance_t * inst, twr_frame_t twr[], uint16_t nframes){
        assert(nframes <= inst->rng->nframes);
        for (uint16_t i = 0; i < nframes; i++)
            inst->rng->frames[i] = &twr[i];
}

/**
 * This API assigns the config parameters to range instance
 *
 * @param inst    pointer to dw1000_dev_instance_t 
 * @param config  pointer to dw1000_rng_config_t
 * @return dw1000_dev_status_t 
 */
dw1000_dev_status_t 
dw1000_rng_config(dw1000_dev_instance_t * inst, dw1000_rng_config_t * config){
    assert(inst);
    assert(config);

    inst->rng->config = config;
    return inst->status;
}

/**
 * This API initializes range request 
 *
 * @param inst          pointer to dw1000_dev_instance_t 
 * @param dst_address   address of the receiver to whom range request to be sent
 * @param code          represents mode of ranging DWT_SS_TWR enables single sided two way ranging DWT_DS_TWR enables double sided 
 * two way ranging DWT_DS_TWR_EXT enables double sided two way ranging with extended frame 
 * @return dw1000_dev_status_t 
 */
dw1000_dev_status_t 
dw1000_rng_request(dw1000_dev_instance_t * inst, uint16_t dst_address, dw1000_rng_modes_t code){

    // This function executes on the device that initiates a request 
    
    os_error_t err = os_sem_pend(&inst->rng->sem,  OS_TIMEOUT_NEVER);
    assert(err == OS_OK);
    
    dw1000_rng_instance_t * rng = inst->rng;                            
    twr_frame_t * frame  = inst->rng->frames[(++rng->idx)%rng->nframes];    
    dw1000_rng_config_t * config = inst->rng->config;

    frame->seq_num++;
    frame->code = code;
    frame->src_address = inst->my_short_address;
    frame->dst_address = dst_address;
   
    dw1000_write_tx(inst, frame->array, 0, sizeof(ieee_rng_request_frame_t));
    dw1000_write_tx_fctrl(inst, sizeof(ieee_rng_request_frame_t), 0, true);     
    dw1000_set_wait4resp(inst, true);    
    dw1000_set_rx_timeout(inst, config->rx_timeout_period); 
    if (rng->control.delay_start_enabled) 
        dw1000_set_delay_start(inst, rng->delay);
    if (dw1000_start_tx(inst).start_tx_error){
        if(inst->extension_cb != NULL){
            dw1000_extension_callbacks_t *head = inst->extension_cb;
            if(inst->extension_cb->tx_error_cb != NULL){
                inst->extension_cb->tx_error_cb(inst);
            }
            inst->extension_cb = head;
        }
        os_sem_release(&inst->rng->sem);
    }
    err = os_sem_pend(&inst->rng->sem, OS_TIMEOUT_NEVER); // Wait for completion of transactions 
    os_sem_release(&inst->rng->sem);
    
   return inst->status;
}

/**
 * This API configures dw1000 to start transmission after certain delay
 *
 * @param inst          pointer to dw1000_dev_instance_t 
 * @param dst_address   address of the receiver to whom range request to be sent 
 * @param delay         time until which request has to be resumed 
 * @param code          represents mode of ranging DWT_SS_TWR enables single sided two way ranging DWT_DS_TWR enables double sided 
 * two way ranging DWT_DS_TWR_EXT enables double sided two way ranging with extended frame
 * @return dw1000_dev_status_t
 */
dw1000_dev_status_t 
dw1000_rng_request_delay_start(dw1000_dev_instance_t * inst, uint16_t dst_address, uint64_t delay, dw1000_rng_modes_t code){
    
    dw1000_rng_instance_t * rng = inst->rng;    
  
    rng->control.delay_start_enabled = 1;
    rng->delay = delay;
    dw1000_rng_request(inst, dst_address, code);
    rng->control.delay_start_enabled = 0;
    
   return inst->status;
}

/**
 * This is a template which should be replaced by the pan_master by a event that tracks UUIDs 
 * and allocated PANIDs and SLOTIDs. See dw1000_pan_set_postprocess to replace current behavor. On the TAG/ANCHOR size this 
 * template generate a json log of the event.
 *
 * @param Pt  transmit power dBm
 * @param G   Antenna Gain dB
 * @param Fc  centre frequency Hz
 * @param R   range in meters
 * @return Pr received signal level dBm
 */
float 
dw1000_rng_path_loss(float Pt, float G, float fc, float R){
    float Pr = Pt + 2 * G + 20 * log10(299792458) - 20 * log10(4 * M_PI * fc * R);
    return Pr;
}

float 
dw1000_rng_bias_correction(dw1000_dev_instance_t * inst, float Pr){
    float bias;
    switch(inst->config.prf){
        case DWT_PRF_16M:
            bias = polyval(rng_bias_poly_PRF16, Pr, sizeof(rng_bias_poly_PRF16)/sizeof(float));
            break;
        case DWT_PRF_64M:
            bias = polyval(rng_bias_poly_PRF64, Pr, sizeof(rng_bias_poly_PRF64)/sizeof(float));
            break;
        default:
            assert(0);
    }
    return bias;
}

#if MYNEWT_VAL(DW1000_RANGE)

/**
 * This API calculates time of flight based on type of ranging
 *
 * @param fframe   pointer to the first twr frame 
 * @param nframe   poinetr to the second twr frame
 * @return time of flight in float
 */
float
dw1000_rng_twr_to_tof(twr_frame_t *fframe, twr_frame_t *nframe){
    float ToF = 0;
    uint64_t T1R, T1r, T2R, T2r;
    int64_t nom,denom;

    assert(fframe != NULL);
    assert(nframe != NULL);

    twr_frame_t * first_frame = fframe;
    twr_frame_t * frame = nframe;

    switch(frame->code){
        case DWT_SS_TWR ... DWT_SS_TWR_END:
            ToF = ((first_frame->response_timestamp - first_frame->request_timestamp)
                    -  (first_frame->transmission_timestamp - first_frame->reception_timestamp))/2.;
        break;
        case DWT_DS_TWR ... DWT_DS_TWR_END:
        case DWT_DS_TWR_EXT ... DWT_DS_TWR_EXT_END:
            T1R = (first_frame->response_timestamp - first_frame->request_timestamp);
            T1r = (first_frame->transmission_timestamp  - first_frame->reception_timestamp);
            T2R = (frame->response_timestamp - frame->request_timestamp);
            T2r = (frame->transmission_timestamp - frame->reception_timestamp);
            nom = T1R * T2R  - T1r * T2r;
            denom = T1R + T2R  + T1r + T2r;
            ToF = (float) (nom) / denom;
            break;
        default: break;
    }
    return ToF;
}
#else

/**
 * This API calculates time of flight based on type of ranging
 *
 * @param rng  pointer to dw1000_rng_instance_t
 * @return time of flight in float
 */
float 
dw1000_rng_twr_to_tof(dw1000_rng_instance_t * rng){
    float ToF = 0;
    uint64_t T1R, T1r, T2R, T2r;
    int64_t nom,denom;

    twr_frame_t * first_frame = rng->frames[(rng->idx-1)%rng->nframes];
    twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];

    switch(frame->code){
        case DWT_SS_TWR ... DWT_SS_TWR_END:
            ToF = ((first_frame->response_timestamp - first_frame->request_timestamp) 
                    -  (first_frame->transmission_timestamp - first_frame->reception_timestamp))/2.; 
        break;
        case DWT_DS_TWR ... DWT_DS_TWR_END:
        case DWT_DS_TWR_EXT ... DWT_DS_TWR_EXT_END:
            T1R = (first_frame->response_timestamp - first_frame->request_timestamp); 
            T1r = (first_frame->transmission_timestamp  - first_frame->reception_timestamp);         
            T2R = (frame->response_timestamp - frame->request_timestamp); 
            T2r = (frame->transmission_timestamp - frame->reception_timestamp); 
            nom = T1R * T2R  - T1r * T2r;
            denom = T1R + T2R  + T1r + T2r;
            ToF = (float) (nom) / denom; 
            break;
        default: break;       
    }
    return ToF;
}
#endif

/**
 * This API calculates time of flight for symmetric type of ranging
 *
 * @param twr[]  pointer to twr buffers 
 * @param code   represents mode of ranging DWT_SS_TWR enables single sided two way ranging DWT_DS_TWR enables double sided 
 * two way ranging DWT_DS_TWR_EXT enables double sided two way ranging with extended frame
 * @return time of flight
 */
uint32_t 
dw1000_rng_twr_to_tof_sym(twr_frame_t twr[], dw1000_rng_modes_t code){
    uint32_t ToF = 0;
    uint64_t T1R, T1r, T2R, T2r;

    switch(code){
        case DWT_SS_TWR:
            ToF = ((twr[0].response_timestamp - twr[0].request_timestamp) 
                    -  (twr[0].transmission_timestamp - twr[0].reception_timestamp))/2.; 
        break;
        case DWT_DS_TWR:
            T1R = (twr[0].response_timestamp - twr[0].request_timestamp); 
            T1r = (twr[0].transmission_timestamp  - twr[0].reception_timestamp);         
            T2R = (twr[1].response_timestamp - twr[1].request_timestamp); 
            T2r = (twr[1].transmission_timestamp  - twr[1].reception_timestamp); 
            ToF = (T1R - T1r + T2R - T2r) >> 2;  
        break;
        default: break;       
    }
    return ToF;
}

/**
 * This function calls for final transmission to calculate range
 *
 * @param inst   pointer to dw1000_dev_instance_t
 * @return void
 */
static void 
rng_tx_final_cb(dw1000_dev_instance_t * inst){

#ifdef DS_TWR_EXT_ENABLE
    dw1000_rng_instance_t * rng = inst->rng; 
    twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];

    frame->cartesian.x = MYNEWT_VAL(LOCAL_COORDINATE_X);
    frame->cartesian.y = MYNEWT_VAL(LOCAL_COORDINATE_Y);
    frame->cartesian.z = MYNEWT_VAL(LOCAL_COORDINATE_Z);
  
#if MYNEWT_VAL(DW1000_BIAS_CORRECTION_ENABLED)
    if (inst->config.bias_correction_enable){ 
        float range = dw1000_rng_tof_to_meters(dw1000_rng_twr_to_tof(rng)); 
        float bias = 2 * dw1000_rng_bias_correction(inst, 
                    dw1000_rng_path_loss(
                        MYNEWT_VAL(DW1000_DEVICE_TX_PWR),
                        MYNEWT_VAL(DW1000_DEVICE_ANT_GAIN),
                        MYNEWT_VAL(DW1000_DEVICE_FREQ),
                        range)
                    );
        frame->spherical.range = range - bias;
    }
#else
    frame->spherical.range = dw1000_rng_tof_to_meters(dw1000_rng_twr_to_tof(rng));
#endif
    frame->spherical_variance.range = MYNEWT_VAL(RANGE_VARIANCE);
    frame->spherical_variance.azimuth = -1;
    frame->spherical_variance.zenith = -1;
    frame->utime = os_cputime_ticks_to_usecs(os_cputime_get32());//dw1000_read_systime(inst)/128;
#endif
}

/**
 * This calls for transmission complete callback
 *
 * @param inst  pointer to dw1000_dev_instance_t
 * @return void
 */
static void 
rng_tx_complete_cb(dw1000_dev_instance_t * inst)
{
    dw1000_rng_instance_t * rng = inst->rng; 
    twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];

    if (inst->fctrl == FCNTL_IEEE_RANGE_16){
        // Unlock Semaphore after last transmission
        if (frame->code == DWT_SS_TWR_FINAL || frame->code == DWT_SS_TWR_T1){
            os_sem_release(&inst->rng->sem);  
        }
#ifdef  DS_TWR_ENABLE
        else{ 
            twr_frame_t * frame = rng->frames[(rng->idx+1)%rng->nframes];
            if (frame->code ==  DWT_DS_TWR_FINAL || frame->code ==  DWT_DS_TWR_EXT_FINAL){
                    os_sem_release(&inst->rng->sem);  
            }
        }
#endif
    }
    if(inst->extension_cb != NULL){
        dw1000_extension_callbacks_t *head = inst->extension_cb;
        if(inst->extension_cb->tx_complete_cb != NULL){
            inst->extension_cb->tx_complete_cb(inst);
        }
        inst->extension_cb = head;
    }
}

/**
 * This calls for receive timeout callback
 *
 * @param inst  pointer to dw1000_dev_instance_t
 * @return void
 */
static void 
rng_rx_timeout_cb(dw1000_dev_instance_t * inst){
	if(inst->extension_cb != NULL){
        dw1000_extension_callbacks_t *head = inst->extension_cb;
        if(inst->extension_cb->rx_timeout_cb != NULL){
            inst->extension_cb->rx_timeout_cb(inst);
        }
        inst->extension_cb = head;
    }
    if(inst->fctrl == FCNTL_IEEE_RANGE_16){
        os_error_t err = os_sem_release(&inst->rng->sem);
        assert(err == OS_OK);
    }
}

/**
 * This calls for receive error callback
 *
 * @param inst  pointer to dw1000_dev_instance_t
 * @return void
 */
static void 
rng_rx_error_cb(dw1000_dev_instance_t * inst){
	if(inst->extension_cb != NULL){
        dw1000_extension_callbacks_t *head = inst->extension_cb;
        if(inst->extension_cb->rx_error_cb != NULL){
            inst->extension_cb->rx_error_cb(inst);
        }
        inst->extension_cb = head;
    }
    if(inst->fctrl == FCNTL_IEEE_RANGE_16){
        os_error_t err = os_sem_release(&inst->rng->sem);   
        assert(err == OS_OK);
    }
}

/**
 * This calls for receive complete callback
 *
 * @param inst  pointer to dw1000_dev_instance_t
 * @return void
 */
static void 
rng_rx_complete_cb(dw1000_dev_instance_t * inst)
{
    uint16_t code, dst_address; 
    dw1000_rng_config_t * config = inst->rng->config;
    dw1000_dev_control_t control = inst->control_rx_context;
    if (inst->fctrl == FCNTL_IEEE_RANGE_16){
        dw1000_read_rx(inst, (uint8_t *) &code, offsetof(ieee_rng_request_frame_t,code), sizeof(uint16_t));
        dw1000_read_rx(inst, (uint8_t *) &dst_address, offsetof(ieee_rng_request_frame_t,dst_address), sizeof(uint16_t));
    }else if(inst->extension_cb != NULL){
        dw1000_extension_callbacks_t *head = inst->extension_cb;
        if(inst->extension_cb->rx_complete_cb != NULL){
            inst->extension_cb->rx_complete_cb(inst);
        }
        inst->extension_cb = head;
        return;
    }else{
        //No extension callbacks also in place. So just return to receive mode again
        inst->control = inst->control_rx_context;
        if (dw1000_restart_rx(inst, control).start_rx_error)  
            inst->rng_rx_error_cb(inst);
        return;
    }

    // IEEE 802.15.4 standard ranging frames, software MAC filtering
    if (dst_address != inst->my_short_address){
        inst->control = inst->control_rx_context;
        if (dw1000_restart_rx(inst, control).start_rx_error)  
            inst->rng_rx_error_cb(inst);    
        return;
    }  

    // IEEE 802.15.4 standard ranging frames
#if MYNEWT_VAL(DW1000_RNG_INDICATE_LED)
    hal_gpio_toggle(LED_1);
#endif

    switch (code){
#ifdef SS_TWR_ENABLE
        case DWT_SS_TWR ... DWT_SS_TWR_FINAL:
            switch(code){
                case DWT_SS_TWR:
                    {
                        // This code executes on the device that is responding to a request
                        // printf("DWT_SS_TWR\n");
                        dw1000_rng_instance_t * rng = inst->rng; 
                        twr_frame_t * frame = rng->frames[(++rng->idx)%rng->nframes];
                        if (inst->frame_len >= sizeof(ieee_rng_request_frame_t))
                            dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_rng_request_frame_t));
                        else 
                            break; 
                    
                        uint64_t request_timestamp = dw1000_read_rxtime(inst);  
                        uint64_t response_tx_delay = request_timestamp + ((uint64_t)config->tx_holdoff_delay << 16);
                        uint64_t response_timestamp = (response_tx_delay & 0xFFFFFFFE00UL) + inst->tx_antenna_delay;
        
                        frame->reception_timestamp = request_timestamp;
                        frame->transmission_timestamp = response_timestamp;
                        frame->dst_address = frame->src_address;
                        frame->src_address = inst->my_short_address;
                        frame->code = DWT_SS_TWR_T1;

                        dw1000_write_tx(inst, frame->array, 0, sizeof(ieee_rng_response_frame_t));
                        dw1000_write_tx_fctrl(inst, sizeof(ieee_rng_response_frame_t), 0, true); 
                        dw1000_set_wait4resp(inst, true);    
                        dw1000_set_delay_start(inst, response_tx_delay);
                        dw1000_set_rx_timeout(inst, config->rx_timeout_period); 

                        if (dw1000_start_tx(inst).start_tx_error)
                            os_sem_release(&rng->sem);  
                        break;
                    }
                case DWT_SS_TWR_T1:
                    {
                        // This code executes on the device that initiated a request, and is now preparing the final timestamps
                        // printf("DWT_SS_TWR_T1\n");
                        dw1000_rng_instance_t * rng = inst->rng; 
                        twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];
                        if (inst->frame_len >= sizeof(ieee_rng_response_frame_t))
                            dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_rng_response_frame_t));
                        else 
                            break;

                        frame->request_timestamp = dw1000_read_txtime_lo(inst);   // This corresponds to when the original request was actually sent
                        frame->response_timestamp = dw1000_read_rxtime_lo(inst);  // This corresponds to the response just received            
                        frame->dst_address = frame->src_address;
                        frame->src_address = inst->my_short_address;
                        frame->code = DWT_SS_TWR_FINAL;
                    
                        // Transmit timestamp final report
                        dw1000_write_tx(inst, frame->array, 0, sizeof(twr_frame_final_t));
                        dw1000_write_tx_fctrl(inst, sizeof(twr_frame_final_t), 0, true);
                        if (dw1000_start_tx(inst).start_tx_error)
                            os_sem_release(&rng->sem);  
                        if(inst->extension_cb != NULL){
                            dw1000_extension_callbacks_t *head = inst->extension_cb;
                            if(inst->extension_cb->rx_complete_cb != NULL){
                                inst->extension_cb->rx_complete_cb(inst);
                            }
                            inst->extension_cb = head;
                        }
                        break;
                    }
                case  DWT_SS_TWR_FINAL:
                    {
                        // This code executes on the device that responded to the original request, and has now receive the response final timestamp. 
                        // This marks the completion of the single-size-two-way request. This final 4th message is perhaps optional in some applicaiton. 
                        // printf("DWT_SS_TWR_FINAL\n");
                        dw1000_rng_instance_t * rng = inst->rng; 
                        twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];
                        if (inst->frame_len >= sizeof(twr_frame_final_t))
                            dw1000_read_rx(inst, frame->array, 0, sizeof(twr_frame_final_t));
                        os_sem_release(&rng->sem);
                        if (inst->rng_complete_cb) {
                            inst->rng_complete_cb(inst);
                        }
                        if(inst->extension_cb != NULL){
                            dw1000_extension_callbacks_t *head = inst->extension_cb;
                            if(inst->extension_cb->rx_complete_cb != NULL){
                                inst->extension_cb->rx_complete_cb(inst);
                            }
                            inst->extension_cb = head;
                        }
                        break;
                    }
                default: 
                    break;
             }
             break;
#endif //SS_TWR_ENABLE
#ifdef DS_TWR_ENABLE
        case DWT_DS_TWR ... DWT_DS_TWR_FINAL:
            switch(code){
                    case DWT_DS_TWR:
                        {
                            // This code executes on the device that is responding to a original request
                            // printf("DWT_DS_TWR\n");
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * frame = rng->frames[(++rng->idx)%rng->nframes];
                            if (inst->frame_len >= sizeof(ieee_rng_request_frame_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_rng_request_frame_t));
                            else 
                                break; 

                            uint64_t request_timestamp = dw1000_read_rxtime(inst);
                            uint64_t response_tx_delay = request_timestamp + ((uint64_t)config->tx_holdoff_delay << 16);
                            uint64_t response_timestamp = (response_tx_delay & 0xFFFFFFFE00UL) + inst->tx_antenna_delay;
            
                            frame->reception_timestamp =  request_timestamp;
                            frame->transmission_timestamp =  response_timestamp;
                            frame->dst_address = frame->src_address;
                            frame->src_address = inst->my_short_address;
                            frame->code = DWT_DS_TWR_T1;

                            dw1000_write_tx(inst, frame->array, 0, sizeof(ieee_rng_response_frame_t));
                            dw1000_write_tx_fctrl(inst, sizeof(ieee_rng_response_frame_t), 0, true); 
                            dw1000_set_wait4resp(inst, true);    
                            dw1000_set_delay_start(inst, response_tx_delay);   
                            dw1000_set_rx_timeout(inst, config->rx_timeout_period); 

                            if (dw1000_start_tx(inst).start_tx_error)
                                os_sem_release(&rng->sem);
                            break;
                        }
                    case DWT_DS_TWR_T1:
                        {
                            // This code executes on the device that initiated the original request, and is now preparing the next series of timestamps
                            // The 1st frame now contains a local copy of the initial first side of the double sided scheme. 
                            // printf("DWT_DS_TWR_T1\n");
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];
                            twr_frame_t * next_frame = rng->frames[(++rng->idx)%rng->nframes];

                            if (inst->frame_len >= sizeof(ieee_rng_response_frame_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_rng_response_frame_t));
                            else 
                                break;

                            // This corresponds to when the original request was actually sent
                            frame->request_timestamp = next_frame->request_timestamp = dw1000_read_txtime_lo(inst);
                            // This corresponds to the response just received
                            frame->response_timestamp = next_frame->response_timestamp = dw1000_read_rxtime_lo(inst);
                      
                            uint16_t src_address = frame->src_address; 
                            uint8_t seq_num = frame->seq_num; 

                             // Note:: Advance to next frame 
                            frame = next_frame;                            
                            frame->dst_address = src_address;
                            frame->src_address = inst->my_short_address;
                            frame->seq_num = seq_num + 1;
                            frame->code = DWT_DS_TWR_T2;

                            uint64_t request_timestamp = dw1000_read_rxtime(inst);  
                            uint64_t response_tx_delay = request_timestamp + ((uint64_t)config->tx_holdoff_delay << 16);
                            uint64_t response_timestamp = (response_tx_delay & 0xFFFFFFFE00UL) + inst->tx_antenna_delay;
                            
                            frame->reception_timestamp = request_timestamp;
                            frame->transmission_timestamp = response_timestamp;

                            dw1000_write_tx(inst, frame->array, 0, sizeof(twr_frame_final_t));
                            dw1000_write_tx_fctrl(inst, sizeof(twr_frame_final_t), 0, true);
                            dw1000_set_wait4resp(inst, true);
                            dw1000_set_delay_start(inst, response_tx_delay);
                            dw1000_set_rx_timeout(inst, config->rx_timeout_period);
                        
                            if (dw1000_start_tx(inst).start_tx_error){
                                if(inst->extension_cb != NULL){
                                    dw1000_extension_callbacks_t *head = inst->extension_cb;
                                    if(inst->extension_cb->tx_error_cb != NULL){
                                        inst->extension_cb->tx_error_cb(inst);
                                    }
                                    inst->extension_cb = head;
                                }
                                os_sem_release(&rng->sem);  
							}
                            break; 
                        }

                    case DWT_DS_TWR_T2:
                        {
                            // This code executes on the device that responded to the original request, and is now preparing the final timestamps
                            // printf("DWT_SDS_TWR_T2\n");
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * previous_frame = rng->frames[(rng->idx++)%rng->nframes];
                            twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];

                            if (inst->frame_len >= sizeof(twr_frame_final_t))
                                dw1000_read_rx(inst,  frame->array, 0, sizeof(twr_frame_final_t));
                            else 
                                break;

                            previous_frame->request_timestamp = frame->request_timestamp;
                            previous_frame->response_timestamp = frame->response_timestamp;

                            frame->request_timestamp = dw1000_read_txtime_lo(inst);   // This corresponds to when the original request was actually sent
                            frame->response_timestamp = dw1000_read_rxtime_lo(inst);  // This corresponds to the response just received            
                            frame->dst_address = frame->src_address;
                            frame->src_address = inst->my_short_address;
                            frame->code = DWT_DS_TWR_FINAL;

                            // Transmit timestamp final report
                            dw1000_write_tx(inst, frame->array, 0, sizeof(twr_frame_final_t));
                            dw1000_write_tx_fctrl(inst, sizeof(twr_frame_final_t), 0, true); 

                            if (dw1000_start_tx(inst).start_tx_error)
                                os_sem_release(&rng->sem);  
                            
                            if (inst->rng_complete_cb) {
                                inst->rng_complete_cb(inst);
                            }
                            break;
                        }
                    case  DWT_DS_TWR_FINAL:
                        {
                            // This code executes on the device that initialed the original request, and has now receive the final response timestamp. 
                            // This marks the completion of the double-single-two-way request. 
                            // printf("DWT_SDS_TWR_FINAL\n");
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];
                            if (inst->frame_len >= sizeof(twr_frame_final_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(twr_frame_final_t));
                            if(inst->extension_cb != NULL){
                                dw1000_extension_callbacks_t *head = inst->extension_cb;
                                if(inst->extension_cb->rx_complete_cb != NULL){
                                    inst->extension_cb->rx_complete_cb(inst);
                                }
                                inst->extension_cb = head;
                            }  
                            os_sem_release(&rng->sem);
                            if (inst->rng_complete_cb) {
                                inst->rng_complete_cb(inst);
                            }
                            break;
                        }
                    default: 
                        break;
                }
            break;
#endif //DS_TWR_ENABLE
#ifdef DS_TWR_EXT_ENABLE
        case DWT_DS_TWR_EXT ... DWT_DS_TWR_EXT_FINAL:
            switch(code){
                    case DWT_DS_TWR_EXT:
                        {
                            // This code executes on the device that is responding to a original request
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * frame = rng->frames[(++rng->idx)%rng->nframes];
                            if (inst->frame_len >= sizeof(ieee_rng_request_frame_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_rng_request_frame_t));
                            else 
                                break; 

                            uint64_t request_timestamp = dw1000_read_rxtime(inst);  
                            uint64_t response_tx_delay = request_timestamp + ((uint64_t)config->tx_holdoff_delay << 16); 
                            uint64_t response_timestamp = (response_tx_delay & 0xFFFFFFFE00UL) + inst->tx_antenna_delay;
            
                            frame->reception_timestamp = request_timestamp;
                            frame->transmission_timestamp = response_timestamp;

                            frame->dst_address = frame->src_address;
                            frame->src_address = inst->my_short_address;
                            frame->code = DWT_DS_TWR_EXT_T1;

                            dw1000_write_tx(inst, frame->array, 0, sizeof(ieee_rng_response_frame_t));
                            dw1000_write_tx_fctrl(inst, sizeof(ieee_rng_response_frame_t), 0, true); 
                            dw1000_set_wait4resp(inst, true);    
                            dw1000_set_delay_start(inst, response_tx_delay);   
                            dw1000_set_rx_timeout(inst, config->rx_timeout_period); 

                            if (dw1000_start_tx(inst).start_tx_error)
                                os_sem_release(&rng->sem);  
                            break;
                        }
                    case DWT_DS_TWR_EXT_T1:
                        {
                            // This code executes on the device that initiated the original request, and is now preparing the next series of timestamps
                            // The 1st frame now contains a local copy of the initial first side of the double sided scheme. 
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * frame = rng->frames[(rng->idx++)%rng->nframes];
                            twr_frame_t * next_frame = rng->frames[(rng->idx)%rng->nframes];

                            if (inst->frame_len >= sizeof(ieee_rng_response_frame_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(ieee_rng_response_frame_t));
                            else 
                                break;

                            frame->request_timestamp = next_frame->request_timestamp = dw1000_read_txtime_lo(inst);    // This corresponds to when the original request was actually sent
                            frame->response_timestamp = next_frame->response_timestamp = dw1000_read_rxtime_lo(inst);  // This corresponds to the response just received      
                        
                            uint16_t src_address = frame->src_address; 
                            uint8_t seq_num = frame->seq_num; 

                            // Note:: Advance to next frame 
                            frame = next_frame;                            
                            frame->dst_address = src_address;
                            frame->src_address = inst->my_short_address;
                            frame->seq_num = seq_num + 1;
                            frame->code = DWT_DS_TWR_EXT_T2;

                            uint64_t request_timestamp = dw1000_read_rxtime(inst);  
                            uint64_t response_tx_delay = request_timestamp + ((uint64_t)config->tx_holdoff_delay << 16); 
                            uint64_t response_timestamp = (response_tx_delay & 0xFFFFFFFE00UL) + inst->tx_antenna_delay;
                            
                            frame->reception_timestamp = request_timestamp;
                            frame->transmission_timestamp = response_timestamp;

                            // Final callback, prior to transmission, use this callback to populate the EXTENDED_FRAME fields.
                            if (inst->rng_tx_final_cb != NULL)
                                inst->rng_tx_final_cb(inst);

                            dw1000_write_tx(inst, frame->array, 0, sizeof(twr_frame_t));
                            dw1000_write_tx_fctrl(inst, sizeof(twr_frame_t), 0, true); 
                            dw1000_set_wait4resp(inst, true);    
                            dw1000_set_delay_start(inst, response_tx_delay);   
                            dw1000_set_rx_timeout(inst, config->rx_timeout_period); 
                        
                            if (dw1000_start_tx(inst).start_tx_error)
                                os_sem_release(&rng->sem);  

                            break; 
                        }

                    case DWT_DS_TWR_EXT_T2:
                        {
                            // This code executes on the device that responded to the original request, and is now preparing the final timestamps
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * previous_frame = rng->frames[(rng->idx++)%rng->nframes];
                            twr_frame_t * frame = rng->frames[(rng->idx)%rng->nframes];

                            if (inst->frame_len >= sizeof(twr_frame_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(twr_frame_t));
                            else 
                                break;

                            previous_frame->request_timestamp = frame->request_timestamp;
                            previous_frame->response_timestamp = frame->response_timestamp;

                            frame->request_timestamp = dw1000_read_txtime_lo(inst);   // This corresponds to when the original request was actually sent
                            frame->response_timestamp = dw1000_read_rxtime_lo(inst);  // This corresponds to the response just received            
                            frame->dst_address = frame->src_address;
                            frame->src_address = inst->my_short_address;
                            frame->code = DWT_DS_TWR_EXT_FINAL;

                            // Final callback, prior to transmission, use this callback to populate the EXTENDED_FRAME fields.
                            if (inst->rng_tx_final_cb != NULL)
                                inst->rng_tx_final_cb(inst);

                            // Transmit timestamp final report
                            dw1000_write_tx(inst, frame->array, 0, sizeof(twr_frame_t));
                            dw1000_write_tx_fctrl(inst, sizeof(twr_frame_t), 0, true); 

                            if (dw1000_start_tx(inst).start_tx_error)
                                os_sem_release(&rng->sem);

                            if (inst->rng_complete_cb) {
                                inst->rng_complete_cb(inst);
                            }
                            break;
                        }
                    case  DWT_DS_TWR_EXT_FINAL:
                        {
                            // This code executes on the device that initialed the original request, and has now receive the final response timestamp. 
                            // This marks the completion of the double-single-two-way request. 
                            dw1000_rng_instance_t * rng = inst->rng; 
                            twr_frame_t * frame = inst->rng->frames[(rng->idx)%rng->nframes];
                            if (inst->frame_len >= sizeof(twr_frame_t))
                                dw1000_read_rx(inst, frame->array, 0, sizeof(twr_frame_t));
                            os_sem_release(&rng->sem);

                            if (inst->rng_complete_cb) {
                                inst->rng_complete_cb(inst);
                            }
                            if(inst->extension_cb != NULL){
                                dw1000_extension_callbacks_t *head = inst->extension_cb;
                                if(inst->extension_cb->rx_complete_cb != NULL){
                                    inst->extension_cb->rx_complete_cb(inst);
                                }
                                inst->extension_cb = head;
                            }
                            break;
                        }
                    default: 
                        break;
                }
            break;
#endif //DS_TWR_EXT_ENABLE
        default: 
            // Use this callback to extend interface and ranging services
            if(inst->extension_cb != NULL){
                dw1000_extension_callbacks_t *head = inst->extension_cb;
                if(inst->extension_cb->rx_complete_cb != NULL)
                    inst->extension_cb->rx_complete_cb(inst);
                inst->extension_cb = head;
            }
            break;
    }  
}



