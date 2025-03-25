## Usage Example

If you are familiar with the Amiga CLI and have the ability to get
ROM images to your Amiga, then programming and switching between Kickstart
ROM banks in the running system is not difficult.

List all banks.

![smash bank show](smash_example_1_bank_show.jpg?raw=true "smash bank show")

Write DiagROM to a bank.

![smash write](smash_example_2_write.jpg?raw=true "smash write")

Name that bank for future reference.

![smash bank name](smash_example_3_bank_name.jpg?raw=true "smash bank name")

It's always a good idea to verify what you wrote.

![smash verify](smash_example_4_verify.jpg?raw=true "smash verif")

Switch to that bank and reboot.

![smash bank current](smash_example_5_bank_current.jpg?raw=true "smash bank current")

After pressing Enter, your computer is now running DiagROM.

How do you switch back? If you had specified a "long reset" sequence, you could press and hold Control-Amiga-Amiga to switch back. Since you haven't yet done that, you will need to power cycle your Amiga or connect to the KickSmash over USB and tell it from the host to switch.

-------------------------------------------------------

The smash utility can also be used to test communication between your Amiga and Kicksmash.

![smash test start](smash_example_6_test_start.jpg?raw=true "smash test")
![smash test end](smash_example_7_test_end.jpg?raw=true "smash test (end)")
