#include <stdio.h>
#include <math.h>
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"

/*
  Example set of bytes coming over the iBUS line for setting servos: 
    20 40 DB 5 DC 5 54 5 DC 5 E8 3 D0 7 D2 5 E8 3 DC 5 DC 5 DC 5 DC 5 DC 5 DC 5 DA F3
  Explanation
    Protocol length: 20
    Command code: 40 
    Channel 0: DB 5  -> value 0x5DB
    Channel 1: DC 5  -> value 0x5Dc
    Channel 2: 54 5  -> value 0x554
    Channel 3: DC 5  -> value 0x5DC
    Channel 4: E8 3  -> value 0x3E8
    Channel 5: D0 7  -> value 0x7D0
    Channel 6: D2 5  -> value 0x5D2
    Channel 7: E8 3  -> value 0x3E8
    Channel 8: DC 5  -> value 0x5DC
    Channel 9: DC 5  -> value 0x5DC
    Channel 10: DC 5 -> value 0x5DC
    Channel 11: DC 5 -> value 0x5DC
    Channel 12: DC 5 -> value 0x5DC
    Channel 13: DC 5 -> value 0x5DC
    Checksum: DA F3 -> calculated by adding up all previous bytes, total must be FFFF
 */

// LED pins
#define RED_PIN 18

// Receiver pins
#define UART_RX_PIN 5
#define UART_ID uart1
#define BAUD_RATE 115200
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY UART_PARITY_NONE
#define PROTOCOL_LENGTH 0x20
#define PROTOCOL_OVERHEAD 0x03
#define PROTOCOL_COMMAND40 0x40
#define PROTOCOL_CHANNELS 6

// Motor pins
#define PWM_WRAP 500
#define PWM_A 0 // 0
#define IN1A 2 // 2
#define IN2A 1 // 1
#define PWM_B 7 // 7
#define IN1B 4 // 4
#define IN2B 6 // 6
#define STBY 3 // 3

// UART vars
uint16_t channel[PROTOCOL_CHANNELS];
uint8_t buffer[32];
uint8_t ptr = 0;
uint8_t len = 0;
uint16_t chksum = 0;
uint16_t lchksum = 0;
uint16_t hchksum = 0;

struct motorA {
    pwm: PWM_A, // Pin PWM channel
    in1: IN1A, // Pin
    in2: IN2A, // Pin
    speed: 0, // 0 - PWM_WRAP
    direction: 1, // 1 = forward, 0 = backward
};

struct motorB {
    pwm: PWM_B, // Pin PWM channel
    in1: IN1B, // Pin
    in2: IN2B, // Pin
    speed: 0, // 0 - PWM_WRAP
    direction: 1, // 1 = forward, 0 = backward
};

// Function to normalize values
int normalize(double value, double old_min, double old_max, double new_min, double new_max) {
    // Check if value is within old range
    if (value < old_min || value > old_max) {
        printf("Value is out of old range, returning 0\n");
        return 0;
    }
    // Check if new_min is less than new_max
    if (new_min >= new_max) {
        printf("Invalid new range, returning 0\n");
        return 0;
    }
    double old_range = old_max - old_min;
    double new_range = new_max - new_min;
    double normalized_value = (((value - old_min) * new_range) / old_range) + new_min;
    return round(normalized_value);  // round to the nearest integer
}

int calculate_motor_speeds() {

    // get acceleration speed and direction
    uint16_t speed = channel[2]; // 1000 - 2000
    uint16_t steer = channel[0]; // 1000 - 2000

    // check if moving foward or backward (+-10 tolerance)
    if(speed >= 1510 && speed <= 2000) {
        // forward
        motorA.direction = 1;
        motorB.direction = 1;
    }
    else if(speed >= 1000 && speed <= 1490) {
        // backward
        motorA.direction = 0;
        motorB.direction = 0;
    }
    else {
        // stop
        motorA.direction = 1;
        motorB.direction = 1;
        speed = 1500;
    }

    // STEERING POWER ==================================================================================================
    if(steer > 1510 && steer <= 2000) {
        // turn right - reduce speed of motorB
        double turn_perc = (steer-1500)/500*100;
        // reduce the speed of motorB by turn_perc
        speed_reduction = speed * (turn_perc/100);
        // set motorB (right wheel) speed to speed-speed_reduction to turn right
        motorB.speed = normalize(round(speed-speed_reduction), 1000, 2000, 0, PWM_WRAP);
    }
    else if(steer >= 1000 && steer < 1490) {
        // turn left - reduce speed of motorB
        double turn_perc = abs(steer-1500)/500*100;
        // reduce the speed of motorB by turn_perc
        speed_reduction = speed * (turn_perc/100);
        // set motorB (left wheel) speed to speed-speed_reduction to turn left
        motorA.speed = normalize(round(speed-speed_reduction), 1000, 2000, 0, PWM_WRAP);
    }
    else {
        // both motors are same speed
        motorA.speed = normalize(speed, 1000, 2000, 0, PWM_WRAP);
        motorB.speed = normalize(speed, 1000, 2000, 0, PWM_WRAP);
    }

    printf("A: %d B: %d\n", motorA.speed, motorB.speed);

}

