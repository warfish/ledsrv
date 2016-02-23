#!/bin/bash

LEDSRV_FIFO_NAME=/tmp/ledsrv

if [[ $# < 1 ]]; then
    echo "$0:";
    echo " get-led-state | set-led-state <on|off>";
    echo " get-led-color | set-led-color <red|green|blue>";
    echo " get-led-rate | set-led-rate <1..5>";
    exit 0;
fi

reply='';

case $1 in
"get-led-state") 
    echo get-led-state > $LEDSRV_FIFO_NAME
    reply = `cat $LEDSRV_FIFO_NAME`
;;
esac

echo $reply


