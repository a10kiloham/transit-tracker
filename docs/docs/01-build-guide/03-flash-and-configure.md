# Part 2: Flash & Configure

Next, we will flash the Transit Tracker firmware to the Matrix Portal board, connect it to Wi-Fi, and select which routes you want to display.

## Flash Firmware

1. Plug the board into your computer.
2. Hold the "Boot" button on the board. While continuing to hold "Boot", press "Reset" at the same time, then let go of both buttons. The board is now in flashing mode.
3. Visit the [configuration website](https://countdown-config.horner.tj) and press "Prepare new device".
4. Press "Flash Firmware".
5. Select the Transit Tracker from the list of devices. It should be named something like "USB JTAG/serial debug unit". If it's something else, you may need to repeat step 2.
6. Press "Install Transit Tracker Firmware".
7. Check "Erase device", then press "Next", then "Install".
8. Once you see "Installation complete", press the reset button on the board.

## Connect to Wi-Fi

1. Press "Next" to connect to Wi-Fi.
2. Enter your Wi-Fi credentials and press "Connect".
3. Once flashed and connected to Wi-Fi, you can exit the flashing dialog and press "Next: Configuration". Make sure the board and your computer are on the same network.

## Select Routes

1. Go to "Select feed" and choose the one that corresponds to your transit agency.
2. Go to "Select routes" and find the stops/routes you are interested in, then press "Save".
3. You can customize various options in "Customize styles".
4. Once you are done, press "Save config" to push the configuration to the board.
