#!/bin/bash

LEDSRV_FIFO_NAME=/tmp/ledsrv
LEDSRV_IN_FIFO=/tmp/ledsrv.in.$BASHPID
LEDSRV_OUT_FIFO=/tmp/ledsrv.out.$BASHPID

if [[ $# < 1 ]]; then
    echo "$0:";
    echo " get-led-state | set-led-state <on|off>";
    echo " get-led-color | set-led-color <red|green|blue>";
    echo " get-led-rate | set-led-rate <1..5>";
    exit 0;
fi

# Create our fifos for server and send connection request
mkfifo $LEDSRV_IN_FIFO
mkfifo $LEDSRV_OUT_FIFO
echo $BASHPID > $LEDSRV_FIFO_NAME

case $1 in
"get-led-state"|"get-led-color"|"get-led-rate") 
    echo $1 > $LEDSRV_IN_FIFO
;;

"set-led-state"|"set-led-color"|"set-led-rate") 
    echo $1 $2 > $LEDSRV_IN_FIFO
;;

esac

cat $LEDSRV_OUT_FIFO
rm $LEDSRV_IN_FIFO $LEDSRV_OUT_FIFO
