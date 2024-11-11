KickSmash32 is a Kickstart ROM replacement module for the Amiga 3000
and Amiga 4000 computer systems. Features include:
   * In-system programming via an Amiga command line utility
   * Out-of-system programming (USB-C) via a Linux command line utility
   * Up to 8 independent flash banks
   * Configurable ROM bank switching on long reboot or cold power-on
   * Optional host file service to the Amiga over the USB-C link

![KickSmash32 photo](photos/2024_11_07_kicksmash32_rev5_profile_view_2.jpg?raw=true "Kicksmash32")

All hardware, firmware, and software is open source.

View files in the doc directory for complete documentation.

-------------------------------------------------------

## Usage Example

If you are familiar with the Amiga CLI and have the ability to get
ROM images to your Amiga, then programming and switching between Kickstart
ROM banks in the running system is not difficult.

List all banks.

![smash bank show](doc/smash_example_1_bank_show.jpg?raw=true "smash bank show")

Write DiagROM to a bank.

![smash write](doc/smash_example_2_write.jpg?raw=true "smash write")

Name that bank for future reference.

![smash bank name](doc/smash_example_3_bank_name.jpg?raw=true "smash bank name")

It's always a good idea to verify what you wrote.

![smash verify](doc/smash_example_4_verify.jpg?raw=true "smash verif")

Switch to that bank and reboot.

![smash bank current](doc/smash_example_5_bank_current.jpg?raw=true "smash bank current")

After pressing Enter, your computer is now running DiagROM.

How do you switch back? If you had specified a "long reset" sequence, you could press and hold Control-Amiga-Amiga to switch back. Since you haven't yet done that, you will need to power cycle your Amiga or connect to the KickSmash over USB and tell it from the host to switch.
