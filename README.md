# Exo Sense Pi kernel module

Raspberry Pi OS Kernel module for [Exo Sense Pi](https://www.sferalabs.cc/product/exo-sense-pi/) - the multi-sensor module based on the Raspberry Pi Compute Module 4.

- [Compile and Install](#install)
- [Calibration](#tha-calibration)
- [Usage](#usage)
    - [LED](#led)
    - [Digital Inputs](#digital-in)
    - [Digital Output](#digital-out)
    - [Digital I/O TTLx](#digital-io)
    - [Temperature, Humidity, Air quality](#tha)
    - [System Temperature](#sys-temp)
    - [PIR motion detection](#pir)
    - [Light intensity](#lux)
    - [Buzzer](#buzzer)
    - [Wiegand](#wiegand)
    - [Microphone](#microphone)
    - [Secure element](#sec-elem)
    - [1-Wire](#1wire)

## <a name="install"></a>Compile and Install

If you don't have git installed:

    sudo apt install git

Clone this repo:

    git clone --depth 1 https://github.com/sfera-labs/exo-sense-pi-kernel-module.git
    
Install the Raspberry Pi kernel headers:

    sudo apt install raspberrypi-kernel-headers

Make and install:

    cd exo-sense-pi-kernel-module
    make
    sudo make install
    
Compile the Device Tree and install it:

    dtc -@ -Hepapr -I dts -O dtb -o exosensepi.dtbo exosensepi.dts
    sudo cp exosensepi.dtbo /boot/overlays/

Add to `/boot/config.txt` the following line:

    dtoverlay=exosensepi
    
If you want to use TTL1 as 1-Wire bus, add this line too:

    dtoverlay=w1-gpio

Optionally, to be able to use the `/sys/class/exosensepi/` files not as super user, create a new group "exosensepi" and set it as the module owner group by adding an udev rule:

    sudo groupadd exosensepi
    sudo cp 99-exosensepi.rules /etc/udev/rules.d/

and add your user to the group, e.g. for user "pi":

    sudo usermod -a -G exosensepi pi

Install the calibration script and service:

    sudo cp exosensepi-calibrate.service /lib/systemd/system/
    sudo cp exosensepi-calibrate.py /usr/local/bin/
    sudo chmod +x /usr/local/bin/exosensepi-calibrate.py

Reboot:

    sudo reboot

## <a name="tha-calibration"></a>Calibration

Exo Sense Pi produces internal heat that influences its sensors readings. To compensate, the kernel module applies a transformation to the values reported by the temperature sensor based on calibration parameters computed by the procedure described below.    
The compensated temperature values are, in turn, used to adjust humidity and VOC values.

To perform the calibration procedure, enable the `exosensepi-calibrate` service:

    sudo systemctl enable exosensepi-calibrate

Shut down (`sudo shutdown now`) the Pi and remove power. Wait for the module to cool off (about 1 hour) and power it back on.

The calibration procedure will start automatically. You will hear a short beep when it starts and the blue LED will blink while running.

The process should finish in about 30 minutes, but could run for up to 80 minutes. When it completes you will hear 3 short beeps and the blue LED will be steady on. The calibration service gets automatically disabled after completion so it won't run again on the next boot.

You will find the computed calibration parameters `temp_calib_m` and `temp_calib_b` set in `/etc/modprobe.d/exosensepi.conf`:

    options exosensepi temp_calib_m=<M> temp_calib_b=<B>

Reboot Exo Sense Pi to have the module reload and apply the calibration parameters.

## <a name="usage"></a>Usage

After installing the module, you will find all the available devices under the directory `/sys/class/exosensepi/`.

The following paragraphs list all the possible devices (directories) and files coresponding to Exo Sense Pi's features. 

You can read and/or write to these files to configure, monitor and control your Exo Sense Pi.

### <a name="led"></a>LED - `/sys/class/exosensepi/led/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|status|R/W|0|LED off|
|status|R/W|1|LED on|
|status|W|F|Flip LED's state|
|blink|W|*t*|LED on for *t* ms|
|blink|W|*ton* *toff* *rep*|LED blink *rep* times with *ton*/*toff* ms periods. E.g. "200 50 3"|

### <a name="digital-in"></a>Digital Inputs - `/sys/class/exosensepi/digital_in/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|di*N*|R|1|Digital input *N* high|
|di*N*|R|0|Digital input *N* low|

### <a name="digital-out"></a>Digital Output - `/sys/class/exosensepi/digital_out/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|do1|R/W|0|Digital output 1 open|
|do1|R/W|1|Digital output 1 closed|
|do1|W|F|Flip digital output 1's state|

### <a name="digital-io"></a>Digital I/O TTLx - `/sys/class/exosensepi/digital_io/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|ttl*N*_mode|R/W|x|TTL *N* (1 - 2) line not controlled by kernel module|
|ttl*N*_mode|R/W|in|TTL *N* (1 - 2) line set as input|
|ttl*N*_mode|R/W|out|TTL *N* (1 - 2) line set as output|
|ttl*N*|R(/W)|0|TTL *N* (1 - 2) line low. Writable only in output mode|
|ttl*N*|R(/W)|1|TTL *N* (1 - 2) line high. Writable only in output mode|

### <a name="tha"></a>Temperature, Humidity, Air quality - `/sys/class/exosensepi/tha/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|temp_rh|R|*s* *t* *tCal* *rh* *rhCal*|Temperature and humidity values. *s* represents an internal temperature variation factor; calibrated values are more reliable when *s* is stable between subsequent readings. *t* is the raw temperature (&deg;C/100); *tCal* is the calibrated temperature (&deg;C/100); *rh* is the raw relative humidity (%/100); *rhCal* is the calibrated relative humidity (%/100)|
|temp_rh_voc|R|*s* *t* *tCal* *rh* *rhCal* *voc* *vocIdx*|Temperature, humidity and air quality values. *s*, *t*, *tCal*, *rh*, *rhCal* are as above; *voc* is the raw value from the Volatile Organic Compound (VOC) sensor; *vocIdx* is the VOC index which represents an air quality value on a scale from 0 to 500 where a lower value represents cleaner air and a value of 100 represent the typical air composition over the past 24h. To have reliable VOC index values, read this file continuously with intervals of 1 second|
|temp_offset|R/W|*val*|Temperature offset (&deg;C/100, positive or negative) to be added for the computation of the above calibrated values to conpensate for external factors that might influence Exo Sense Pi|

### <a name="sys-temp"></a>System Temperature - `/sys/class/exosensepi/sys_temp/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|t1|R|*val*|Temperature value from sensor T1 (&deg;C/100)|
|t2|R|*val*|Temperature value from sensor T2 (&deg;C/100)|

### <a name="pir"></a>PIR motion detection - `/sys/class/exosensepi/pir/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|status|R|0|PIR sensor at rest|
|status|R|1|PIR sensor detecting motion|

### <a name="lux"></a>Light intensity - `/sys/class/exosensepi/lux/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|lux|R|*val*|Light intensity (lx/100)|

### <a name="buzzer"></a>Buzzer

The buzzer can be controlled via simple ON/OFF commands or via PWM to produce tones variations.

#### <a name="buzzer-onoff"></a>ON/OFF - `/sys/class/exosensepi/buzzer/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|status|R/W|0|Buzzer off|
|status|R/W|1|Buzzer on|
|status|W|F|Flip buzzer's state|
|beep|W|*t*|Buzzer on for *t* ms|
|beep|W|*ton* *toff* *rep*|Buzzer beep *rep* times with *ton*/*toff* ms periods. E.g. "200 50 3"|

#### <a name="buzzer-pwm"></a>PWM control

To set up PWM control write `1` to `/sys/class/pwm/pwmchip0/export`, e.g.:

    sudo echo 1 > /sys/class/pwm/pwmchip0/export
    
Once set, use `/sys/class/pwm/pwmchip0/pwm1/period` and `/sys/class/pwm/pwmchip0/pwm1/duty_cycle` to set period and duty-cycle values (in nanoseconds).

Activate/deactivate the buzzer by writing `1`/`0` to `/sys/class/pwm/pwmchip0/pwm1/enable`. You can modify period and duty-cycle values while the buzzer is active. 

### <a name="wiegand"></a>Wiegand - `/sys/class/exosensepi/wiegand/`

You can use the TTL lines as a Wiegand interface for a keypad or card reader. Connect TTL1/TTL2 respctively to the D0/D1 lines of the Wiegand device.

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|enabled|R/W|0|Wiegand interface disabled|
|enabled|R/W|1|Wiegand interface enabled|
|data|R|*ts* *bits* *data*|Latest data read. The first number (*ts*) represents an internal timestamp of the received data, it shall be used only to discern newly available data from the previous one. *bits* reports the number of bits received (max 64). *data* is the sequence of bits received represnted as unsigned integer|

The following properties can be used to improve noise detection and filtering. The noise property reports the latest event and is reset to 0 after being read.

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|pulse_width_max|R/W|*val*|Maximum bit pulse width accepted, in &micro;s|
|pulse_width_min|R/W|*val*|Minimum bit pulse width accepted, in &micro;s|
|pulse_itvl_max|R/W|*val*|Maximum interval between pulses accepted, in &micro;s|
|pulse_itvl_min|R/W|*val*|Minimum interval between pulses accepted, in &micro;s|
|noise|R|0|No noise|
|noise|R|10|Fast pulses on lines|
|noise|R|11|Pulses interval too short|
|noise|R|12/13|Concurrent movement on both D0/D1 lines|
|noise|R|14|Pulse too short|
|noise|R|15|Pulse too long|

### <a name="microphone"></a>Microphone

The I2S MEMS microphone can be used with the standard ALSA drivers.

Run `arecord -l` to check the microphone is available:

    $ arecord -l
    **** List of CAPTURE Hardware Devices ****
    card 0: exosensepimic [exosensepi-mic], device 0: bcm2835-i2s-ics43432-hifi ics43432-hifi-0 [bcm2835-i2s-ics43432-hifi ics43432-hifi-0]
      Subdevices: 1/1
      Subdevice #0: subdevice #0

Record a WAV audio file (stop with <kbd>Ctrl</kbd> + <kbd>C</kbd>):

    arecord -D plughw:0 -c1 -r 48000 -f S32_LE -t wav -V mono -v rec.wav

You may need to change the number after `plughw:` depending on the card number returned by `arecord -l` (e.g. `plughw:1`).

You can add software volume control with alsamixer; to this end, copy the `asoundrc` file from this repo to `~/.asoundrc`:

    cp asoundrc ~/.asoundrc

then run `arecord` once and stop it (<kbd>Ctrl</kbd> + <kbd>C</kbd>):

    arecord -D plughw:0 -c1 -r 48000 -f S32_LE -t wav -V mono -v rec.wav

Now run `alsamixer` and, press <kbd>F6</kbd> to open the sound card selector and select `exosensepi-mic`.   
Press <kbd>F4</kbd> to select the "Capture" view , adjust the volume with the up/down arrow keys and press <kbd>Esc</kbd> to save and exit.

You can now record from the `dmic_sv` device with adjusted volume:

    arecord -D dmic_sv -c2 -r 44100 -f S32_LE -t wav -V mono -v rec-vol.wav

### <a name="sec-elem"></a>Secure Element - `/sys/class/exosensepi/sec_elem/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|serial_num|R|9 1-byte HEX value|Secure element serial number|

### <a name="1wire"></a>1-Wire

If 1-wire is configured, you will find the list of connected 1-Wire sensors' IDs in `/sys/bus/w1/devices/` with format `28-XXXXXXXXXXXX`.    
To get the measured temperature read the file `w1_slave` under the sensor's directory, e.g.:

    $ cat /sys/bus/w1/devices/28-XXXXXXXXXXXX/w1_slave 
    25 01 55 00 7f ff 0c 0c 08 : crc=08 YES
    25 01 55 00 7f ff 0c 0c 08 t=18312
    
At the end of the first line you will read `YES` if the communication succeded and the read temperature value will be reported at the end of the second line expressed in &deg;C/1000.
