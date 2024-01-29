#!/bin/bash
SLIDESHOW_FOLDER="$HOME/Slideshow"
SLIDESHOW_FILE="$HOME/slideshow-file"

[[ ! -d "$SLIDESHOW_FOLDER" ]] && \
	mkdir "$SLIDESHOW_FOLDER"

function fatal() {
	echo -e "FATAL: $1"
	exit 1
}

function warning() {
	echo -e "ERROR: $1"
}

function updateSlideshowFile() {
	find "$SLIDESHOW_FOLDER" -type f | sort | sed -e 's/^/@/' -e 's/$/\n/' > "$SLIDESHOW_FILE"

    # Remove all files except for png, jpg and jpeg
    sed -i -E '/.*\.(png|jpe?g)$/I!d' "$SLIDESHOW_FILE"

	[[ ! -s "$SLIDESHOW_FILE" ]] && \
		fatal "No slides found in '$SLIDESHOW_FOLDER'"
}

function startSlideshow() {
	timeout 1s xset q &> /dev/null || \
		fatal "No X server running"

	[[ ! -e "$SLIDESHOW_FILE" ]] && \
		fatal "No slideshow file found, run first with 'update' command"

	pidof sent > /dev/null && \
		fatal "Slideshow already running"

	sent "$SLIDESHOW_FILE" &
}

function killSlideshow() {
	PID="$(pidof sent)"

	[[ -z "$PID" ]] && \
		fatal "No 'sent' instance running"

	kill "$PID"
	sleep "0.5s"

	PID="$(pidof sent)"

	[[ -n "$PID" ]] && \
		fatal "Could not terminate 'sent' with pid $PID"
}

function printHelp() {
	echo -e "usage: kiltisbulletin.sh COMMAND\n"
	echo -e "Available commands:"
	echo -e "  update\tUpdates slideshow config"
	echo -e "  start\t\tStarts the slideshow"
	echo -e "  kill\t\tTerminates running slideshow"
}

COMMAND="$(echo $1 | tr '[:upper:]' '[:lower:]')"

case "$COMMAND" in
    "update")
        updateSlideshowFile
        exit
        ;;
    "start")
        startSlideshow
        exit
        ;;
    "kill")
        killSlideshow
        exit
        ;;
    *)
	printHelp
	exit 1
	;;
esac

