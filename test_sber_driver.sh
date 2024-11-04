#!/bin/bash

DEVICE=/dev/sber_driver

echo "Running Test 1: Write and read back"
echo "testdata" > $DEVICE
READ_DATA=$(cat $DEVICE)
if [ "$READ_DATA" == "testdata" ]; then
    echo "Test 1 Passed"
else
    echo "Test 1 Failed"
fi

echo "Running Test 2: Overflow"
dd if=/dev/zero of=$DEVICE bs=1 count=1001 2>/dev/null
if [ $? -ne 0 ]; then
    echo "Test 2 Passed"
else
    echo "Test 2 Failed"
fi

echo "Running Test 3: Single Open Mode"
sudo ioctl $DEVICE 1
(cat $DEVICE &)
sleep 1
cat $DEVICE
if [ $? -ne 0 ]; then
    echo "Test 3 Passed"
else
    echo "Test 3 Failed"
fi

echo "Running Test 4: Multi Open Mode"
sudo ioctl $DEVICE 2
(echo "data1" > $DEVICE &) 
(echo "data2" > $DEVICE &)
wait
echo "Test 4 requires manual check for independent data writes"

echo "Running Test 5: Queue Clearing"
echo "temporary" > $DEVICE
cat $DEVICE > /dev/null
READ_DATA=$(cat $DEVICE)
if [ -z "$READ_DATA" ]; then
    echo "Test 5 Passed"
else
    echo "Test 5 Failed"
fi
