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
    - [Secure element](#sec-elem)
    - [1-Wire](#1wire)
    - [Microphone](#microphone)
    - [SoundEval - Sound Level Evaluation Utility](#soundEval)

## <a name="install"></a>Compile and Install

Make sure your system is updated:

    sudo apt update
    sudo apt upgrade

If you are using a **32-bit** OS, add to `/boot/config.txt` the following line: [[why?](https://github.com/raspberrypi/firmware/issues/1795)]

    arm_64bit=0
    
Reboot:

    sudo reboot
    
After reboot, install git and the Raspberry Pi kernel headers:

    sudo apt install git raspberrypi-kernel-headers

Clone this repo:

    git clone --depth 1 https://github.com/sfera-labs/exo-sense-pi-kernel-module.git

Make and install:

    cd exo-sense-pi-kernel-module
    make clean
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
    
To use the [sound level evaluation functionalities](#soundEval), install the `soundEval` utility:

    sh install-snd-eval.sh
    
This script will install 2 required libraries for the `soundEval` utility: `libasound2-dev` (version `>=1.1.8-1+rpt1` required) and `libfftw3-dev` (version `>=3.3.8-2` required).

Finally, reboot:

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

For each digital input, we also expose: 
* the debounced state
* 2 debounce times in ms ("on" for high state and "off" for low state) with default value of 50ms
* 2 state counters ("on" for high state and "off" for low state)

The debounce times for each DI has been splitted in "on" and "off" in order to make the debounce feature more versatile and suited for particular application needs (e.g. if we consider digital input 1, and set its debounce "on" time to 50ms and its debounce "off" time to 0ms, we just created a delay-on type control for digital input 1 with delay-on time equal to 50ms).    
Change in value of a debounce time automatically resets both counters.    
The debounce state of each digital input at system start is UNDEFINED (-1), because if the signal on the specific channel cannot remain stable for a period of time greater than the ones defined as debounce "on" and "off" times, we are not able to provide a valid result. 

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|di*N*_deb<sup>([pollable](https://github.com/sfera-labs/knowledge-base/blob/main/raspberrypi/poll-sysfs-files.md))</sup>|R|1|Digital input *N* debounced value high|
|di*N*_deb<sup>([pollable](https://github.com/sfera-labs/knowledge-base/blob/main/raspberrypi/poll-sysfs-files.md))</sup>|R|0|Digital input *N* debounced value low|
|di*N*_deb<sup>([pollable](https://github.com/sfera-labs/knowledge-base/blob/main/raspberrypi/poll-sysfs-files.md))</sup>|R|-1|Digital input *N* debounced value undefined|
|di*N*_deb_on_ms|RW|val|Minimum stable time in ms to trigger change of the debounced value of digital input *N* to high state. Default value=50|
|di*N*_deb_off_ms|RW|val|Minimum stable time in ms to trigger change of the debounced value of digital input *N* to low state. Default value=50|
|di*N*_deb_on_cnt|R|val| Number of times with the debounced value of the digital input *N* in high state. Rolls back to 0 after 4294967295|
|di*N*_deb_off_cnt|R|val|Number of times with the debounced value of the digital input *N* in low state. Rolls back to 0 after 4294967295|

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
|status<sup>([pollable](https://github.com/sfera-labs/knowledge-base/blob/main/raspberrypi/poll-sysfs-files.md))</sup>|R|0|PIR sensor at rest|
|status<sup>([pollable](https://github.com/sfera-labs/knowledge-base/blob/main/raspberrypi/poll-sysfs-files.md))</sup>|R|1|PIR sensor detecting motion|
|cnt|R/W|*val*|*val* is a counter that indicates the number of times the status of the PIR sensor changes from 0 to 1. In write mode, only the value 0 is permitted as input value, for reset purpose. Rolls back to 0 after 4294967295|

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
|data<sup>([pollable](https://github.com/sfera-labs/knowledge-base/blob/main/raspberrypi/poll-sysfs-files.md))</sup>|R|*ts* *bits* *data*|Latest data read. The first number (*ts*) represents an internal timestamp of the received data, it shall be used only to discern newly available data from the previous one. *bits* reports the number of bits received (max 64). *data* is the sequence of bits received represnted as unsigned integer|

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

### <a name="sec-elem"></a>Secure Element - `/sys/class/exosensepi/sec_elem/`

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|serial_num|R|9 1-byte HEX values|Secure element serial number|

### <a name="1wire"></a>1-Wire

If 1-wire is configured, you will find the list of connected 1-Wire sensors' IDs in `/sys/bus/w1/devices/` with format `28-XXXXXXXXXXXX`.
To get the measured temperature (in &deg;C/1000) read the file `temperature` under the sensor's directory, e.g.:

    $ cat /sys/bus/w1/devices/28-XXXXXXXXXXXX/temperature 
    24625

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
    
### <a name="soundEval"></a>SoundEval - Sound Level Evaluation Utility

The `soundEval` utility is capable of providing the results of different classes of sound level meters (LEQ = Equivalent Continuous Sound Level), such as:

     - LAEQ,F = Equiv. contin. sound level with fast time weighting and A frequency weighting
                (time weighting = fast 125ms, A-weighting frequency weighting, results in dB(A))
     - LAEQ,S = Equiv. contin. sound level with slow time weighting and A frequency weighting
                (time weighting = slow 1000ms, A-weighting frequency weighting, results in dB(A))
     - LAEQ,I = Equiv. contin. sound level with impulse time weighting and A frequency weighting
                (time weighting = impulse 35ms, A-weighting frequency weighting, results in dB(A))
     - LEQ,F  = Equiv. contin. sound level with fast time weighting and without frequency weighting (also called Z frequency weighting)
                (time weighting = fast 125ms, no frequency weighting, results in dB)
     - LEQ,S  = Equiv. contin. sound level with slow time weighting and without frequency weighting (also called Z frequency weighting)
                (time weighting = slow 1000ms, no frequency weighting, results in dB)
     - LEQ,I  = Equiv. contin. sound level with impulse time weighting and without frequency weighting (also called Z frequency weighting)
                (time weighting = impulse 35ms, no frequency weighting, results in dB)
     - LCEQ,F = Equiv. contin. sound level with fast time weighting and C frequency weighting
                (time weighting = fast 125ms, C-weighting frequency weighting, results in dB(C))
     - LCEQ,S = Equiv. contin. sound level with slow time weighting and C frequency weighting
                (time weighting = slow 1000ms, C-weighting frequency weighting, results in dB(C))
     - LCEQ,I = Equiv. contin. sound level with impulse time weighting and C frequency weighting
                (time weighting = impulse 35ms, C-weighting frequency weighting, results in dB(C))

Essentially, it is able to perform 2 types of analysis:

- **Period evaluation**: continuous evaluation of short time periods (125ms if time weight = fast, 1000ms if time weight = slow, 35ms if time weight = impulse). This type of evaluation is commonly used to build applications as classifier of specific events/sounds. E.g. if our purpose is to detect fast impulse sounds as gun shots or explosions, we may use an LAEQ,I type period evaluation, and when the sound level is above a certain threshold, it's possible to trigger immediate actions right after the detection.

- **Interval evaluation**: continuous evaluation of prolonged time intervals, specified by the user. This type of evaluation is suited for applications where the period of time in analysis is bigger than the time constants fast, slow or impulse. An example of application is where we analyze an interval of 8 working hours with a LAEQ,F type sound meter, and if the equivalent continuous sound level is bigger than the threshold specified by the legislation regarding the maximum sound level in a working environment (usually 85dB(A)), we know that it's time to consider the use of personal sound protective equipment.
The interval evaluation result represents the Root Mean Square value of the acustic pressure for that interval, and is not the sum of the results of the single period evaluations which the interval is composed of.

If frequency type A-weighting or C-weighting is selected, the sound levels are adjusted according to the values of a specific table with adjustment factors. This table groups the adjustment factors into bandwidths of octaves or fractions of octaves. The `soundEval` utility allows the choice between groups of 1 octave or groups of 1/3 octaves.

**1/3 octave weighting table:**
|Frequency [Hz]|A-weighting (dB)|C-weighting (dB)|
|----|:---:|:-:|
|6.3|-85.4|-21.3|
|8|-77.8|-17.7|
|10|-70.4|-14,3|
|12.5|-63,4|-11.2|
|16|-56.7|-8.5|
|20|-50.5|-6.2|
|25|-44.7|-4.4|
|31.5|-39.4|-3.0|
|40|-34.6|-2.0|
|50|-30.2|-1.3|
|63|-26.2|-0,8|
|80|-22.5|-0.5|
|100|-19.1|-0.3|
|125|-16.1|-0.2|
|160|-13.4|-0.1|
|200|-10.9|0.0|
|250|-8.6|0.0|
|315|-6.6|0.0|
|400|-4.8|0.0|
|500|-3.2|0.0|
|630|-1.9|0.0|
|800|-0.8|0.0|
|1000|0.0|0.0|
|1250|0.6|0.0|
|1600|1.0|-0.1|
|2000|1.2|-0.2|
|2500|1.3|-0.3|
|3150|1.2|-0.5|
|4000|1.0|-0.8|
|5000|0.5|-1.3|
|6300|-0.1|-2.0|
|8000|-1.1|-3.0|
|10000|-2.5|-4.4|
|12500|-4.3|-6.2|
|16000|-6.6|-8.5|
|20000|-9.3|-11.2|

**1 octave weighting table:**
|Frequency [Hz]|A-weighting (dB)|C-weighting (dB)|
|----|:---:|:-:|
|8|-77.8|-17.7|
|16|-56.7|-8.5|
|31.5|-39.4|-3.0|
|63|-26.2|-0,8|
|125|-16.1|-0.2|
|250|-8.6|0.0|
|500|-3.2|0.0|
|1000|0.0|0.0|
|2000|1.2|-0.2|
|4000|1.0|-0.8|
|8000|-1.1|-3.0|
|16000|-6.6|-8.5|

Explaining all the different classes of sound level meters is out of the scope of this documentation, but following are some general rules, with hope that it will be helpful to the user for the right choice.

**Time weighting**:
- **Fast**: is usually used to replicate the natural response of human ear (125ms)
- **Slow**: is good at "ignoring" short, fast sounds like car doors slamming or balloons popping. his makes slow weighting a good choice for environmental noise studies, especially for studies that span many hours or even days.
- **Impulse**: is usually used in situations where there are sharp impulsive noises to be measured, such as fireworks or gun shots.

**Frequency weighting**:
- **A**: the data are frequency weighted according to the A-WEIGHTING function, which main purpose is to replicate the human ear response at different frequency bandwidths.
- **C**: flat response with the extreme high (near 20kHz) and low (near 0 Hz) frequencies attenuated.
- **Z**: no frequency adjustments are made in base to the frequency bandwidths.


The `soundEval` utility functionalities can be accessed under `/sys/class/exosensepi/sound_eval/`:

|File|R/W|Value|Description|
|----|:---:|:-:|-----------|
|enabled|R/W|0|Utility disabled, audio card available for other applications|
|enabled|R/W|1|Utility enabled, audio card used by `soundEval`|
|weight_time|R/W|F|FAST time weighting selected|
|weight_time|R/W|S|SLOW time weighting selected|
|weight_time|R/W|I|IMPULSE time weighting selected|
|weight_freq|R/W|A|A-weight frequency weighting selected|
|weight_freq|R/W|Z|Z-weight frequency weighting selected|
|weight_freq|R/W|C|C-weight frequency weighting selected|
|weight_freq_bands|R/W|1|1 octave frequency weighting table selected|
|weight_freq_bands|R/W|3|1/3 octave frequency weighting table selected|
|interval_sec|R/W|*val*|*val* is the custom interval of evaluation in seconds. If set to 0, the interval evaluation is not running and the leq_interval file with interval evaluation result is not updated|
|leq_period|R|*ts* *val*|*ts* represents an internal timestamp of the received data, it shall be used only to discern newly available data from the previous one. *ts* is in Unix time epoch format in milliseconds. *val* is the result of the period evaluation, in millidecibels according to the set time (fast, slow or impulse), frequency weighting (dB, dB(A) or dB(C)) and frequency weighting table (1 octave or 1/3 octave). If the first evaluation is not yet complete or the utility has never been enabled, *val* has value -1 and *ts* has value 0.|
|leq_interval|R|*ts* *val*|*ts* represents an internal timestamp of the received data, it shall be used only to discern newly available data from the previous one. *ts* is in Unix time epoch format in milliseconds. *val* is the result of the interval evaluation, in millidecibels according to the set time (fast, slow or impulse), frequency weighting (dB, dB(A) or dB(C)) and frequency weighting table (1 octave or 1/3 octave). If the first evaluation is not yet complete or the utility has never been enabled, *val* has value -1 and *ts* has value 0.|
|leq_period_bands|R|*ts* *val_1* *val_2* .. *val_n*|*ts* represents an internal timestamp of the received data, it shall be used only to discern newly available data from the previous one. *ts* is in Unix time epoch format in milliseconds. *val_1* *val_2* .. *val_n* are the equivalent sound levels of each frequency bandwidth detected during the period evaluation, in millidecibels according to the set time (fast, slow or impulse), frequency weighting (dB, dB(A) or dB(C)) and frequency weighting table (1 octave or 1/3 octave). If the first evaluation is not yet complete or the utility has never been enabled, *val_1* *val_2* .. *val_n* have value -1 and *ts* has value 0.|
