## Prerequisites

You will need multiarch gcc and libstdc++. Prepare the environment:

    git submodule update --init
    make prepare

## Building

Build the agent:

    make

## Using

Run the agent:

    ./tf2_healslut.elf $(pidof hl2_linux)

Enjoy!
