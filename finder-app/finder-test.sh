#!/bin/sh
set -e
set -u

NUMFILES=10
WRITESTR=AELD_IS_FUN
WRITEDIR=/tmp/aeld-data
OUTPUTFILE=/tmp/assignment4-result.txt

# Use absolute path for config files
CONF_DIR=/etc/finder-app/conf
username=$(cat "${CONF_DIR}/username.txt")
assignment=$(cat "${CONF_DIR}/assignment.txt")

if [ $# -ge 1 ]; then
    NUMFILES=$1
fi
if [ $# -ge 2 ]; then
    WRITESTR=$2
fi
if [ $# -ge 3 ]; then
    WRITEDIR="/tmp/aeld-data/$3"
fi

MATCHSTR="The number of files are ${NUMFILES} and the number of matching lines are ${NUMFILES}"

echo "Writing ${NUMFILES} files containing string ${WRITESTR} to ${WRITEDIR}"

rm -rf "${WRITEDIR}"
mkdir -p "${WRITEDIR}"

for i in $(seq 1 $NUMFILES); do
    writer "${WRITEDIR}/${username}$i.txt" "${WRITESTR}"
done

OUTPUTSTRING=$(finder.sh "${WRITEDIR}" "${WRITESTR}")

echo "${OUTPUTSTRING}" > "${OUTPUTFILE}"

set +e
echo "${OUTPUTSTRING}" | grep "${MATCHSTR}"
if [ $? -eq 0 ]; then
    echo "success"
    exit 0
else
    echo "failed: expected ${MATCHSTR} in output but found:"
    echo "${OUTPUTSTRING}"
    exit 1
fi