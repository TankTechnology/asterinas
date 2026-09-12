#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0

set -euo pipefail

umask 077

readonly CONSOLE="${ASTERINAS_DESKTOP_M9_CONSOLE:-/dev/console}"
readonly TIMEOUT_SECONDS="${ASTERINAS_DESKTOP_M9_TIMEOUT_SECONDS:-120}"
readonly COMMAND_TIMEOUT_SECONDS="${ASTERINAS_DESKTOP_M9_COMMAND_TIMEOUT_SECONDS:-120}"
readonly VIM_OUTPUT="${ASTERINAS_DESKTOP_M9_VIM_OUTPUT:-/run/asterinas-m9-vim.txt}"
# Keep the media fixture on the ext2 rootfs instead of /run's tmpfs.  This
# avoids exercising an unimplemented/undersized tmpfs path while validating
# the applications themselves.
readonly WORK_DIRECTORY="${ASTERINAS_DESKTOP_M9_WORK_DIRECTORY:-/var/tmp}"
readonly READY_MARKER="DEBIAN_DESKTOP_M9_SOFTWARE_READY vim=pass ffmpeg=pass ffprobe=pass media=rawvideo"
readonly VIDEO_PLAYER_READY_MARKER="DEBIAN_DESKTOP_M9_VIDEO_PLAYER_READY fixture=rawvideo probe=ffprobe decode=ffmpeg player=ffplay output=x11 status=pass"

failure_emitted=0
work_directory=""

emit() {
    printf '%s\n' "$1" >>"$CONSOLE"
}

fail() {
    if ((failure_emitted == 0)); then
        failure_emitted=1
        emit "DEBIAN_DESKTOP_M9_FAIL reason=$1"
    fi
    exit 1
}

fail_with_log() {
    local reason="$1"
    local log_file="${2:-}"
    local detail=""
    if [[ -n "$log_file" && -s "$log_file" ]]; then
        IFS= read -r detail <"$log_file" || true
        detail="${detail//$'\r'/ }"
        detail="${detail:0:180}"
        if [[ -n "$detail" ]]; then
            emit "DEBIAN_DESKTOP_M9_DIAG step=$reason message=$detail"
        fi
    fi
    fail "$reason"
}

cleanup() {
    if [[ -n "$work_directory" && -d "$work_directory" ]]; then
        rm -rf -- "$work_directory"
    fi
}

trap cleanup EXIT

[[ "$TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail invalid-timeout
[[ "$COMMAND_TIMEOUT_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail invalid-command-timeout
[[ "$VIM_OUTPUT" == /* && "$VIM_OUTPUT" != *$'\n'* && "$VIM_OUTPUT" != *$'\r'* ]] ||
    fail invalid-vim-output
[[ "$WORK_DIRECTORY" == /* && "$WORK_DIRECTORY" != *$'\n'* && "$WORK_DIRECTORY" != *$'\r'* ]] ||
    fail invalid-work-directory

deadline=$((SECONDS + TIMEOUT_SECONDS))
check_deadline() {
    ((SECONDS < deadline)) || fail overall-timeout
}

bounded() {
    timeout --signal=KILL "${COMMAND_TIMEOUT_SECONDS}s" "$@"
}

work_directory="$(mktemp -d "$WORK_DIRECTORY/asterinas-desktop-m9.XXXXXX")" ||
    fail work-directory
vim_output="$VIM_OUTPUT"
video_input="$work_directory/video.rgb"

check_deadline
command -v vim >/dev/null 2>&1 || fail vim-missing
rm -f -- "$vim_output"
if ! bounded vim -Nu NONE -n -es \
    -c 'call setline(1, ["ASTERINAS_VIM_PASS"])' \
    -c "wq! $vim_output"; then
    fail vim-failed
fi
[[ -f "$vim_output" && ! -L "$vim_output" ]] || fail vim-output
grep -Fxq 'ASTERINAS_VIM_PASS' "$vim_output" || fail vim-content

check_deadline
command -v ffmpeg >/dev/null 2>&1 || fail ffmpeg-missing
command -v ffprobe >/dev/null 2>&1 || fail ffprobe-missing
command -v ffplay >/dev/null 2>&1 || fail ffplay-missing

# Feed two deterministic raw RGB frames directly to the multimedia tools. This
# keeps the smoke test focused on decode/probe/playback and avoids treating a
# very slow encoder path as a prerequisite for video-player availability.
if ! bounded dd if=/dev/zero of="$video_input" bs=768 count=2 status=none; then
    fail video-input-failed
fi
[[ -s "$video_input" && ! -L "$video_input" ]] || fail video-output

check_deadline
video_ffprobe_log="$work_directory/video-ffprobe.stderr"
video_probe_output="$(bounded ffprobe -v error -f rawvideo -pixel_format rgb24 \
    -video_size 16x16 -framerate 2 -count_frames -select_streams v:0 \
    -show_entries stream=codec_name,width,height,nb_read_frames \
    -of csv=p=0 "$video_input" 2>"$video_ffprobe_log")" ||
    fail_with_log video-probe-failed "$video_ffprobe_log"
[[ "$video_probe_output" == 'rawvideo,16,16,2' ]] ||
    fail video-probe-mismatch

check_deadline
video_decode_log="$work_directory/video-decode.stderr"
if ! bounded ffmpeg -nostdin -v error -threads 1 -f rawvideo \
    -pixel_format rgb24 -video_size 16x16 -framerate 2 -i "$video_input" \
    -frames:v 2 -f null - 2>"$video_decode_log"; then
    fail_with_log video-decode-failed "$video_decode_log"
fi

check_deadline
ffplay_log="$work_directory/ffplay.stderr"
if ! bounded env DISPLAY=:0 XAUTHORITY=/home/asterinas/.Xauthority \
    SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software \
    ffplay -autoexit -an -v error -f rawvideo -pixel_format rgb24 \
        -video_size 16x16 -framerate 2 -x 64 -y 64 \
        -window_title AsterinasM9Video "$video_input" \
        </dev/null 2>"$ffplay_log"; then
    fail_with_log ffplay-failed "$ffplay_log"
fi

emit "$READY_MARKER"
emit "$VIDEO_PLAYER_READY_MARKER"
