# FIFO Control Tests

A series of tests for developing the FIFO Control patch series (Link goes here)

### Installation

~~~
make && make install
~~~

### Usage

Run the commands for the following tests from the build directory.

***

## RTT Test

Tests the return time trip of a single byte from userspace. Attempts to use internal loopback if the device supports it, otherwise it requires a physical loopback. 

### Usage

~~~
./rtt_test <serial-device>
~~~

Substitute <serial-device> with the serial device to test(eg. /dev/ttyS0).


### Outputs

RTT of a single byte serial transmission in microseconds 

***

## UART Probe 

A Kernel module which provides debugfs interfaces for testing serial devices for the FIFO size and trigger levels. Currently only works with 16550 compatible devices(eg.  16650, 16750, 16850, etc.). Uses internal loopback. 

### Usage

#### Probe all serial devices

~~~
./uart_probe.sh
~~~

#### Probe specific serial device

Ensure debugfs is enabled  
  
~~~
sudo mount -t debugfs none /sys/kernel/debug
~~~
      
Select serial device to test.
Replace <serial_device> with desired device(eg. ttyS0).

~~~
echo -n <serial_device> | sudo tee /sys/kernel/debug/uart_probe/select_dev
~~~

Ensure the port is closed before testing. Close any program using it.

#### Run the Tests

##### Rx Trigger Level

~~~
sudo cat /sys/kernel/debug/uart_probe/rx_trig_level
~~~

##### Tx Trigger Level
~~~
sudo cat /sys/kernel/debug/uart_probe/tx_trig_level
~~~

##### Rx FIFO Size
~~~
sudo cat /sys/kernel/debug/uart_probe/rx_fifo_size
~~~

##### Tx FIFO Size
~~~
sudo cat /sys/kernel/debug/uart_probe/tx_fifo_size
~~~

***