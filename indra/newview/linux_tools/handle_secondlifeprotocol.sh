#!/bin/bash

# Send a URL of the form secondlife://... to the viewer.
#

URL="$1"

if [ -z "$URL" ]; then
    #echo Usage: $0 secondlife://...
    echo "Usage: $0 [ secondlife://  | hop:// ] ..."
    exit
fi

RUN_PATH=`dirname "$0" || echo .`
# [2026-09-19] This script lives in etc/; the launcher is one level up.
cd "${RUN_PATH}"

if [ `pidof do-not-directly-run-wolfviewer-bin` ]; then
	exec dbus-send --type=method_call --dest=com.secondlife.ViewerAppAPIService /com/secondlife/ViewerAppAPI com.secondlife.ViewerAppAPI.GoSLURL string:"$1"
else
	exec ../wolfviewer -url \'"${URL}"\'
fi

