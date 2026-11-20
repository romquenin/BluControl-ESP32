# Wireless GameCube controller

🚧 **Work in progress**: nothing is published for the GameCube yet.

The plan is to keep an original GameCube controller completely intact:
- the ESP32 connects to the wires that normally go to the console, and plays the console's role on the controller's data line (Joybus);
- rumble is driven through the Joybus protocol itself, with a 5 V supply on the controller's rumble power line, so no extra rumble board is needed.

Thanks to [Yakara](https://github.com/yakaracolombia) for the GameCube wireless mod that inspired this work.
