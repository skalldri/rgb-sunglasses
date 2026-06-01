# Proto0 Board Pinout

Inspired by the Thingy53 board, use is as reference as needed (especially for QSPI setup): /root/ncs/v3.1.1/zephyr/boards/nordic/thingy53

## Proto0 Pinout
P0.26 = BMI270 Interrupt 1
P0.24 = BMI270 Chip Select
P0.25 = BMI270 Interrupt 2
P0.12 = BMI270 (SPI) Clock
P0.10 = BMI270 (SPI) MISO
P0.09 = BMI270 (SPI) MOSI

P1.04 = BQ25792 Interrupt (N, active low?)

P0.21 = Mic (PDM) Clock
P0.19 = Mic (PDM) Data

DTS: led_strip_spi_0
P1.05 = LED_DIN

DTS: led_strip_spi_1
P0.29 = LED_DIN_1

DTS: led_strip_spi_2 (new on Proto0)
P1.01 = LED_DIN_2 (Onboard LEDs)

DTS: qspi?
P0.13 = QSPI 0
P0.14 = QSPI 1
P0.15 = QSPI 2
P0.16 = QSPI 3
P0.17 = QSPI Clock
P0.18 = QSPI Chip Select

DTS spi flash MX25R6435F


P0.22 = I2C IRQ (battery charger?)
P1.03 = I2C SCL
P1.02 = I2C SDA

P0.02 = Wake (N, active low)
P1.10 = Button 0
P1.11 = Button 1
P0.30 = Button 2
P1.12 = Button 3

P1.13 = NRF UART TX
P1.14 = NRF UART RX

P0.00 = XTAL Crystal 1
P0.01 = XTAL Crystal 2