// set a motor direction
void update_motor(struct motor) {
    if (direction == 0) {
        // Backwards // write motor.IN1 = LOW and motor.IN2 = HIGH
        gpio_put(motor.in1, 0);
        gpio_put(motor.in2, 1);
    } else {
        // Forwards // write motor.IN1 = HIGH and motor.IN2 = LOW
        gpio_put(motor.in1, 1);
        gpio_put(motor.in2, 0);
    }
    // write motor.PWM = speed
    pwm_set_gpio_level(motor.pwm, motor.speed);
}

void on_uart_rx() {
    // get the first byte of the message
    uint8_t fb = uart_getc(UART_ID);
    // get the length of the message and check if valid (between 0x03 and 0x20)
    if (fb <= PROTOCOL_LENGTH && fb > PROTOCOL_OVERHEAD) {
        ptr = 0; // reset pointer
        len = fb - PROTOCOL_OVERHEAD; // set length of message
        chksum = 0xFFFF - fb; // calculate checksum
        while(ptr < len) {
            // get data and update checksum
            uint8_t value = uart_getc(UART_ID); // get data
            buffer[ptr++] = value; // store data in buffer
            chksum -= value; // update checksum
        }
        lchksum = uart_getc(UART_ID); // get checksum low byte
        hchksum = uart_getc(UART_ID); // get checksum high byte
        if(chksum == (hchksum << 8) + lchksum) {
            // valid servo command received
            if(buffer[0] == PROTOCOL_COMMAND40) {
                // extract channel data from buffer
                for (uint8_t i = 1; i < PROTOCOL_CHANNELS * 2 + 1; i += 2) {
                    channel[i / 2] = buffer[i] | (buffer[i + 1] << 8);
                }
            }
        }
    }
}

int main() {
    // Initialize the standard I/O library and setup LED pin
    stdio_init_all();
    gpio_init(RED_PIN, motorA.in1, motorA.in2, motorB.in1, motorB.in2, STBY);
    gpio_set_dir(RED_PIN, GPIO_OUT);
    // Init the UART for our receiver
    // Set up our UART with a basic baud rate.
    uart_init(UART_ID, BAUD_RATE);
    // Set the TX and RX pins by using the function select on the GPIO
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);
    // Set UART flow control CTS/RTS, we don't want these, so turn them off
    uart_set_hw_flow(UART_ID, false, false);
    // Set our data format
    uart_set_format(UART_ID, DATA_BITS, STOP_BITS, PARITY);
    // Turn off FIFO's - we want to do this character by character
    uart_set_fifo_enabled(UART_ID, true);
    // Select correct interrupt for the UART we are using
    int UART_IRQ = UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;
    // And set up and enable the interrupt handlers
    irq_set_exclusive_handler(UART_IRQ, on_uart_rx);
    irq_set_enabled(UART_IRQ, true);
    // Now enable the UART to send interrupts - RX only
    uart_set_irq_enables(UART_ID, true, false);
    // Initialize motors =======================================================
    // enable GPIO port to PWM
    gpio_set_dir(motorA.in1, GPIO_OUT);
    gpio_set_dir(motorA.in2, GPIO_OUT);
    gpio_set_dir(motorB.in1, GPIO_OUT);
    gpio_set_dir(motorB.in2, GPIO_OUT);
    // set GPIO function to PWM
    gpio_set_function(motorA.pwm, GPIO_FUNC_PWM);
    gpio_set_function(motorB.pwm, GPIO_FUNC_PWM);
    // Find out which PWM slice is connected to our GPIO pin
    uint slice_numA = pwm_gpio_to_slice_num(motorA.pwm);
    uint slice_numB = pwm_gpio_to_slice_num(motorB.pwm);
    // enable PWM channels
    pwm_set_enabled(slice_numA, true);
    pwm_set_enabled(slice_numB, true);
    // set PWM wrap, in microseconds
    pwm_set_wrap(slice_numA, PWM_WRAP);
    pwm_set_wrap(slice_numB, PWM_WRAP);
    // set PWM channels
    pwm_set_chan_level(slice_numA, motorA.pwm, motorA.speed);
    pwm_set_chan_level(slice_numB, motorB.pwm, motorB.speed);
    // The main loop
    while (true) {
        gpio_put(RED_PIN, 1);
        sleep_ms(250);
        gpio_put(RED_PIN, 0);
        sleep_ms(250);
        printf(
            "Channel 1: %d Channel 2: %d Channel 3: %d Channel 4: %d Channel 5: %d Channel 6: %d \n",
            channel[0], // right stick left/right - steering
            channel[1], // right stick up/down
            channel[2], // acceleration - left stick up/down
            channel[3], // left stick left/right
            channel[4], // potentiometer left
            channel[5] // potentiometer right
        );
        calculate_motor_speeds();
        // update_motor(motorA);
        // update_motor(motorB);
    }
}
