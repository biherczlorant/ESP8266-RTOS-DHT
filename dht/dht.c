#include <dht/dht.h>
#include <display.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <rom/ets_sys.h>  // os_delay_us
#include <string.h>

// DHT timer precision in microseconds
#define DHT_TIMER_INTERVAL 2
#define DHT_DATA_BITS 40

// #define DEBUG_DHT
#ifdef DEBUG_DHT
#define debug(fmt, ...) printf("%s" fmt "\n", "dht: ", ##__VA_ARGS__);
#else
#define debug(fmt, ...) /* (do nothing) */
#endif

/*
 *  Note:
 *  A suitable pull-up resistor should be connected to the selected GPIO line
 *
 *  __           ______          _______ ___________________________
 *    \    A    /      \   C    /       \   DHT duration_data_low    / \
 *     \_______/   B    \______/    D    \__________________________/   DHT
 * duration_data_high    \__
 *
 *
 *  Initializing communications with the DHT requires four 'phases' as follows:
 *
 *  Phase A - MCU pulls signal low for at least 18000 us
 *  Phase B - MCU allows signal to float back up and waits 20-40us for DHT to
 * pull it low Phase C - DHT pulls signal low for ~80us Phase D - DHT lets
 * signal float back up for ~80us
 *
 *  After this, the DHT transmits its first bit by holding the signal low for
 * 50us and then letting it float back high for a period of time that depends on
 * the data bit. duration_data_high is shorter than 50us for a logic '0' and
 * longer than 50us for logic '1'.
 *
 *  There are a total of 40 data bits transmitted sequentially. These bits are
 * read into a byte array of length 5.  The first and third bytes are humidity
 * (%) and temperature (C), respectively.  Bytes 2 and 4 are zero-filled and the
 * fifth is a checksum such that:
 *
 *  byte_5 == (byte_1 + byte_2 + byte_3 + btye_4) & 0xFF
 *
 */

/**
 * Wait specified time for pin to go to a specified state.
 * If timeout is reached and pin doesn't go to a requested state
 * false is returned.
 * The elapsed time is returned in pointer 'duration' if it is not NULL.
 */
static bool dht_await_pin_state(uint8_t pin, uint32_t timeout,
                                bool expected_pin_state, uint32_t* duration) {
    for (uint32_t i = 0; i < timeout; i += DHT_TIMER_INTERVAL) {
        // need to wait at least a single interval to prevent reading a jitter
        os_delay_us(DHT_TIMER_INTERVAL);
        if (gpio_get_level(pin) == expected_pin_state) {
            if (duration) {
                *duration = i;
            }
            return true;
        }
    }

    return false;
}

/**
 * Request data from DHT and read raw bit stream.
 * The function call should be protected from task switching.
 * Return false if error occurred.
 */
static inline bool dht_fetch_data(dht_sensor_type_t sensor_type, uint8_t pin,
                                  bool bits[DHT_DATA_BITS]) {
    uint32_t low_duration;
    uint32_t high_duration;

    // Phase 'A' pulling signal low to initiate read sequence
    gpio_set_level(pin, 0);
    os_delay_us(sensor_type == DHT_TYPE_SI7021 ? 500 : 20000);
    gpio_set_level(pin, 1);

    // Step through Phase 'B', 40us
    if (!dht_await_pin_state(pin, 40, false, NULL)) {
        debug("Initialization error, problem in phase 'B'\n");
        return false;
    }

    // Step through Phase 'C', 88us
    if (!dht_await_pin_state(pin, 88, true, NULL)) {
        debug("Initialization error, problem in phase 'C'\n");
        return false;
    }

    // Step through Phase 'D', 88us
    if (!dht_await_pin_state(pin, 88, false, NULL)) {
        debug("Initialization error, problem in phase 'D'\n");
        return false;
    }

    // Read in each of the 40 bits of data...
    for (int i = 0; i < DHT_DATA_BITS; i++) {
        if (!dht_await_pin_state(pin, 65, true, &low_duration)) {
            debug("LOW bit timeout\n");
            return false;
        }
        if (!dht_await_pin_state(pin, 75, false, &high_duration)) {
            debug("HIGHT bit timeout\n");
            return false;
        }
        bits[i] = high_duration > low_duration;
    }
    return true;
}

/**
 * Pack two data bytes into single value and take into account sign bit.
 */
static inline int16_t dht_convert_data(dht_sensor_type_t sensor_type,
                                       uint8_t msb, uint8_t lsb) {
    int16_t data;

    if (sensor_type == DHT_TYPE_DHT22) {
        data = msb & 0x7F;
        data <<= 8;
        data |= lsb;
        if (msb & BIT(7)) {
            data = 0 - data;  // convert it to negative
        }
    } else {
        data = msb * 10;
    }

    return data;
}

esp_err_t dht_read_data(dht_sensor_type_t sensor_type, gpio_num_t pin,
                        int16_t* humidity, int16_t* temperature) {
    bool bits[DHT_DATA_BITS];
    uint8_t data[DHT_DATA_BITS / 8] = {0};
    bool result;

    taskENTER_CRITICAL();
    result = dht_fetch_data(sensor_type, pin, bits);
    taskEXIT_CRITICAL();

    if (!result) {
        return ESP_FAIL;
    }

    for (uint8_t i = 0; i < DHT_DATA_BITS; i++) {
        // Read each bit into 'result' byte array...
        data[i / 8] <<= 1;
        data[i / 8] |= bits[i];
    }

    if (data[4] != ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        debug("Checksum failed, invalid data received from sensor\n");
        return ESP_FAIL;
    }

    *humidity = dht_convert_data(sensor_type, data[0], data[1]);
    *temperature = dht_convert_data(sensor_type, data[2], data[3]);

    debug("Sensor data: humidity=%d, temp=%d\n", *humidity, *temperature);

    return ESP_OK;
}

esp_err_t dht_read_float_data(dht_sensor_type_t sensor_type, gpio_num_t pin,
                              float* humidity, float* temperature) {
    int16_t i_humidity, i_temp;

    if (dht_read_data(sensor_type, pin, &i_humidity, &i_temp) == ESP_OK) {
        *humidity = (float)i_humidity / 10;
        *temperature = (float)i_temp / 10;
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t dht_init(gpio_num_t pin, bool pull_up) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pin_bit_mask = 1ULL << pin,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE};
    esp_err_t result = gpio_config(&io_conf);
    if (result == ESP_OK) {
        gpio_set_level(pin, 1);
        return ESP_OK;
    }
    return result;
}

void dht_write_sensor_data_to_display(void) {
    int16_t temperature = 0;
    int16_t humidity = 0;

    if (dht_read_data(DHT_TYPE_DHT11, 12, &humidity, &temperature) != ESP_OK) {
        ESP_LOGE("dht", "couldn't read dht data");
    }
    char tempstr[10];
    char humstr[10];

    snprintf(tempstr, sizeof(tempstr),
             "%d\xb0"
             "C",
             temperature/10);
    snprintf(humstr, sizeof(humstr), "%d%%", humidity/10);

    display_temp_and_hum_screen(&tempstr, &humstr);
}
