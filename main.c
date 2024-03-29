#include <stdio.h>
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

#define UART_ID uart1
#define BAUD_RATE 115200
#define DATA_BITS 8
#define STOP_BITS 1
#define PARITY UART_PARITY_NONE
#define PROTOCOL_LENGTH 0x20
#define PROTOCOL_OVERHEAD 0x03
#define PROTOCOL_COMMAND40 0x40
#define PROTOCOL_CHANNELS 6
#define UART_RX_PIN 5
#define RED_PIN 18

uint8_t buffer[32];
uint8_t ptr = 0;
uint8_t len = 0;
uint16_t chksum = 0;
uint16_t lchksum = 0;
uint16_t hchksum = 0;
uint16_t channel[PROTOCOL_CHANNELS];

// normalize the value to a range between 0 and 100
uint16_t normalize(uint16_t value, uint8_t type) {
    return ((value - 1000) / 10);
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
    // Initialize the standard I/O library
    stdio_init_all();
    gpio_init(RED_PIN);
    gpio_set_dir(RED_PIN, GPIO_OUT);
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
    // The main loop
    while (true) {
        gpio_put(RED_PIN, 1);
        sleep_ms(250);
        gpio_put(RED_PIN, 0);
        sleep_ms(250);
        printf(
            "Channel 1: %d Channel 2: %d Channel 3: %d Channel 4: %d Channel 5: %d Channel 6: %d \n",
            normalize(channel[0], 0),
            normalize(channel[1], 0),
            normalize(channel[2], 0),
            normalize(channel[3], 0),
            normalize(channel[4], 1),
            normalize(channel[5], 1)
        );
    }
}
