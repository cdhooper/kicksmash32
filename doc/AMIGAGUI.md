## Usage Example

The main feature of SmashTool, the Amiga Kicksmash graphical program,
is to program and name Kicksmash ROM banks. Here's an example:

![SmashTool](smashtool_01_full_window.jpg?raw=true "SmashTool")

When SmashTool starts up, it will query Kicksmash and report useful
information such as the firmware version, board name, and the name
of the ROM in each bank.

If you hold down the right mouse button, you will see some additional
options, including the Clock menu, where you can update the Amiga
time either from the time you previously stored in Kicksmash or the
time of your Kicksmash USB host, if connected by USB and running
hostsmash.

![Clock](smashtool_02_clock_menu.jpg?raw=true "Clock")

Let's program bank 1 with a new Kickstart image. Start by selecting
the bank to program:

![Select Bank](smashtool_03_select_bank.jpg?raw=true "Select Bank")

Now click the "Select..." button to select a ROM file to use.

![Select...](smashtool_04_select_rom_file.jpg?raw=true "Select...")

An Amiga file requester will appear. Locate the ROM image to use.

![ROM file](smashtool_05_select_rom_file.jpg?raw=true "ROM file")

After clicking "OK", you will see the ROM file name presented.

![ROM file](smashtool_06_select_rom_file.jpg?raw=true "ROM file")

You've already selected the bank to program, so now click "Program"

![Program](smashtool_07_program_button.jpg?raw=true "Program")

SmashTool will prompt you, as this will erase whatever is currently
in the ROM bank.

![Program or Cancel](smashtool_08_program_prompt.jpg?raw=true "Program or Cancel")

SmashTool works with Kicksmash to erase and program the ROM. The bank
is first erased.

![Erasing](smashtool_09_erasing.jpg?raw=true "Erasing")

Then programming commences.

![Programming](smashtool_10_programming.jpg?raw=true "Programming")

Programming will take between 10 and 60 seconds, depending on the speed
of your Amiga. Once done, the bank will be automatically verified, so
long as "Verify after programming" was selected.

![Programming done](smashtool_11_programming.jpg?raw=true "Programming done")

Now, let's name the bank which was just programmed. Click with your
mouse on the bank name and begin typing.

![Bank Name](smashtool_12_name_bank.jpg?raw=true "Bank Name")

When done, there is no need to press enter.

![Bank Name](smashtool_13_name_bank.jpg?raw=true "Bank Name")

Click the Save button to write the settings you've changed to Kicksmash.

![Save](smashtool_14_save.jpg?raw=true "Save")

When the save is complete, you will notice the Save button returns to
being disabled.

![Save disabled](smashtool_15_save_done.jpg?raw=true "Save disabled")

There are other things you can do with SmashTool, such as merging banks.
Refer to the Kicksmash [`sw_kicksmash.txt`](sw_kicksmash.txt) documentation
for information on bank merging.
