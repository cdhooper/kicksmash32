@echo "Getting wdi-simple from"
@echo "https://github.com/rogerclarkmelbourne/Arduino_STM32/blob/master/drivers/win/wdi-simple.exe?raw=true"
wget https://github.com/rogerclarkmelbourne/Arduino_STM32/blob/master/drivers/win/wdi-simple.exe?raw=true" -o wdi-simple.exe
@echo "Installing wdi-simple as a driver for KickSmash"
wdi-simple.exe --vid 0x1209 --pid 0x1610 --type 3 --name "KickSmash Prg" --dest "maple-serial"
