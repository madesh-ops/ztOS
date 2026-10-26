#!/usr/bin/env bash
# Root-only helper. Package names and APK paths are validated per action.
set -euo pipefail
[[ $EUID == 0 && $# -ge 1 ]] || exit 2
. /usr/lib/ztos/user.sh
lxc_args=(-P /var/lib/waydroid/lxc -n waydroid)
case $1 in
    audio)
        [[ $# == 1 ]] || exit 2
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    prepare)
        [[ $# == 1 ]] || exit 2
        systemctl start waydroid-container.service
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
            timeout 10 lxc-unfreeze "${lxc_args[@]}"
        fi
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    freeze)
        [[ $# == 1 ]] || exit 2
        # Never while Android is still booting: a first boot (every app
        # compiled) takes minutes, and frozen with the screen off it would
        # never finish. The next freeze request after boot applies.
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] &&
           [[ $(timeout 3 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/getprop sys.boot_completed 2>/dev/null) == 1 ]]; then
            timeout 5 lxc-freeze "${lxc_args[@]}"
        fi
        ;;
    unfreeze)
        [[ $# == 1 ]] || exit 2
        systemctl start waydroid-container.service
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
            timeout 10 lxc-unfreeze "${lxc_args[@]}"
        fi
        /usr/local/libexec/ztos-waydroid-audio
        ;;
    install)
        [[ $# == 2 ]] || exit 2
        apk=$(realpath -e -- "$2") || exit 2
        [[ $apk == "$ZTOS_HOME"/.local/share/ztos/android/apks/*.apk && -f $apk ]] || exit 2
        ztos_as_user WAYLAND_DISPLAY=wayland-0 \
            timeout 90 waydroid app install "$apk"
        ;;
    remove)
        [[ $# == 2 && $2 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || exit 2
        ztos_as_user WAYLAND_DISPLAY=wayland-0 \
            timeout 60 waydroid app remove "$2"
        ;;
    close)
        [[ $# == 2 && $2 =~ ^[a-zA-Z][a-zA-Z0-9_]*(\.[a-zA-Z0-9_]+)+$ ]] || exit 2
        # An app quit from the switcher after going back to Start finds
        # Android frozen (the idle policy): nothing in it can answer then, so
        # thaw it, stop the app, give Android a moment to take its window
        # away, and freeze it again as it was.
        frozen=false
        if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
            frozen=true
            timeout 10 lxc-unfreeze "${lxc_args[@]}"
        fi
        # Android's own cmd (a native binary) straight into the container:
        # 0.2 s, where waydroid shell and am (a Java program) took 1.4 s
        timeout 10 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- \
            /system/bin/cmd activity force-stop "$2"
        if $frozen; then
            sleep 0.5
            timeout 5 lxc-freeze "${lxc_args[@]}"
        fi
        ;;
    recover)
        [[ $# == 1 ]] || exit 2
        systemctl restart waydroid-container.service
        ;;
    ready)
        [[ $# == 1 ]] || exit 2
        deadline=$((SECONDS + 90))
        while (( SECONDS < deadline )); do
            if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == FROZEN ]]; then
                timeout 5 waydroid container unfreeze || true
            fi
            if [[ $(lxc-info "${lxc_args[@]}" -sH 2>/dev/null) == RUNNING ]] &&
               [[ $(timeout 3 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/getprop sys.boot_completed 2>/dev/null) == 1 ]] &&
               timeout 3 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/service check package 2>/dev/null | grep -q ': found$' &&
               timeout 3 lxc-attach "${lxc_args[@]}" -- /system/bin/service check waydroidplatform 2>/dev/null | grep -q ': found$'; then
                /usr/local/libexec/ztos-waydroid-audio
                exit 0
            fi
            sleep 0.5
        done
        echo 'Android failed readiness check.' >&2
        exit 1
        ;;
    *) exit 2 ;;
esac
