solaXd
======

Daemon for communication with SolaX-X1 Mini inverter via RS485.

    ┌─────────┐              ┌─────────┐              ┌─────────┐
    │ SolaX   │     RS485    │ USB to  │      USB     │   RPi   │
    │ X1-Mini │<------------>│ RS485   │<------------>│   or    │
    │ Inverter│              │ Adapter │              │   PC    │
    └─────────┘              └─────────┘              └─────────┘
Recommendations for RS485 wiring:
* Twisted pair
* Termination with 120 Ohm
* Optional: Bias resistors with 2x 1K


# Features

* SolaX-X1 Mini Live-Data
* RS485 / TTY back-end
* JSON-Path output format
* HTTP front-end
* systemd support


## Latest Version

* 0.1.0 (2021-03-05)
   * Initial commit


## Known bugs

* The HTTP front-end is working fine with HTTP-Binding of openHAB 3, but not correct with Chrome or Edge browser.


## Build & Installation

To build the solaXd Building requires the following packages and/or features: ``git, gcc``

Note: The expected interfaces to the inverter is ``/dev/ttyUSB0``, please adapt if necessary (for more details refer to chapter "Adding a USB interface").

    # get the source code
    git clone https://github.com/jensjordan/solaXd.git
    
    # build + install
    cd solaXd
    sudo sh install.sh
    
    # test of solaXd (with simulated inverter dat
    ./solaXd -d /dev/ttyUSB0 -L 3 -x
    # test of solaXd
    ./solaXd -d /dev/ttyUSB0 -L 4
    
    # edit configuration file
    sudo nano /etc/default/solaxd
    
    # Enable and start the daemon on systemd based systems (e.g. Debian 8, Ubuntu 15.x, Raspbian Jessie and newer):
    sudo systemctl enable solaxd.service
    sudo systemctl start solaxd.service
    
    # Check status of solaXd
    systemctl status solaxd.service
    
    cd ..


## Configuration

The following parameter are used by solaXd:
    
    -d <DEV>    Use DEV as solaXd serial/tty device
    -p <PORT>   Port of HTTP-Server
    -s <SAMPLE> Samples used for average calculation
    -a <ADDR>   Use ADDR as inverter bus address
    -l <FILE>   Write log to FILE, instead to stderr
    -L <LEVEL>  Log LEVEL: 0=error / 1=notice / 2=info / 3=debug / 4=trace
    -x          Enable test mode with simulated inverter data
    --help      Display this help and exit
    --version   Output version information and exit
    
Note: If solaXd is started by systemd the configuration file ``/etc/default/solaxd`` will be used.


## Uninstall :(

Because sometime we need it.

    cd solaXd
    sudo sh uninstall.sh


## Connect to openHAB

The solaXd was designed to transfer Live-Data from SolaX-X1 Mini inverter into openHAB. Therefor the HTTP-Binding will be used.
Ensure the following openHAB addons are installed: ``HTTP binding, JSON-Path translation``

Here you will find an example configuration for a HTTP thing:

    Thing http:url:solaxd "SolaX-Daemon" @ "HTTP" [ baseURL="http://127.0.0.1:6789", refresh=10 ]   //, contentType="application/json"]
    {
        Channels:
        Type number : quality_of_service       "Quality of Service"        [ stateTransformation="JSONPATH:$.inverter.quality_of_service" ]
        Type number : live_data_temperature    "Live-Data: Temperature"    [ stateTransformation="JSONPATH:$.inverter.live_data.temperature" ]
        Type number : live_data_dc1_voltage    "Live-Data: DC Voltage 1"   [ stateTransformation="JSONPATH:$.inverter.live_data.dc1_voltage" ]
        Type number : live_data_dc1_current    "Live-Data: DC Current 1"   [ stateTransformation="JSONPATH:$.inverter.live_data.dc1_current" ]
        Type number : live_data_dc2_voltage    "Live-Data: DC Voltage 2"   [ stateTransformation="JSONPATH:$.inverter.live_data.dc2_voltage" ]
        Type number : live_data_dc2_current    "Live-Data: DC Current 2"   [ stateTransformation="JSONPATH:$.inverter.live_data.dc2_current" ]
        Type number : live_data_ac_voltage     "Live-Data: AC Voltage"     [ stateTransformation="JSONPATH:$.inverter.live_data.ac_voltage" ]
        Type number : live_data_ac_current     "Live-Data: AC Current"     [ stateTransformation="JSONPATH:$.inverter.live_data.ac_current" ]
        Type number : live_data_frequency      "Live-Data: Frequency"      [ stateTransformation="JSONPATH:$.inverter.live_data.frequency" ]
        Type number : live_data_power          "Live-Data: AC Power"       [ stateTransformation="JSONPATH:$.inverter.live_data.power" ]
        Type number : live_data_energy_today   "Live-Data: Energy Today"   [ stateTransformation="JSONPATH:$.inverter.live_data.energy_today" ]
        Type number : live_data_energy_total   "Live-Data: Energy Total"   [ stateTransformation="JSONPATH:$.inverter.live_data.energy_total" ]
        Type number : live_data_runtime_total  "Live-Data: Runtime Total"  [ stateTransformation="JSONPATH:$.inverter.live_data.runtime_total" ]
        Type number : live_data_status         "Live-Data: Status"         [ stateTransformation="JSONPATH:$.inverter.live_data.status" ]
        Type number : live_data_error_bits     "Live-Data: Error Bits"     [ stateTransformation="JSONPATH:$.inverter.live_data.error_bits" ]
    }

