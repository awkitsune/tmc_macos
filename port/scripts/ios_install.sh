#!/bin/sh
# Build the iOS/iPadOS app signed with your Apple ID's team and install it on a
# connected iPhone or iPad (README.md, "iPhone and iPad", has the one-time setup).
#   port/scripts/ios_install.sh [--team TEAM_ID] [--device DEVICE] [--selftest]
# The team is the one of the Apple ID added in Xcode > Settings > Accounts
# (a free account's "Personal Team"); --team picks one if there are several.
# DEVICE is a name or UDID from `xcrun devicectl list devices`; without it, the
# one connected device. With --selftest the app is then launched with
# TMC_SELFTEST=1 and its checks print here, ending in "N failed checks" (import
# the ROM in the app once first). TMC_IOS_BUNDLE_ID overrides the bundle ID (a
# free account cannot use one that another account registered).
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
build=$root/build/ios
team=
device=
selftest=no
while [ $# -gt 0 ]; do
    case $1 in
        --team) team=$2; shift 2 ;;
        --device) device=$2; shift 2 ;;
        --selftest) selftest=yes; shift ;;
        *) sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done

# The development teams of the accounts signed in to Xcode.
if [ -z "$team" ]; then
    team=$(defaults export com.apple.dt.Xcode - 2>/dev/null | python3 -c '
import plistlib, sys
try:
    prefs = plistlib.loads(sys.stdin.buffer.read())
except Exception:
    prefs = {}
teams = {}
for entries in prefs.get("IDEProvisioningTeamByIdentifier", {}).values():
    for entry in entries:
        teams[entry["teamID"]] = entry.get("teamName", "")
if len(teams) != 1:
    sys.stderr.write("ios_install: %s\n" % ("no Apple ID in Xcode: add one in Xcode > Settings > Accounts" if not teams
                     else "several teams; pick one with --team: " + ", ".join("%s (%s)" % t for t in teams.items())))
    sys.exit(1)
print(next(iter(teams)))
')
fi
bundle=${TMC_IOS_BUNDLE_ID:-io.github.awkitsune.tmc-ios}

# The connected device (its hardware UDID, which xcodebuild and devicectl both take).
devices=$(mktemp)
trap 'rm -f "$devices"' EXIT
xcrun devicectl list devices --json-output "$devices" > /dev/null
udid=$(python3 - "$devices" "$device" <<'EOF'
import json, sys
wanted = sys.argv[2]
found = []
for d in json.load(open(sys.argv[1]))["result"]["devices"]:
    hw, props = d.get("hardwareProperties", {}), d.get("deviceProperties", {})
    if hw.get("platform") != "iOS":
        continue
    names = {props.get("name"), hw.get("udid"), d.get("identifier")}
    if wanted and wanted not in names:
        continue
    if not wanted and d.get("connectionProperties", {}).get("tunnelState") == "unavailable":
        continue
    found.append((hw.get("udid"), props.get("name")))
if len(found) != 1:
    sys.stderr.write("ios_install: %s\n" % ("no connected iPhone or iPad (plug it in and unlock it)" if not found
                     else "several devices; name one: " + ", ".join(n for _, n in found)))
    sys.exit(1)
print(found[0][0])
EOF
)
echo "Building for $udid (the first build compiles the whole game: several minutes, with no output)"

cmake -S "$root/port" -B "$build" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
    -DTMC_BAKE_DATA=OFF -DTMC_IOS_TEAM="$team" -DTMC_IOS_BUNDLE_ID="$bundle" ${TMC_PYTHON:+-DTMC_PYTHON="$TMC_PYTHON"} > /dev/null
# Automatic signing makes (and renews) the free 7-day provisioning profile for this device.
xcodebuild -project "$build/tmc_native.xcodeproj" -scheme tmc_ios -configuration Release \
    -destination "id=$udid" -allowProvisioningUpdates -allowProvisioningDeviceRegistration -jobs 4 -quiet build
app=$build/Release-iphoneos/TheMinishCap.app
echo "Installing on $udid"
xcrun devicectl device install app --device "$udid" "$app"

if [ "$selftest" = yes ]; then
    xcrun devicectl device process launch --device "$udid" --terminate-existing --console \
        --environment-variables '{"TMC_SELFTEST": "1"}' "$bundle"
fi
