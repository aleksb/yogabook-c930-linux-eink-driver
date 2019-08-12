#!/bin/bash
set +e
rmmod wacom 2>/dev/null
rmmod usbhid 2>/dev/null
rmmod eink 2>/dev/null
modprobe eink
modprobe usbhid
modprobe wacom
true