And here an example for the corresponding items:

    Number SolaX_QualityOfService  "SolaX Quality of Service"       { channel="http:url:solaxd:quality_of_service" }
    Number SolaX_Temperatur        "SolaX Temperatur [%d °C]"       { channel="http:url:solaxd:live_data_temperature" }
    Number SolaX_DC_Voltage        "SolaX DC-Voltage [%.1f V]"      { channel="http:url:solaxd:live_data_dc1_voltage" }
    Number SolaX_DC_Current        "SolaX DC-Current [%.1f A]"      { channel="http:url:solaxd:live_data_dc1_current" }
    Number SolaX_DC2_Voltage       "SolaX DC2-Voltage [%.1f V]"     { channel="http:url:solaxd:live_data_dc2_voltage" }
    Number SolaX_DC2_Current       "SolaX DC2-Current [%.1f A]"     { channel="http:url:solaxd:live_data_dc2_current" }
    Number SolaX_AC_Voltage        "SolaX AC-Voltage [%.1f V]"      { channel="http:url:solaxd:live_data_ac_voltage" }
    Number SolaX_AC_Current        "SolaX AC-Current [%.1f A]"      { channel="http:url:solaxd:live_data_ac_current" }
    Number SolaX_Frequency         "SolaX Frequency [%.2f Hz]"      { channel="http:url:solaxd:live_data_frequency" }
    Number SolaX_Power             "SolaX Power [%d W]"             { channel="http:url:solaxd:live_data_power" }
    Number SolaX_Energy_Today      "SolaX Energy Today [%.1f kWh]"  { channel="http:url:solaxd:live_data_energy_today" }
    Number SolaX_Energy_Total      "SolaX Energy Total [%.1f kWh]"  { channel="http:url:solaxd:live_data_energy_total" }
    Number SolaX_Runtime_Total     "SolaX Runtime Total [%.0f h]"   { channel="http:url:solaxd:live_data_runtime_total" }
    Number SolaX_Status            "SolaX Status [%d]"              { channel="http:url:solaxd:live_data_status" }
    Number SolaX_Error_Bits        "SolaX Error Bits [0x%08X]"      { channel="http:url:solaxd:live_data_error_bits" }


## Adding a USB interface by symlink

If you attach a USB to RS485 converter to your RasberryPi or computer, it will show up as ``/dev/ttyUSB0``.
This is a problem, if you ever add another serial interface that uses the same driver, solaXd will use maybe the wrong device.

Therefore, you do this:

* We are interested on the device serial number:
  ```
  udevadm info --attribute-walk --path=/sys/bus/usb-serial/devices/ttyUSB1 | grep "ATTRS{serial}"
  ```
  It contains a line like this: ``ATTRS{serial}=="A123ABCD"   <-- device serial number``
* Create a new udev rule:
  ```
  echo 'ACTION=="add", SUBSYSTEM=="tty", ATTRS{serial}=="device serial number", SYMLINK+="ttySOLAX", OWNER="dialout"' > /lib/udev/rules.d/70-solaxd.rules
  ```
  Of course you need to replace the ``device serial number`` with whatever ``udevadm`` displayed.
  This rule creates a symlink ``/dev/ttySOLAXD`` which points to identified USB device. 
  An example file should be in ``/lib/udev/rules.d/``.
* Reboot and verify that ``/dev/ttySOLAX`` exists:
  ```
  ls -l /dev/ttySOLAX
  ```

The solaXd configuration will use that symlink.


## Trademark Disclaimer

Product names, logos, brands and other trademarks referred to within this project are the property of their respective trademark holders. These trademark holders are not affiliated with our website. They do not sponsor or endorse our materials.
