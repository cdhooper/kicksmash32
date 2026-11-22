#!/bin/bash
#
# This is a shell script to help with automating the build of a Kickstart
# ROM image which includes smashfs (smashfsrom). First build smashfsrom by
# typing "make" in the amiga directory, and then run this script. It will
# prompt for the path to Capitoline (get it from
# http://capitoline.twocatsblack.com/) and then a source Kickstart ROM image.
#
# The new ROM is built from all modules of the source ROM, in their
# original order, except those in REMOVE_MODULES (audio.device and
# ramdrive.device). Building a new image (rather than deleting modules in
# place) leaves all free space in one block where smashfsrom can fit.
#
# Do not remove FFS ("filesystem"): Kickstart 3.2 system-startup calls
# FindResident("filesystem") when booting from any disk, and raises a
# dead-end alert 87000000 if it is missing (even with FFS in the RDB).
#
# The output file will be named smashfsrom.rom by default -- and this is
# what you would then program into one of your ROM banks.

ROMRC=.genromrc
SMASHFSROM_OUT="smashfsrom.rom"
SMASHFSROM="$(dirname "$0")/smashfsrom"
REMOVE_MODULES="audio.device ramdrive.device"

[ -f $ROMRC ] && . $ROMRC

usage()
{
    echo "Usage: $(basename $0) [-d] [<source_ROM> [<output_ROM>]]"
    echo
    echo "-d           use smashfsrom_d (serial debug output) instead"
    echo "<source_ROM> is a release Kickstart 3.x ROM image"
    echo "<output_ROM> is the filename to create (default: $SMASHFSROM_OUT)"
    echo
    echo "These modules are removed from the source ROM: $REMOVE_MODULES"
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
        grep -v ROMSOURCE $ROMRC 2>/dev/null
        echo ROMSOURCE="$ROMSOURCE"
    ) >$ROMRC.new
    mv $ROMRC.new $ROMRC
}

if [ "$1" == "-h" ]; then
    usage
    exit 0
fi
if [ "$1" == "-d" ]; then
    SMASHFSROM="${SMASHFSROM}_d"
    shift
fi
if [ ! -z "$1" ]; then
    if [ ! -r "$1" ]; then
        echo "Source ROM $1 does not exist."
        usage
        exit 1
    fi
    ROMSOURCE="$1"
fi
if [ ! -z "$2" ]; then
    SMASHFSROM_OUT="$2"
fi

if [ ! -r "$SMASHFSROM" ]; then
    echo "$SMASHFSROM does not exist: type \"make\" first"
    exit 1
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

if ! command -v $PROG >/dev/null; then
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
    PPATH="$CAPDIR/$PROG"
else
    PPATH="$(command -v $PROG)"
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

# List the modules in the source ROM
LISTING="$("$PPATH" <<EOF
loadhashes "$CAPDIR/Capitoline Hashes"
loadrom "$ROMSOURCE"
rom
exit
EOF
)"

ROMBASE="$(echo "$LISTING" | sed -n 's/.*ROMBase:\([0-9a-fA-F]*\).*/\1/p' | head -1)"
ROMSIZE="$(echo "$LISTING" | sed -n 's/.*(\([0-9]*\) of [0-9]*\/.*/\1/p' | head -1)"
if [ -z "$ROMBASE" ] || [ -z "$ROMSIZE" ]; then
    echo "Could not read ROM layout of $ROMSOURCE"
    exit 1
fi

# Module lines: "<num> <name> <x>:<offset>:<size> ...", name may have spaces
MODULES="$(echo "$LISTING" |
           sed -n 's/^ *[0-9][0-9]* \(.*[^ ]\)  *[0-9]*:[0-9]*:[0-9]* .*/\1/p')"

ADDS=""
REMOVED=""
while IFS= read -r MOD; do
    case "$MOD" in
        Checksum|Size|Vectors)
            continue  # Regenerated below
            ;;
        [Kk]ickity*)
            ADDS="${ADDS}add kickity-split"$'\n'
            continue
            ;;
    esac
    SKIP=0
    for RM in $REMOVE_MODULES; do
        if [ "$MOD" == "$RM" ]; then
            SKIP=1
            REMOVED="$REMOVED $MOD"
        fi
    done
    [ $SKIP -eq 0 ] && ADDS="${ADDS}add \"\$SRC\" \"$MOD\""$'\n'
done <<< "$MODULES"

if [ -z "$REMOVED" ]; then
    echo "Warning: $REMOVE_MODULES not found in $ROMSOURCE"
fi

rm -f "$SMASHFSROM_OUT"
OUTPUT="$("$PPATH" <<EOF
loadhashes "$CAPDIR/Capitoline Hashes"
auditfiles ROMs
alias SRC "$ROMSOURCE"
newrom $((ROMSIZE / 1024))k
rombase 0x$ROMBASE

${ADDS}add "$SMASHFSROM"
add checksum
add size
add vectors
checksum
rom
saverom $SMASHFSROM_OUT
exit
EOF
)"

if echo "$OUTPUT" | grep -qi "space in rom"; then
    rm -f "$SMASHFSROM_OUT"
    FREE="$(echo "$OUTPUT" | sed -n 's/^-- (Unallocated) \([0-9]*\)/\1/p' |
            sort -n | tail -1)"
    echo "$(basename $SMASHFSROM) does not fit in $ROMSOURCE"
    echo "after removing$REMOVED (largest free space: $FREE bytes)."
    exit 1
fi
if ! echo "$OUTPUT" | grep -q "^ *[0-9][0-9]* .*smashfs"; then
    rm -f "$SMASHFSROM_OUT"
    echo "$OUTPUT"
    echo "Failed to add $SMASHFSROM to ROM"
    exit 1
fi
if [ ! -s "$SMASHFSROM_OUT" ]; then
    echo "$OUTPUT"
    echo "Failed to create $SMASHFSROM_OUT"
    exit 1
fi
echo "$OUTPUT" | grep "^ *[0-9][0-9]* \|^-- "
echo
echo "Created $SMASHFSROM_OUT from $ROMSOURCE"
echo "Removed:$REMOVED  Added: $(basename $SMASHFSROM)"
