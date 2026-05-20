#!/bin/bash
#
# This is a shell script to help with automating the build of the ROM
# switcher image. First build romswitch by typing "make" in the amiga
# directory, and then run this script. It will prompt for the path
# to Capitoline (get it from http://capitoline.twocatsblack.com/) and
# then a source Kickstart ROM image. I recommend ROM 3.1.4 or newer.
# The output file will be named romswitch.rom -- and this is what you
# would then program into one of your ROM banks.

ROMRC=.genromrc
ROMSWITCH="romswitch.rom"

[ -f $ROMRC ] && . $ROMRC

usage()
{
    echo "Usage: $(basename $0) [<source_ROM> [<output_ROM>]]"
    echo
    echo "<source_ROM> is a release Kickstart 3.x ROM image"
    echo "             Use at least 3.1.4 for 68060 compatibility"
    echo "             Use at least 3.1.4 for 68060 compatibility"
    echo "<output_ROM> is the filename to create (default: $ROMSWITCH)"
}

show_possible_roms()
{
    LIST="$(
        find ..      -size 512k 2>/dev/null
        find $CAPDIR -size 512k 2>/dev/null
    )"
    if [ ! -z "$LIST" ]; then
        echo "Possible source Kickstart ROM images:"
        for NAME in $LIST; do
            echo "    $NAME"
        done
    fi
}

get_romfile_path()
{
    show_possible_roms
    echo "Enter the path to your source Kickstart ROM image"
    unset ROMSOURCE
    while [ -z "$ROMSOURCE" ]; do
        read ROMSOURCE
    done
    if [ ! -e "$ROMSOURCE" ]; then
        echo "$ROMSOURCE" does not exist
        exit 1
    fi
    if [ ! -r "$ROMSOURCE" ]; then
        echo "$ROMSOURCE" is not readable
        exit 1
    fi
}

replace_romsource_in_rc()
{
    (
        grep -v ROMSOURCE $ROMRC
        echo ROMSOURCE="$ROMSOURCE"
    ) >$ROMRC.new
    mv $ROMRC.new $ROMRC
}

replace_romswitch_in_rc()
{
    (
        grep -v ROMSWITCH $ROMRC
        echo ROMSWITCH="$ROMSWITCH"
    ) >$ROMRC.new
    mv $ROMRC.new $ROMRC
}

if [ ! -z "$1" ]; then
    if [ "$1" == "-h" ]; then
        usage
        exit 0
    fi
    if [ ! -r "$1" ]; then
        echo "Source ROM %1 does not exist."
        usage
        exit 1
    fi
    ROMSOURCE="$1"
fi

OS="$(uname -s)"
case "$OS" in
    Linux)
        PROG=capcli.Linux
        ;;
    Darwin)
        PROG=capcli.MacOS
        ;;
    CYGWIN*|MINGW*|MSYS*)
        PROG=capcli.exe
        ;;
    *)
        echo "Unknown operating system."
        exit 1
        ;;
esac

if ! command -v $PROG; then
    echo "$PROG is not in your path"
    echo "This program is part of Capitoline, which is available here:"
    echo "    http://capitoline.twocatsblack.com/"
    echo
    echo "Enter the path to Capitoline"
    unset CAPDIR
    read CAPDIR
    while [ -z "$CAPDIR" ]; do
        read CAPDIR
    done
    if [ ! -x $CAPDIR/$PROG ]; then
        echo "Can't find $PROG in $CAPDIR"
        exit 1
    fi
    echo "PATH=$CAPDIR:\$PATH" >> $ROMRC
    PATH="$CAPDIR:PATH"
else
    PPATH="$(which $PROG)"
    CAPDIR="$(dirname $PPATH)"
fi


if [ -z "$ROMSOURCE" ]; then
    get_romfile_path
    echo $ROMSOURCE
    replace_romsource_in_rc
fi
if [ ! -r "$ROMSOURCE" ]; then
    echo "$ROMSOURCE does not exist."
    get_romfile_path
    replace_romsource_in_rc
fi

$PPATH <<EOF
loadhashes "$CAPDIR/Capitoline Hashes"
auditfiles ROMs
alias SRC "$ROMSOURCE"
newrom 512k
rombase 0xF80000

add "\$SRC" exec.library
add "\$SRC" cia.resource
add "\$SRC" graphics.library
add "\$SRC" keymap.library
add "\$SRC" layers.library
add "\$SRC" potgo.resource
add "\$SRC" input.device
add "\$SRC" keyboard.device
add "\$SRC" gameport.device
add "\$SRC" timer.device
add "\$SRC" utility.library
add "\$SRC" intuition.library
add "\$SRC" gadtools.library
add romswitch
add checksum
add size
add vectors
checksum
rom
saverom $ROMSWITCH
exit
EOF
