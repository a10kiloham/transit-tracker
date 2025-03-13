# How do I switch Wi-Fi networks?

You can switch the Wi-Fi network your Transit Tracker connects to in a few different ways.

## USB

1. Connect your Transit Tracker to a computer with a Chromium browser (Firefox and Safari do not support this process).
2. Visit the [flashing page](https://countdown-config.horner.tj/flash).
3. Click the "Flash Firmware" button.
4. Select your Transit Tracker from the list. It will usually be listed as something like "USB JTAG/serial debug unit".
5. Click the "Connect" button.
6. Click "Connect to Wi-Fi" or "Change Wi-Fi".
7. Select your network from the list and enter its password if necessary.

## Wi-Fi

If the Transit Tracker cannot find the configured network, it will start broadcasting its own after about a minute. You can connect your phone or laptop to this network to change the Wi-Fi settings.

1. Power on your Transit Tracker and wait about one minute.
2. Connect to the Wi-Fi network starting with `transit-tracker-`. The password is `hunter2hunter2`.
3. Visit the configuration page at http://192.168.4.1 if it does not appear automatically.
4. Select the new Wi-Fi network from the list or enter its details manually.
